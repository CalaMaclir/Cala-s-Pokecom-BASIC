#include "serial_transfer.hpp"
#include "serial_crlf_filter.hpp"
#include "bluetooth_serial.hpp"
#include "xmodem.hpp"
#include "mock_sdk.hpp"
#include <cassert>
#include <deque>
#include <vector>
#include <cstring>
#include <cstdio>

std::uint64_t test_time = 0;
bool usb_connected = false, usb_enabled = true, uart_enabled = true;
std::deque<unsigned char> usb_input, uart_input, bluetooth_input;
std::vector<unsigned char> usb_output, uart_output, bluetooth_output;
void (*uart_callback)(void*) = nullptr;
void* uart_context = nullptr;
int local_key = -1;
bool bluetooth_on = true, bluetooth_link = true, bluetooth_console = true;
bool bluetooth_transfer = false, bluetooth_write_allowed = true;
int bluetooth_service_calls = 0;
std::size_t bluetooth_bulk_read_calls = 0;
std::uint32_t bluetooth_rx_overflow = 0;

int read_queue(std::deque<unsigned char>& q, char* b, int size) {
    int n = 0;
    while (n < size && !q.empty()) {
        b[n++] = static_cast<char>(q.front());
        q.pop_front();
    }
    return n;
}
void flush() {}
stdio_driver_t stdio_usb {
    [](const char* b,int n) {usb_output.insert(usb_output.end(),b,b+n);}, flush,
    [](char* b,int n) {return read_queue(usb_input,b,n);}, nullptr
};
stdio_driver_t stdio_uart {
    [](const char* b,int n) {uart_output.insert(uart_output.end(),b,b+n);}, flush,
    [](char* b,int n) {return read_queue(uart_input,b,n);},
    [](void(*cb)(void*),void* ctx) {uart_callback=cb; uart_context=ctx;}
};
void stdio_flush() {}
void stdio_set_driver_enabled(stdio_driver_t* p,bool b) {
    if(p==&stdio_usb) usb_enabled=b; else uart_enabled=b;
}
bool stdio_usb_connected() {return usb_connected;}
void sleep_us(unsigned n) {
    test_time += n;
    if (uart_callback && !uart_input.empty()) uart_callback(uart_context);
}
void sleep_ms(unsigned n) {sleep_us(n*1000);}
void uart_write_blocking(int, const std::uint8_t* b, std::size_t n) {
    uart_output.insert(uart_output.end(),b,b+n);
}
namespace rmb::picocalc::keyboard { int read_key() {return local_key;} }

namespace rmb::bluetooth_serial {
bool enabled() { return bluetooth_on; }
bool connected() { return bluetooth_link; }
bool console_enabled() { return bluetooth_console; }
bool begin_transfer() {
    if (!bluetooth_on || !bluetooth_link || bluetooth_transfer) return false;
    bluetooth_input.clear();
    bluetooth_transfer = true;
    return true;
}
void end_transfer() { bluetooth_input.clear(); bluetooth_transfer = false; }
bool transfer_active() { return bluetooth_transfer; }
std::size_t read_transfer(std::uint8_t* destination,
                          std::size_t capacity) {
    ++bluetooth_bulk_read_calls;
    if (!destination || !capacity || !bluetooth_transfer ||
        !bluetooth_link) return 0;
    std::size_t count = 0;
    while (count < capacity && !bluetooth_input.empty()) {
        destination[count++] = bluetooth_input.front();
        bluetooth_input.pop_front();
    }
    return count;
}
int read_transfer() {
    std::uint8_t value = 0;
    return read_transfer(&value, 1) == 1
        ? static_cast<int>(value) : -1;
}
bool write_transfer(const std::uint8_t* data,std::size_t size) {
    if (!bluetooth_transfer || !bluetooth_link || !bluetooth_write_allowed) return false;
    bluetooth_output.insert(bluetooth_output.end(),data,data+size);
    return true;
}
void service() { ++bluetooth_service_calls; }
const char* last_error() {
    return bluetooth_link ? "TRANSFER TX BACKPRESSURE" : "BLUETOOTH DISCONNECTED";
}
std::uint32_t rx_overflow_count() { return bluetooth_rx_overflow; }
}

