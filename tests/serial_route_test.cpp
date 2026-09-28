#include "serial_transfer.hpp"
#include "serial_crlf_filter.hpp"
#include "xmodem.hpp"
#include "mock_sdk.hpp"
#include <cassert>
#include <deque>
#include <vector>
#include <cstring>
#include <cstdio>

std::uint64_t test_time = 0;
bool usb_connected = false, usb_enabled = true, uart_enabled = true;
std::deque<unsigned char> usb_input, uart_input;
std::vector<unsigned char> usb_output, uart_output;
void (*uart_callback)(void*) = nullptr;
void* uart_context = nullptr;
int local_key = -1;

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

    usb_connected = true;
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    end_serial_transfer();
    assert(std::strcmp(serial_transfer_route_name(), "USB CDC") == 0);

    console_local_input();
    assert(std::strcmp(serial_transfer_route_name(), "USB CDC") == 0);

    std::puts(
        "AUTO/explicit USB/UART routing, binary I/O, ownership and recovery tests passed"
    );
}
