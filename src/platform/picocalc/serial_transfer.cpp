#include "serial_transfer.hpp"
#include "xmodem.hpp"
#include "picocalc_keyboard.hpp"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

#if __has_include("hardware/dma.h") && __has_include("hardware/irq.h") && \
    __has_include("hardware/uart.h") && defined(PICO_RP2350) && PICO_RP2350
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#define RMB_HAVE_UART_DMA 1
#else
#define RMB_HAVE_UART_DMA 0
#endif

#include <algorithm>
#include <climits>
#include <cstring>

namespace rmb::platform {
namespace {
using Route = SerialTransferRoute;
Route command_route = Route::Auto, transfer_route = Route::Auto;
stdio_driver_t* transfer_driver = nullptr;
bool active = false, local_cancel = false;
bool diagnostic_active = false;
std::uint32_t next_key_poll = 0;
const char* last_error = "SERIAL NOT AVAILABLE";
SerialTransferPerformance performance;

// One internal-SRAM ring is shared by the mutually exclusive IRQ and DMA
// UART receive paths. Power-of-two sizing and alignment permit RP2350 DMA
// write-address wrapping without a second hot buffer.
constexpr std::uint32_t ring_size = 4096;
constexpr std::uint32_t ring_mask = ring_size - 1;
alignas(ring_size) std::uint8_t ring[ring_size];
volatile std::uint32_t irq_produced = 0, irq_consumed = 0;
volatile bool overflow = false;

#if RMB_HAVE_UART_DMA
int dma_channel = -1;
volatile std::uint64_t dma_completed = 0;
std::uint64_t dma_consumed = 0;

// One self-triggered transfer covers exactly one ring. RP2350 reloads it in
// hardware with no 28-bit normal-count ceiling or re-arm gap. The completion
// IRQ extends the current in-ring position into a monotonic 64-bit producer.
constexpr std::uint32_t dma_transfer_count = ring_size;

void uart_dma_irq() {
    if (dma_channel < 0 || !dma_channel_get_irq1_status(dma_channel)) return;
    dma_channel_acknowledge_irq1(dma_channel);
    dma_completed += dma_transfer_count;
}
#endif

bool poll_local_cancel() {
    if (local_cancel) return true;
    const auto now = to_ms_since_boot(get_absolute_time());
    if (static_cast<std::int32_t>(now - next_key_poll) < 0) return false;
    next_key_poll = now + 25;
    const int key = picocalc::keyboard::read_key();
    if (key == 3 || key == 0xb1 || key == 0xd0) local_cancel = true;
    return local_cancel;
}

void uart_received(void*) {
    char buffer[64];
    int n;
    while ((n = stdio_uart.in_chars(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) {
            if (irq_produced - irq_consumed >= ring_size) {
                overflow = true;
                return;
            }
            ring[irq_produced & ring_mask] =
                static_cast<std::uint8_t>(buffer[i]);
            ++irq_produced;
        }
    }
}

int uart_irq_read(char* data, int size) {
    const auto irq = save_and_disable_interrupts();
    const std::uint32_t available = irq_produced - irq_consumed;
    const int count = std::min<int>(size, static_cast<int>(available));
    for (int i = 0; i < count; ++i)
        data[i] = static_cast<char>(ring[(irq_consumed + i) & ring_mask]);
    irq_consumed += static_cast<std::uint32_t>(count);
    restore_interrupts(irq);
    return count;
}

#if RMB_HAVE_UART_DMA
bool start_uart_dma() {
    dma_channel = dma_claim_unused_channel(false);
    if (dma_channel < 0) return false;

    dma_channel_config config = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_ring(&config, true, 12); // 2^12 = 4096 bytes
    channel_config_set_dreq(&config, uart_get_dreq(uart_default, false));
    dma_completed = 0;
    dma_consumed = 0;
    dma_channel_acknowledge_irq1(dma_channel);
    irq_add_shared_handler(
        DMA_IRQ_1, uart_dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    dma_channel_set_irq1_enabled(dma_channel, true);
    dma_channel_configure(
        dma_channel,
        &config,
        ring,
        &uart_get_hw(uart_default)->dr,
        dma_encode_transfer_count_with_self_trigger(dma_transfer_count),
        true
    );
    return true;
}

std::uint64_t dma_produced() {
    if (dma_channel < 0) return 0;
    const auto irq = save_and_disable_interrupts();
    // Fold a pending completion into the base before sampling the reloaded
    // counter. This also makes polling correct if the shared IRQ was delayed.
    uart_dma_irq();
    const std::uint64_t completed = dma_completed;
    const std::uint32_t remaining =
        dma_channel_hw_addr(dma_channel)->transfer_count &
        DMA_CH0_TRANS_COUNT_COUNT_BITS;
    const std::uint64_t produced =
        completed + (dma_transfer_count - remaining);
    restore_interrupts(irq);
    return produced;
}

int uart_dma_read(char* data, int size) {
    const std::uint64_t produced = dma_produced();
    const std::uint64_t available = produced - dma_consumed;
    if (available > ring_size) {
        overflow = true;
        return 0;
    }
    const int count = std::min<int>(size, static_cast<int>(available));
    const std::uint32_t start =
        static_cast<std::uint32_t>(dma_consumed) & ring_mask;
    const int first = std::min<int>(count, ring_size - start);
    if (first) std::memcpy(data, ring + start, static_cast<std::size_t>(first));
    if (count > first)
        std::memcpy(data + first, ring, static_cast<std::size_t>(count - first));
    // DMA remains live while copying. Reject this chunk if it lapped the
    // consumer after the first snapshot, rather than returning overwritten
    // bytes to the protocol core.
    if (dma_produced() - dma_consumed > ring_size) {
        overflow = true;
        return 0;
    }
    dma_consumed += static_cast<std::uint32_t>(count);
    return count;
}

void stop_uart_dma() {
    if (dma_channel < 0) return;
    dma_channel_set_irq1_enabled(dma_channel, false);
    irq_remove_handler(DMA_IRQ_1, uart_dma_irq);
    dma_channel_abort(dma_channel);
    dma_channel_acknowledge_irq1(dma_channel);
    dma_channel_unclaim(dma_channel);
    dma_channel = -1;
    dma_completed = 0;
    dma_consumed = 0;
}
#endif

int uart_buffered_read(char* data, int size) {
#if RMB_HAVE_UART_DMA
    if (performance.uart_rx_mode == UartRxMode::Dma)
        return uart_dma_read(data, size);
#endif
    return uart_irq_read(data, size);
}

Route selected_route(Route explicit_route = Route::Auto) {
    if (explicit_route != Route::Auto) return explicit_route;
    if (command_route != Route::Auto) return command_route;
    return stdio_usb_connected() ? Route::Usb : Route::Uart;
}

bool connected() {
    if (!active) return false;
    if (transfer_route == Route::Uart && overflow) {
        last_error = "UART RX OVERFLOW";
        return false;
    }
    if (transfer_route == Route::Usb && !stdio_usb_connected()) {
        last_error = "USB DISCONNECTED";
        return false;
    }
    return true;
}

int raw_read(char* data, int size) {
    if (transfer_route == Route::Uart) return uart_buffered_read(data, size);
    if (!performance.usb_cdc_bulk && size > 1) size = 1;
    return transfer_driver->in_chars(data, size);
}

void restore_uart() {
    if (transfer_route != Route::Uart) return;
#if RMB_HAVE_UART_DMA
    stop_uart_dma();
    uart_set_irq_enables(uart_default, false, false);
    uart_set_baudrate(uart_default, PICO_DEFAULT_UART_BAUD_RATE);
#endif
    if (stdio_uart.set_chars_available_callback)
        stdio_uart.set_chars_available_callback(nullptr, nullptr);
}
}

bool serial_transfer_active() { return active || diagnostic_active; }
bool usb_cdc_ready() { return stdio_usb_connected(); }
DiagnosticSerialResult begin_usb_diagnostic() {
    if (serial_transfer_active()) return DiagnosticSerialResult::Busy;
    if (!usb_cdc_ready()) return DiagnosticSerialResult::Unavailable;
    diagnostic_active = true;
    return DiagnosticSerialResult::Ready;
}
bool write_usb_diagnostic(const char* text) {
    if (!diagnostic_active || active || !text || !usb_cdc_ready()) return false;
    stdio_usb.out_chars(text, static_cast<int>(std::strlen(text)));
    return usb_cdc_ready();
}
void end_usb_diagnostic() { diagnostic_active = false; }


const SerialTransferPerformance& serial_transfer_performance() {
    return performance;
}

void set_ymodem_rx_bulk(bool enabled) { performance.ymodem_rx_bulk = enabled; }
void set_tx_packet_coalesce(bool enabled) {
    performance.tx_packet_coalesce = enabled;
}
void set_usb_cdc_bulk(bool enabled) { performance.usb_cdc_bulk = enabled; }
bool set_uart_transfer_baud(std::uint32_t baud) {
    if (baud != 115200 && baud != 230400 && baud != 460800 && baud != 921600)
        return false;
    performance.uart_baud = baud;
    return true;
}
void set_uart_rx_mode(UartRxMode mode) { performance.uart_rx_mode = mode; }
const char* uart_rx_mode_name(UartRxMode mode) {
    return mode == UartRxMode::Dma ? "DMA" : "IRQ";
}

void console_local_input() {
    if (!active) command_route = Route::Auto;
}

int console_serial_read(unsigned timeout_us, bool same_route) {
    if (serial_transfer_active()) return -1;
    const auto deadline = make_timeout_time_us(timeout_us);
    do {
        char c;
        if ((!same_route || command_route != Route::Uart) &&
            stdio_usb_connected() &&
            stdio_usb.in_chars(&c, 1) == 1) {
            command_route = Route::Usb;
            return static_cast<unsigned char>(c);
        }
        if ((!same_route || command_route != Route::Usb) &&
            stdio_uart.in_chars(&c, 1) == 1) {
            command_route = Route::Uart;
            return static_cast<unsigned char>(c);
        }
        if (timeout_us) sleep_us(50);
    } while (!time_reached(deadline));
    return -1;
}

const char* serial_transfer_route_name(SerialTransferRoute route) {
    switch (selected_route(route)) {
    case Route::Usb: return "USB CDC";
    case Route::Uart: return "UART0";
    case Route::Auto: break;
    }
    return "UART0";
}

const char* serial_transfer_error() { return last_error; }

bool begin_serial_transfer(SerialTransferRoute route) {
    if (serial_transfer_active()) {
        last_error = "SERIAL BUSY";
        return false;
    }

    transfer_route = selected_route(route);
    transfer_driver = transfer_route == Route::Usb ? &stdio_usb : &stdio_uart;
    stdio_flush();
    if (transfer_route == Route::Usb && !stdio_usb_connected()) {
        last_error = "USB DISCONNECTED";
        transfer_driver = nullptr;
        return false;
    }
    if (transfer_route == Route::Uart &&
        !stdio_uart.set_chars_available_callback) {
        last_error = "UART BUFFER NOT AVAILABLE";
        transfer_driver = nullptr;
        return false;
    }

    active = true;
    local_cancel = false;
    next_key_poll = 0;
    irq_produced = irq_consumed = 0;
    overflow = false;
    last_error = "SERIAL CONNECTION LOST";
    stdio_set_driver_enabled(transfer_driver, false);

    if (transfer_route == Route::Uart) {
#if RMB_HAVE_UART_DMA
        const std::uint32_t actual = uart_set_baudrate(
            uart_default, performance.uart_baud);
        const std::uint32_t difference = actual > performance.uart_baud
            ? actual - performance.uart_baud
            : performance.uart_baud - actual;
        if (difference > performance.uart_baud / 100) {
            last_error = "UART BAUD UNSUPPORTED";
            restore_uart();
            stdio_set_driver_enabled(transfer_driver, true);
            transfer_driver = nullptr;
            active = false;
            return false;
        }
        if (performance.uart_rx_mode == UartRxMode::Dma) {
            stdio_uart.set_chars_available_callback(nullptr, nullptr);
            uart_set_irq_enables(uart_default, false, false);
            if (!start_uart_dma()) {
                last_error = "UART DMA UNAVAILABLE";
                restore_uart();
                stdio_set_driver_enabled(transfer_driver, true);
                transfer_driver = nullptr;
                active = false;
                return false;
            }
        } else {
            stdio_uart.set_chars_available_callback(uart_received, nullptr);
        }
#else
        if (performance.uart_rx_mode == UartRxMode::Dma) {
            last_error = "UART DMA UNAVAILABLE";
            stdio_set_driver_enabled(transfer_driver, true);
            transfer_driver = nullptr;
            active = false;
            return false;
        }
        stdio_uart.set_chars_available_callback(uart_received, nullptr);
#endif
    }
    return true;
}

int serial_transfer_read_some(
    std::uint8_t* data, std::size_t size, unsigned timeout_ms
) {
    if (!data || size == 0) return 0;
    const auto deadline = make_timeout_time_ms(timeout_ms);
    do {
        if (poll_local_cancel()) return xmodem::cancelled;
        if (!connected()) return xmodem::disconnected;
        const int wanted = static_cast<int>(
            std::min<std::size_t>(size, static_cast<std::size_t>(INT_MAX)));
        const int count = raw_read(reinterpret_cast<char*>(data), wanted);
        if (count > 0) return count;
        if (!timeout_ms) return 0;
        sleep_ms(1);
    } while (!time_reached(deadline));
    return xmodem::timeout;
}

int serial_transfer_read_exact(
    std::uint8_t* data, std::size_t size, unsigned timeout_ms
) {
    if (!data && size) return xmodem::disconnected;
    const auto deadline = make_timeout_time_ms(timeout_ms);
    std::size_t received = 0;
    while (received < size) {
        if (poll_local_cancel()) return xmodem::cancelled;
        if (!connected()) return xmodem::disconnected;
        const int wanted = static_cast<int>(std::min<std::size_t>(
            size - received, static_cast<std::size_t>(INT_MAX)));
        const int count = raw_read(
            reinterpret_cast<char*>(data + received), wanted);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (time_reached(deadline)) return xmodem::timeout;
        sleep_ms(1);
    }
    return static_cast<int>(received);
}

int serial_transfer_read(unsigned timeout_ms) {
    std::uint8_t byte = 0;
    const int count = serial_transfer_read_exact(&byte, 1, timeout_ms);
    return count == 1 ? byte : count;
}

bool serial_transfer_write(const std::uint8_t* data, std::size_t size) {
    if (!connected()) return false;
    if (transfer_route == Route::Uart)
        uart_write_blocking(uart_default, data, size);
    else
        transfer_driver->out_chars(
            reinterpret_cast<const char*>(data), static_cast<int>(size));
    // Preserve the existing flush contract. Packet coalescing naturally turns
    // three YMODEM writes/flushes into one without independent flush changes.
    if (transfer_driver->out_flush) transfer_driver->out_flush();
    return connected();
}

void end_serial_transfer() {
    if (!active) return;
    const auto deadline = make_timeout_time_ms(1000);
    auto quiet = make_timeout_time_ms(200);
    while (!time_reached(deadline) && !time_reached(quiet)) {
        char buffer[64];
        if (raw_read(buffer, sizeof(buffer)) > 0)
            quiet = make_timeout_time_ms(200);
        sleep_ms(1);
    }

    restore_uart();
    stdio_set_driver_enabled(transfer_driver, true);
    transfer_driver = nullptr;
    active = false;
    local_cancel = false;
    irq_produced = irq_consumed = 0;
    overflow = false;
}

} // namespace rmb::platform
