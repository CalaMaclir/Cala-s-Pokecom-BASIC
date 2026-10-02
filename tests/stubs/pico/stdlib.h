#pragma once
using uint = unsigned int;
constexpr int GPIO_FUNC_SPI = 0;
constexpr int GPIO_OUT = 1;
inline void gpio_init(uint) {}
inline void gpio_put(uint, bool) {}
inline void gpio_set_dir(uint, bool) {}
inline void gpio_set_function(uint, int) {}
inline void gpio_set_input_hysteresis_enabled(uint, bool) {}

inline bool test_sleep_capture = false;
inline unsigned test_sleep_count = 0;
inline uint test_sleep_values[8] = {};
inline void test_sleep_begin_capture() {
    test_sleep_count = 0;
    test_sleep_capture = true;
}
inline void test_sleep_end_capture() { test_sleep_capture = false; }
inline void sleep_ms(uint ms) {
    if (test_sleep_capture &&
        test_sleep_count < sizeof(test_sleep_values) / sizeof(test_sleep_values[0])) {
        test_sleep_values[test_sleep_count++] = ms;
    }
}
