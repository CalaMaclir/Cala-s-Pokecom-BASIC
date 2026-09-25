#pragma once
#include <cstddef>
#include <cstdint>
namespace rmb::bluetooth_serial {
bool init(); bool enable(); void disable();
bool enabled(); bool connected();
const char* status(); const char* device_name(); const char* last_error();
bool set_console_enabled(bool enabled);
bool console_enabled();
void set_test_terminal_active(bool active);
bool test_terminal_active();
bool begin_transfer();
void end_transfer();
bool transfer_active();
int read_console();
int read_test();
int read_transfer();
std::size_t read_transfer(std::uint8_t* destination,
                          std::size_t capacity);
bool write_transfer(const std::uint8_t* data,std::size_t length);
bool write(const std::uint8_t* data,std::size_t length);
bool write_text(const char* text);
bool write_console(const std::uint8_t* data,std::size_t length);
bool write_console_text(const char* text);
void write_console_char(char value);
void service();
std::uint32_t rx_overflow_count();
std::uint32_t tx_overflow_count();
std::uint32_t tx_queued_bytes();
std::uint32_t tx_sent_bytes();
} // namespace rmb::bluetooth_serial
