#include "serial_transfer.hpp"
#include "serial_crlf_filter.hpp"
#include "xmodem.hpp"
#include "mock_sdk.hpp"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include <cassert>
#include <deque>
#include <vector>
#include <cstring>
#include <cstdio>
#include <climits>
#include <string>

std::uint64_t test_time = 0;
bool usb_connected = false, usb_enabled = true, uart_enabled = true;
std::deque<unsigned char> usb_input, uart_input;
std::vector<unsigned char> usb_output, uart_output;
void (*uart_callback)(void*) = nullptr;
void* uart_context = nullptr;
int local_key = -1;
int usb_chunk_limit = INT_MAX;
bool usb_disconnect_after_read = false;
bool usb_cancel_after_read = false;
bool mock_dma_claim_available = false, mock_dma_claimed = false;
bool mock_dma_active = false, mock_dma_irq1_enabled = false;
bool mock_dma_self_trigger = false;
std::uint8_t* mock_dma_write_base = nullptr;
std::uint32_t mock_dma_write_index = 0, mock_dma_reload_count = 0;
dma_channel_hw_t mock_dma_channel_hw;
dma_hw_t mock_dma_hw;
uart_hw_t mock_uart_hw;
std::uint32_t mock_uart_baud = PICO_DEFAULT_UART_BAUD_RATE;
void (*mock_dma_irq_handler)() = nullptr;

