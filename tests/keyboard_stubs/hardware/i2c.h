#pragma once

#include <cstddef>
#include <cstdint>

using uint = unsigned int;

struct i2c_inst_t {};
extern i2c_inst_t* i2c1;

std::uint32_t i2c_init(i2c_inst_t*, std::uint32_t);
void i2c_deinit(i2c_inst_t*);
std::uint32_t i2c_set_baudrate(i2c_inst_t*, std::uint32_t);
int i2c_write_timeout_us(
    i2c_inst_t*,
    std::uint8_t,
    const std::uint8_t*,
    std::size_t,
    bool,
    std::uint32_t
);
int i2c_read_timeout_us(
    i2c_inst_t*,
    std::uint8_t,
    std::uint8_t*,
    std::size_t,
    bool,
    std::uint32_t
);

std::size_t i2c_get_write_available(i2c_inst_t*);
int i2c_read_blocking_until(
    i2c_inst_t*, std::uint8_t, std::uint8_t*, std::size_t, bool,
    std::uint64_t
);