int main() {
    using namespace rmb::platform;
    detail::SerialCrLfFilter crlf;
    assert(!crlf.should_ignore('\r'));
    assert(crlf.should_ignore('\n'));
    assert(!crlf.should_ignore('A'));
    assert(!crlf.should_ignore('\n'));

    console_local_input();
    assert(std::strcmp(serial_transfer_route_name(), "UART0") == 0);
    usb_connected = true;
    assert(std::strcmp(serial_transfer_route_name(), "USB CDC") == 0);
    usb_connected = false;

    uart_input.push_back('\r');
    assert(console_serial_read(0) == '\r');
    assert(std::strcmp(serial_transfer_route_name(), "UART0") == 0);
    assert(begin_serial_transfer());
    assert(!uart_enabled && usb_enabled && uart_callback);
    for (int i = 0; i < 256; ++i) uart_input.push_back(i);
    sleep_ms(10);
    for (int i = 0; i < 256; ++i) assert(serial_transfer_read(5) == i);
    const std::uint8_t bytes[] = {
        0,1,2,3,4,6,0x15,0x18,0x1a,0x7f,0x80,0xff
    };
    assert(serial_transfer_write(bytes, sizeof(bytes)));
    assert(uart_output == std::vector<unsigned char>(bytes, bytes + sizeof(bytes)));
    local_key = 0xb1;
    sleep_ms(100);
    assert(serial_transfer_read(1) == rmb::xmodem::cancelled);
    end_serial_transfer();
    local_key = -1;
    assert(uart_enabled && usb_enabled && !uart_callback);

    assert(std::strcmp(
        serial_transfer_route_name(SerialTransferRoute::Usb), "USB CDC") == 0);
    assert(!begin_serial_transfer(SerialTransferRoute::Usb));
    assert(std::strcmp(serial_transfer_error(), "USB DISCONNECTED") == 0);
    assert(uart_enabled && usb_enabled && !uart_callback);

    usb_connected = true;
    assert(std::strcmp(
        serial_transfer_route_name(SerialTransferRoute::Uart), "UART0") == 0);
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    assert(!uart_enabled && usb_enabled && uart_callback);
    end_serial_transfer();

    usb_input.push_back('!');
    assert(console_serial_read(0) == '!');
    assert(std::strcmp(serial_transfer_route_name(), "USB CDC") == 0);
    assert(begin_serial_transfer());
    assert(serial_transfer_write(bytes, sizeof(bytes)));
    usb_connected = false;
    assert(serial_transfer_read(5) == rmb::xmodem::disconnected);
    end_serial_transfer();
    assert(!begin_serial_transfer());

    bluetooth_console = false;
    assert(std::strcmp(
        serial_transfer_route_name(SerialTransferRoute::Bluetooth),
        "BLUETOOTH SPP") == 0);
    bluetooth_input.push_back('x');
    assert(begin_serial_transfer(SerialTransferRoute::Bluetooth));
    assert(bluetooth_transfer && console_serial_read(0) == -1);
    for (std::uint8_t value : bytes) bluetooth_input.push_back(value);
    for (std::uint8_t value : bytes)
        assert(serial_transfer_read(5) == static_cast<int>(value));
    assert(bluetooth_service_calls > 0);

    // The protocol still reads one byte at a time, but Bluetooth is drained
    // into the 2 KiB prefetch cache in very few bulk calls.
    bluetooth_bulk_read_calls = 0;
    std::vector<std::uint8_t> ymodem_frame(1029);
    for (std::size_t i = 0; i < ymodem_frame.size(); ++i) {
        ymodem_frame[i] = static_cast<std::uint8_t>(i & 0xffu);
        bluetooth_input.push_back(ymodem_frame[i]);
    }
    for (std::uint8_t value : ymodem_frame)
        assert(serial_transfer_read(5) == static_cast<int>(value));
    assert(bluetooth_bulk_read_calls <= 1);

    bluetooth_bulk_read_calls = 0;
    std::vector<std::uint8_t> sustained(5 * 1029);
    for (std::size_t i = 0; i < sustained.size(); ++i) {
        sustained[i] = static_cast<std::uint8_t>((i * 37u) & 0xffu);
        bluetooth_input.push_back(sustained[i]);
    }
    for (std::uint8_t value : sustained)
        assert(serial_transfer_read(5) == static_cast<int>(value));
    assert(bluetooth_bulk_read_calls <= 3);
    assert(serial_transfer_write(bytes, sizeof(bytes)));

    // Any RX overflow invalidates YMODEM packet boundaries. The transport
    // must fail immediately rather than feeding corrupted prefetched bytes
    // to the protocol until it eventually times out.
    bluetooth_rx_overflow = 1;
    assert(serial_transfer_read(5) == rmb::xmodem::disconnected);
    assert(std::strcmp(
        serial_transfer_error(), "BLUETOOTH RX OVERFLOW") == 0);
    bluetooth_rx_overflow = 0;
    end_serial_transfer();
    assert(!bluetooth_transfer);
    assert(begin_serial_transfer(SerialTransferRoute::Bluetooth));

    assert(bluetooth_output ==
        std::vector<unsigned char>(bytes, bytes + sizeof(bytes)));

    bluetooth_write_allowed = false;
    assert(!serial_transfer_write(bytes, sizeof(bytes)));
    bluetooth_write_allowed = true;
    bluetooth_link = false;
    assert(serial_transfer_read(5) == rmb::xmodem::disconnected);
    end_serial_transfer();
    assert(!bluetooth_transfer);

    assert(!begin_serial_transfer(SerialTransferRoute::Bluetooth));
    assert(std::strcmp(
        serial_transfer_error(), "BLUETOOTH NOT CONNECTED") == 0);
    bluetooth_link = true;
    bluetooth_on = false;
    assert(!begin_serial_transfer(SerialTransferRoute::Bluetooth));
    assert(std::strcmp(serial_transfer_error(), "BLUETOOTH OFF") == 0);
    bluetooth_on = true;

    bluetooth_console = true;
    console_bluetooth_input();
    assert(std::strcmp(serial_transfer_route_name(), "BLUETOOTH SPP") == 0);
    assert(begin_serial_transfer());
    end_serial_transfer();

    usb_connected = true;
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    end_serial_transfer();
    assert(std::strcmp(serial_transfer_route_name(), "BLUETOOTH SPP") == 0);

    console_local_input();
    assert(std::strcmp(serial_transfer_route_name(), "USB CDC") == 0);

    std::puts(
        "AUTO/explicit USB/UART/Bluetooth routing, binary I/O, ownership and recovery tests passed"
    );
}