void mock_dma_pump() {
    while (mock_dma_active && !uart_input.empty()) {
        mock_dma_write_base[mock_dma_write_index++ & 4095u] = uart_input.front();
        uart_input.pop_front();
        std::uint32_t remaining =
            mock_dma_channel_hw.transfer_count &
            DMA_CH0_TRANS_COUNT_COUNT_BITS;
        --remaining;
        if (remaining == 0) {
            mock_dma_hw.ints1 |= 1u;
            if (mock_dma_self_trigger) remaining = mock_dma_reload_count;
            else mock_dma_active = false;
        }
        mock_dma_channel_hw.transfer_count =
            (mock_dma_self_trigger ? mock_dma_self_trigger_mode : 0u) |
            remaining;
        if (mock_dma_irq1_enabled && mock_dma_irq_handler &&
            (mock_dma_hw.ints1 & 1u))
            mock_dma_irq_handler();
    }
}

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
    [](char* b,int n) {
        const int count=read_queue(usb_input,b,n<usb_chunk_limit?n:usb_chunk_limit);
        if(count&&usb_disconnect_after_read)usb_connected=false;
        if(count&&usb_cancel_after_read)local_key=0xb1;
        return count;
    }, nullptr
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
    mock_dma_pump();
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

    const auto& defaults=serial_transfer_performance();
    assert(defaults.ymodem_rx_bulk);
    assert(defaults.tx_packet_coalesce);
    assert(defaults.usb_cdc_bulk);
    assert(defaults.uart_baud==115200);
    assert(defaults.uart_rx_mode==UartRxMode::Irq);
    assert(set_uart_transfer_baud(230400));
    assert(set_uart_transfer_baud(460800));
    assert(set_uart_transfer_baud(921600));
    assert(!set_uart_transfer_baud(12345));
    assert(set_uart_transfer_baud(115200));

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
    for (int i = 0; i < 4095; ++i) uart_input.push_back(i);
    sleep_ms(10);
    std::vector<std::uint8_t> bulk(4096);
    assert(serial_transfer_read_exact(bulk.data(),4095,5)==4095);
    for(int i=0;i<4095;++i)assert(bulk[i]==static_cast<std::uint8_t>(i));
    for (int i = 0; i < 4096; ++i) uart_input.push_back(i+17);
    sleep_ms(10);
    assert(serial_transfer_read_exact(bulk.data(),4096,5)==4096);
    for(int i=0;i<4096;++i)assert(bulk[i]==static_cast<std::uint8_t>(i+17));
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

    // All 4096 slots are usable; the next byte is detected as an overrun.
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    for(int i=0;i<4097;++i)uart_input.push_back(i);
    sleep_ms(1);
    assert(serial_transfer_read(1)==rmb::xmodem::disconnected);
    assert(std::strcmp(serial_transfer_error(),"UART RX OVERFLOW")==0);
    end_serial_transfer();

    // DMA selection is explicit: a host build has no channel and must report
    // unavailable rather than silently pretending to run DMA.
    set_uart_rx_mode(UartRxMode::Dma);
    assert(!begin_serial_transfer(SerialTransferRoute::Uart));
    assert(std::strcmp(serial_transfer_error(),"UART DMA UNAVAILABLE")==0);

    // RP2350 self-trigger mode continuously reloads once per 4 KiB ring.
    // Consume three complete rings to exercise multiple hardware reloads and
    // verify that the 64-bit producer remains ordered across each boundary.
    mock_dma_claim_available=true;
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    for (int cycle=0;cycle<3;++cycle) {
        for (int i=0;i<4096;++i) uart_input.push_back(i+cycle*37);
        sleep_ms(1);
        assert(serial_transfer_read_exact(bulk.data(),4096,5)==4096);
        for (int i=0;i<4096;++i)
            assert(bulk[i]==static_cast<std::uint8_t>(i+cycle*37));
    }
    end_serial_transfer();
    assert(!mock_dma_active && !mock_dma_claimed && !mock_dma_irq_handler);
    assert(mock_uart_baud==PICO_DEFAULT_UART_BAUD_RATE);
    mock_dma_claim_available=false;
    set_uart_rx_mode(UartRxMode::Irq);

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

    // USB bulk reads must accept natural short packets and keep one deadline.
    assert(begin_serial_transfer(SerialTransferRoute::Usb));
    usb_chunk_limit=64;
    for(int i=0;i<1029;++i)usb_input.push_back(i);
    std::vector<std::uint8_t> usb_bulk(1029);
    assert(serial_transfer_read_exact(usb_bulk.data(),usb_bulk.size(),100)==1029);
    for(int i=0;i<1029;++i)assert(usb_bulk[i]==static_cast<std::uint8_t>(i));
    assert(serial_transfer_read_some(usb_bulk.data(),0,0)==0);
    assert(serial_transfer_read_some(usb_bulk.data(),64,0)==0);

    set_usb_cdc_bulk(false);
    for(int i=0;i<64;++i)usb_input.push_back(i);
    assert(serial_transfer_read_some(usb_bulk.data(),64,0)==1);
    set_usb_cdc_bulk(true);
    while(!usb_input.empty())usb_input.pop_front();

    usb_chunk_limit=7;
    for(int i=0;i<10;++i)usb_input.push_back(i);
    assert(serial_transfer_read_exact(usb_bulk.data(),20,3)==rmb::xmodem::timeout);

    for(int i=0;i<10;++i)usb_input.push_back(i);
    usb_disconnect_after_read=true;
    assert(serial_transfer_read_exact(usb_bulk.data(),20,10)==rmb::xmodem::disconnected);
    usb_disconnect_after_read=false;
    end_serial_transfer();

    usb_connected=true;
    assert(begin_serial_transfer(SerialTransferRoute::Usb));
    for(int i=0;i<10;++i)usb_input.push_back(i);
    usb_cancel_after_read=true;
    assert(serial_transfer_read_exact(usb_bulk.data(),20,100)==rmb::xmodem::cancelled);
    usb_cancel_after_read=false;
    local_key=-1;
    end_serial_transfer();
    usb_chunk_limit=INT_MAX;

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

    usb_connected = false;
    assert(begin_usb_diagnostic()==DiagnosticSerialResult::Unavailable);
    usb_connected = true;
    assert(begin_serial_transfer(SerialTransferRoute::Usb));
    assert(begin_usb_diagnostic()==DiagnosticSerialResult::Busy);
    end_serial_transfer();
    assert(begin_serial_transfer(SerialTransferRoute::Uart));
    assert(begin_usb_diagnostic()==DiagnosticSerialResult::Busy);
    end_serial_transfer();
    usb_input.push_back('z');
    const auto route_before=std::string(serial_transfer_route_name());
    const auto uart_before=uart_output.size();
    const auto usb_before=usb_output.size();
    assert(begin_usb_diagnostic()==DiagnosticSerialResult::Ready);
    assert(serial_transfer_active());
    assert(!begin_serial_transfer(SerialTransferRoute::Uart));
    assert(console_serial_read(0)==-1);
    assert(write_usb_diagnostic("report\r\n"));
    assert(usb_output.size()==usb_before+8&&uart_output.size()==uart_before);
    end_usb_diagnostic();
    assert(!serial_transfer_active()&&!write_usb_diagnostic("invalid"));
    assert(std::string(serial_transfer_route_name())==route_before);
    assert(console_serial_read(0)=='z');
    assert(begin_usb_diagnostic()==DiagnosticSerialResult::Ready);
    usb_connected=false;
    assert(!write_usb_diagnostic("disconnected"));
    end_usb_diagnostic();usb_connected=true;
    std::puts(
        "USB bulk/short-read/cancel, UART 4 KiB ring/overflow, settings and routing passed"
    );
}
