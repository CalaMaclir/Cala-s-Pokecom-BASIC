#pragma once
#include <cstdint>
namespace rmb::picocalc::external_i2c {
enum class Result : std::uint8_t { Ok, Nack, Timeout, Busy, BadAddress };
void init();
void reconfigure_bus_clock();
bool try_lock();
void unlock();
Result scan(std::uint8_t* addresses, int capacity, int& count);
Result read_register(std::uint8_t address, std::uint8_t reg, std::uint8_t& value);
Result write_register(std::uint8_t address, std::uint8_t reg, std::uint8_t value);
Result read_registers(std::uint8_t address, std::uint8_t reg, std::uint8_t* data, int count);
Result write_bytes(std::uint8_t address, const std::uint8_t* data, int count);
}