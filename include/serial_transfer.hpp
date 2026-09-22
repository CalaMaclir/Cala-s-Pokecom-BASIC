#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb::platform {
int console_serial_read(unsigned timeout_us, bool same_route = false);
void console_local_input();
const char* serial_transfer_route_name();
const char* serial_transfer_error();
bool begin_serial_transfer();
void end_serial_transfer();
bool serial_transfer_active();
int serial_transfer_read(unsigned timeout_ms);
bool serial_transfer_write(const std::uint8_t* data, std::size_t size);
}
