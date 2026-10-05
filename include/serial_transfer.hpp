#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb::platform {
enum class SerialTransferRoute {
    Auto,
    Usb,
    Uart
};

enum class UartRxMode {
    Irq,
    Dma
};

// Version 0.91 Stage 1 performance switches are deliberately session-only.
// Defaults are conservative for UART while enabling the protocol/USB fast
// paths which do not change the X/YMODEM wire format.
struct SerialTransferPerformance {
    bool ymodem_rx_bulk = true;
    bool tx_packet_coalesce = true;
    bool usb_cdc_bulk = true;
    std::uint32_t uart_baud = 115200;
    UartRxMode uart_rx_mode = UartRxMode::Irq;
};

int console_serial_read(unsigned timeout_us, bool same_route = false);
void console_local_input();
const char* serial_transfer_route_name(
    SerialTransferRoute route = SerialTransferRoute::Auto
);
const char* serial_transfer_error();
bool begin_serial_transfer(
    SerialTransferRoute route = SerialTransferRoute::Auto
);
void end_serial_transfer();
bool serial_transfer_active();
bool usb_cdc_ready();
enum class DiagnosticSerialResult { Ready, Busy, Unavailable };
DiagnosticSerialResult begin_usb_diagnostic();
bool write_usb_diagnostic(const char* text);
void end_usb_diagnostic();
int serial_transfer_read(unsigned timeout_ms);
int serial_transfer_read_some(
    std::uint8_t* data,
    std::size_t size,
    unsigned timeout_ms
);
int serial_transfer_read_exact(
    std::uint8_t* data,
    std::size_t size,
    unsigned timeout_ms
);
bool serial_transfer_write(const std::uint8_t* data, std::size_t size);
const SerialTransferPerformance& serial_transfer_performance();
void set_ymodem_rx_bulk(bool enabled);
void set_tx_packet_coalesce(bool enabled);
void set_usb_cdc_bulk(bool enabled);
bool set_uart_transfer_baud(std::uint32_t baud);
void set_uart_rx_mode(UartRxMode mode);
const char* uart_rx_mode_name(UartRxMode mode);
}
