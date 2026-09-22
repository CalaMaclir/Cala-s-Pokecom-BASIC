#pragma once
using uint = unsigned int;
constexpr int GPIO_FUNC_SPI = 0;
constexpr int GPIO_OUT = 1;
inline void gpio_init(uint) {}
inline void gpio_put(uint, bool) {}
inline void gpio_set_dir(uint, bool) {}
inline void gpio_set_function(uint, int) {}
inline void gpio_set_input_hysteresis_enabled(uint, bool) {}
inline void sleep_ms(uint) {}
