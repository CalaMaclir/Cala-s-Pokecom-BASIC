#include "serial_transfer.hpp"
#include "xmodem.hpp"
#include "picocalc_keyboard.hpp"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

namespace rmb::platform {
namespace {
enum class Route { Auto, Usb, Uart };
Route command_route = Route::Auto, transfer_route = Route::Auto;
stdio_driver_t* transfer_driver = nullptr;
bool active = false, local_cancel = false;
std::uint32_t next_key_poll = 0;
const char* last_error = "SERIAL NOT AVAILABLE";

// Buffer UART packets while the foreground polls I2C or flushes SD.
constexpr unsigned ring_size = 1024;
std::uint8_t ring[ring_size];
volatile unsigned read_pos = 0, write_pos = 0;
volatile bool overflow = false;
void uart_received(void*) {
    char buffer[32];
    int n;
    while ((n = stdio_uart.in_chars(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; ++i) {
            unsigned next = (write_pos + 1) % ring_size;
            if (next == read_pos) overflow = true;
            else { ring[write_pos] = static_cast<std::uint8_t>(buffer[i]); write_pos = next; }
        }
    }
}
int uart_buffered_read(char* data, int size) {
    auto irq = save_and_disable_interrupts();
    int n = 0;
    while (n < size && read_pos != write_pos) {
        data[n++] = static_cast<char>(ring[read_pos]);
        read_pos = (read_pos + 1) % ring_size;
    }
    restore_interrupts(irq);
    return n;
}
Route selected_route() {
    if (command_route != Route::Auto) return command_route;
    return stdio_usb_connected() ? Route::Usb : Route::Uart;
}
bool connected() {
    if (!active) return false;
    if (overflow) { last_error = "UART RX OVERFLOW"; return false; }
    if (transfer_route == Route::Usb && !stdio_usb_connected()) {
        last_error = "USB DISCONNECTED"; return false;
    }
    // UART bridges expose no native USB DTR: use the protocol timeout.
    return true;
}
int raw_read(char* data, int size) {
    return transfer_route == Route::Uart ? uart_buffered_read(data, size) :
           transfer_driver->in_chars(data, size);
}
}
bool serial_transfer_active() { return active; }
void console_local_input() { command_route = Route::Auto; }
int console_serial_read(unsigned timeout_us, bool same_route) {
    if (active) return -1;
    auto deadline = make_timeout_time_us(timeout_us);
    do {
        char c;
        if ((!same_route || command_route != Route::Uart) && stdio_usb_connected() &&
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
const char* serial_transfer_route_name() {
    return selected_route() == Route::Usb ? "USB CDC" : "UART";
}
const char* serial_transfer_error() { return last_error; }
bool begin_serial_transfer() {
    if (active) { last_error = "SERIAL BUSY"; return false; }
    transfer_route = selected_route();
    transfer_driver = transfer_route == Route::Usb ? &stdio_usb : &stdio_uart;
    if (transfer_route == Route::Usb && !stdio_usb_connected()) {
        last_error = "USB DISCONNECTED"; return false;
    }
    if (transfer_route == Route::Uart && !stdio_uart.set_chars_available_callback) {
        last_error = "UART BUFFER NOT AVAILABLE"; return false;
    }
    stdio_flush();
    active = true;
    local_cancel = false;
    next_key_poll = 0;
    read_pos = write_pos = 0;
    overflow = false;
    last_error = "SERIAL CONNECTION LOST";
    // Bypass stdio fan-out and CRLF conversion. Only the selected driver is
    // removed; normal routing is restored on every exit.
    stdio_set_driver_enabled(transfer_driver, false);
    if (transfer_route == Route::Uart)
        stdio_uart.set_chars_available_callback(uart_received, nullptr);
    return true;
}
int serial_transfer_read(unsigned timeout_ms) {
    auto deadline = make_timeout_time_ms(timeout_ms);
    do {
        if (!connected()) return xmodem::disconnected;
        if (local_cancel) return xmodem::cancelled;
        auto now = to_ms_since_boot(get_absolute_time());
        if (static_cast<std::int32_t>(now - next_key_poll) >= 0) {
            next_key_poll = now + 100;
            int key = picocalc::keyboard::read_key();
            if (key == 3 || key == 0xb1 || key == 0xd0) {
                local_cancel = true;
                return xmodem::cancelled;
            }
        }
        char c;
        if (raw_read(&c, 1) == 1) return static_cast<unsigned char>(c);
        sleep_ms(1);
    } while (!time_reached(deadline));
    return xmodem::timeout;
}
bool serial_transfer_write(const std::uint8_t* data, std::size_t size) {
    if (!connected()) return false;
    if (transfer_route == Route::Uart)
        uart_write_blocking(uart_default, data, size); // no UART-level CRLF conversion either
    else
        transfer_driver->out_chars(reinterpret_cast<const char*>(data), static_cast<int>(size));
    if (transfer_driver->out_flush) transfer_driver->out_flush();
    return connected();
}
void end_serial_transfer() {
    if (!active) return;
    auto deadline = make_timeout_time_ms(1000);
    auto quiet = make_timeout_time_ms(200);
    while (!time_reached(deadline) && !time_reached(quiet)) {
        char buffer[64];
        if (raw_read(buffer, sizeof(buffer)) > 0) quiet = make_timeout_time_ms(200);
        sleep_ms(1);
    }
    if (transfer_route == Route::Uart)
        stdio_uart.set_chars_available_callback(nullptr, nullptr);
    stdio_set_driver_enabled(transfer_driver, true);
    active = false;
}
}
