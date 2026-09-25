#include "serial_transfer.hpp"
#include "bluetooth_serial.hpp"
#include "xmodem.hpp"
#include "picocalc_keyboard.hpp"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <algorithm>

namespace rmb::platform {
namespace {
using Route = SerialTransferRoute;
Route command_route = Route::Auto, transfer_route = Route::Auto;
stdio_driver_t* transfer_driver = nullptr;
bool active = false, local_cancel = false;
std::uint32_t next_key_poll = 0;
const char* last_error = "SERIAL NOT AVAILABLE";

constexpr unsigned ring_size = 1024;
std::uint8_t ring[ring_size];
volatile unsigned read_pos = 0, write_pos = 0;
volatile bool overflow = false;

constexpr std::size_t bluetooth_prefetch_size = 2048;
std::uint8_t bluetooth_prefetch[bluetooth_prefetch_size];
std::size_t bluetooth_prefetch_read = 0;
std::size_t bluetooth_prefetch_count = 0;
std::uint32_t bluetooth_rx_overflow_start = 0;

void clear_bluetooth_prefetch() {
    bluetooth_prefetch_read = 0;
    bluetooth_prefetch_count = 0;
}

bool bluetooth_prefetch_ready() {
    return bluetooth_prefetch_read < bluetooth_prefetch_count;
}

void uart_received(void*) {
    char buffer[32];
    int n;
    while ((n = stdio_uart.in_chars(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) {
            const unsigned next = (write_pos + 1) % ring_size;
            if (next == read_pos) overflow = true;
            else {
                ring[write_pos] = static_cast<std::uint8_t>(buffer[i]);
                write_pos = next;
            }
        }
    }
}

int uart_buffered_read(char* data, int size) {
    const auto irq = save_and_disable_interrupts();
    int n = 0;
    while (n < size && read_pos != write_pos) {
        data[n++] = static_cast<char>(ring[read_pos]);
        read_pos = (read_pos + 1) % ring_size;
    }
    restore_interrupts(irq);
    return n;
}

Route selected_route(Route explicit_route = Route::Auto) {
    if (explicit_route != Route::Auto) return explicit_route;
    if (command_route != Route::Auto) return command_route;
    return stdio_usb_connected() ? Route::Usb : Route::Uart;
}

bool connected() {
    if (!active) return false;
    if (transfer_route == Route::Bluetooth) {
        if (!bluetooth_serial::connected() ||
            !bluetooth_serial::transfer_active()) {
            clear_bluetooth_prefetch();
            last_error = "BLUETOOTH DISCONNECTED";
            return false;
        }
        if (bluetooth_serial::rx_overflow_count() !=
            bluetooth_rx_overflow_start) {
            clear_bluetooth_prefetch();
            last_error = "BLUETOOTH RX OVERFLOW";
            return false;
        }
        return true;
    }
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

int bluetooth_buffered_read(char* data, int size) {
    int count = 0;
    while (count < size) {
        if (!bluetooth_prefetch_ready()) {
            bluetooth_prefetch_count = bluetooth_serial::read_transfer(
                bluetooth_prefetch, sizeof(bluetooth_prefetch));
            bluetooth_prefetch_read = 0;
            if (!bluetooth_prefetch_count) break;
        }
        const auto available =
            bluetooth_prefetch_count - bluetooth_prefetch_read;
        const auto wanted = static_cast<std::size_t>(size - count);
        const auto amount = std::min(available, wanted);
        for (std::size_t i = 0; i < amount; ++i)
            data[count + static_cast<int>(i)] =
                static_cast<char>(
                    bluetooth_prefetch[bluetooth_prefetch_read + i]);
        bluetooth_prefetch_read += amount;
        count += static_cast<int>(amount);
    }
    return count;
}

int raw_read(char* data, int size) {
    if (transfer_route == Route::Bluetooth)
        return bluetooth_buffered_read(data, size);
    return transfer_route == Route::Uart ? uart_buffered_read(data, size) :
           transfer_driver->in_chars(data, size);
}
}

bool serial_transfer_active() { return active; }

void console_local_input() {
    if (!active) command_route = Route::Auto;
}

void console_bluetooth_input() {
    if (!active) command_route = Route::Bluetooth;
}

int console_serial_read(unsigned timeout_us, bool same_route) {
    if (active) return -1;
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
    case Route::Bluetooth: return "BLUETOOTH SPP";
    case Route::Auto: break;
    }
    return "UART0";
}

const char* serial_transfer_error() { return last_error; }

bool begin_serial_transfer(SerialTransferRoute route) {
    if (active) {
        last_error = "SERIAL BUSY";
        return false;
    }

    clear_bluetooth_prefetch();
    transfer_route = selected_route(route);
    transfer_driver = nullptr;
    stdio_flush();

    if (transfer_route == Route::Bluetooth) {
        if (!bluetooth_serial::enabled()) {
            last_error = "BLUETOOTH OFF";
            return false;
        }
        if (!bluetooth_serial::connected()) {
            last_error = "BLUETOOTH NOT CONNECTED";
            return false;
        }
        if (!bluetooth_serial::begin_transfer()) {
            last_error = bluetooth_serial::last_error();
            return false;
        }
        bluetooth_rx_overflow_start =
            bluetooth_serial::rx_overflow_count();
    } else {
        transfer_driver =
            transfer_route == Route::Usb ? &stdio_usb : &stdio_uart;
        if (transfer_route == Route::Usb && !stdio_usb_connected()) {
            last_error = "USB DISCONNECTED";
            return false;
        }
        if (transfer_route == Route::Uart &&
            !stdio_uart.set_chars_available_callback) {
            last_error = "UART BUFFER NOT AVAILABLE";
            return false;
        }
    }

    active = true;
    local_cancel = false;
    next_key_poll = 0;
    read_pos = write_pos = 0;
    overflow = false;
    last_error = transfer_route == Route::Bluetooth
        ? "BLUETOOTH CONNECTION LOST" : "SERIAL CONNECTION LOST";

    if (transfer_route != Route::Bluetooth) {
        stdio_set_driver_enabled(transfer_driver, false);
        if (transfer_route == Route::Uart)
            stdio_uart.set_chars_available_callback(uart_received, nullptr);
    }
    return true;
}

int serial_transfer_read(unsigned timeout_ms) {
    // RFCOMM/Windows scheduling occasionally inserts a >1 s gap between
    // YMODEM bytes even though the link remains healthy. Keep the short
    // 250 ms EOT grace window exact, but give normal packet/header reads
    // enough Bluetooth-specific tolerance to avoid spurious TIMEOUT.
    unsigned effective_timeout_ms = timeout_ms;
    if (transfer_route == Route::Bluetooth &&
        timeout_ms >= 1000 && timeout_ms < 4000) {
        effective_timeout_ms = 4000;
    }
    const auto deadline = make_timeout_time_ms(effective_timeout_ms);
    do {
        if (local_cancel) {
            clear_bluetooth_prefetch();
            return xmodem::cancelled;
        }

        const auto now = to_ms_since_boot(get_absolute_time());
        if (static_cast<std::int32_t>(now - next_key_poll) >= 0) {
            next_key_poll = now + 100;
            const int key = picocalc::keyboard::read_key();
            if (key == 3 || key == 0xb1 || key == 0xd0) {
                local_cancel = true;
                clear_bluetooth_prefetch();
                return xmodem::cancelled;
            }
        }

        char c;
        if (transfer_route == Route::Bluetooth) {
            // An overflow destroys packet framing even if prefetched bytes are
            // still available. Detect it before returning any more protocol
            // data so YMODEM aborts immediately instead of retrying garbage.
            if (!connected()) return xmodem::disconnected;
            if (bluetooth_prefetch_ready() && raw_read(&c, 1) == 1)
                return static_cast<unsigned char>(c);

            // Refill and link checks occur only when the local cache is empty.
            // A 1K YMODEM packet therefore needs only a few CYW43 critical
            // sections rather than one critical section per protocol byte.
            bluetooth_serial::service();
            if (!connected()) return xmodem::disconnected;
            if (raw_read(&c, 1) == 1)
                return static_cast<unsigned char>(c);
        } else {
            if (!connected()) return xmodem::disconnected;
            if (raw_read(&c, 1) == 1)
                return static_cast<unsigned char>(c);
        }
        sleep_ms(1);
    } while (!time_reached(deadline));
    return xmodem::timeout;
}

bool serial_transfer_write(const std::uint8_t* data, std::size_t size) {
    if (!connected()) return false;
    if (transfer_route == Route::Bluetooth) {
        if (!bluetooth_serial::write_transfer(data, size)) {
            last_error = bluetooth_serial::last_error();
            return false;
        }
        return connected();
    }
    if (transfer_route == Route::Uart)
        uart_write_blocking(uart_default, data, size);
    else
        transfer_driver->out_chars(
            reinterpret_cast<const char*>(data), static_cast<int>(size));
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
        if (transfer_route == Route::Bluetooth)
            bluetooth_serial::service();
        sleep_ms(1);
    }

    if (transfer_route == Route::Bluetooth) {
        bluetooth_serial::end_transfer();
    } else {
        if (transfer_route == Route::Uart)
            stdio_uart.set_chars_available_callback(nullptr, nullptr);
        stdio_set_driver_enabled(transfer_driver, true);
    }
    clear_bluetooth_prefetch();
    transfer_driver = nullptr;
    active = false;
}

} // namespace rmb::platform
