#pragma once

#include <cstdint>

using uint = unsigned int;
using absolute_time_t = std::uint64_t;

constexpr int GPIO_FUNC_I2C = 1;
constexpr int GPIO_FUNC_SIO = 2;
constexpr bool GPIO_IN = false;
constexpr bool GPIO_OUT = true;

absolute_time_t get_absolute_time();
std::uint32_t to_ms_since_boot(absolute_time_t);
void sleep_ms(std::uint32_t);
void sleep_us(std::uint64_t);
void gpio_set_function(uint, int);
void gpio_pull_up(uint);
void gpio_put(uint, bool);
void gpio_set_dir(uint, bool);
bool gpio_get(uint);

absolute_time_t make_timeout_time_us(std::uint64_t);
bool time_reached(absolute_time_t);
void tight_loop_contents();
