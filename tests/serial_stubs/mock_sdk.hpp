#pragma once
#include <cstddef>
#include <cstdint>
struct stdio_driver_t {
    void (*out_chars)(const char*, int);
    void (*out_flush)();
    int (*in_chars)(char*, int);
    void (*set_chars_available_callback)(void (*)(void*), void*);
};
extern stdio_driver_t stdio_usb, stdio_uart;
void stdio_flush();
void stdio_set_driver_enabled(stdio_driver_t*, bool);
bool stdio_usb_connected();
using absolute_time_t = std::uint64_t;
extern std::uint64_t test_time;
inline absolute_time_t get_absolute_time() { return test_time; }
inline absolute_time_t make_timeout_time_us(unsigned n) { return test_time+n; }
inline absolute_time_t make_timeout_time_ms(unsigned n) { return test_time+n*1000; }
inline unsigned to_ms_since_boot(absolute_time_t t) { return t/1000; }
inline bool time_reached(absolute_time_t t) { return test_time>=t; }
void sleep_ms(unsigned);
void sleep_us(unsigned);
inline unsigned save_and_disable_interrupts() { return 0; }
inline void restore_interrupts(unsigned) {}
constexpr int uart_default = 0;
void uart_write_blocking(int, const std::uint8_t*, std::size_t);
