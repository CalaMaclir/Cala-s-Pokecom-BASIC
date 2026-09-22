#pragma once

#include <cstdint>

namespace rmb::platform {

enum class ConsoleMode {
    Lcd,
    Serial,
    Both
};

struct DateTime {
    int year = 2000;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

void init();
void clear_screen();
void clear_lcd();
void clear_lcd_color(std::uint32_t rgb);

void put_char(char c);
void put_string(const char* text);
void clear_to_eol();
void screen_put_char(char c);
void serial_put_char_raw(char c);
void serial_put_string_raw(const char* text);

void set_console_mode(ConsoleMode mode);
ConsoleMode get_console_mode();

using StatusRefreshCallback = void (*)(void* context);
using ScreenshotCallback = void (*)(void* context);
using ServiceCallback = void (*)(void* context);

void set_text_color(std::uint32_t foreground, std::uint32_t background);
void set_status_area_enabled(bool enabled);
bool status_area_enabled();
void set_function_key_bar_enabled(bool enabled);
bool function_key_bar_enabled();
void set_status_refresh_callback(
    StatusRefreshCallback callback,
    void* context
);
void set_screenshot_callback(
    ScreenshotCallback callback,
    void* context
);
void set_background_service_callback(ServiceCallback callback, void* context);
void set_sleep_prepare_callback(ServiceCallback callback, void* context);
void sleep_millis(std::uint32_t milliseconds);
void enter_sleep_mode();
void draw_text_row(
    int row,
    const char* text,
    std::uint32_t foreground,
    std::uint32_t background
);

bool get_battery_status(int& percent, bool& charging);
bool caps_lock_enabled();
bool shift_held();
bool get_datetime(DateTime& value);
bool set_datetime(const DateTime& value);
bool hardware_rtc_available();
const char* datetime_last_error();
bool set_lcd_backlight(std::uint8_t value);
bool get_lcd_backlight(std::uint8_t& value);

int get_char();
bool break_requested();
std::uint32_t monotonic_millis();
std::uint32_t system_clock_hz();
std::uint32_t full_cpu_clock_hz();
bool set_cpu_clock_mhz(std::uint32_t mhz);

int cursor_column();
int cursor_row();
int text_columns();
int text_rows();
void set_cursor_position(int column, int row);
void scroll_text_rows(int rows);

void set_graphics_color(std::uint32_t rgb);
std::uint32_t graphics_color();
void graphics_clear(std::uint32_t rgb = 0x000000);
void graphics_pixel(int x, int y);
void graphics_line(int x1, int y1, int x2, int y2);
void graphics_line_to(int x, int y);
void graphics_circle(int cx, int cy, int radius);
void graphics_box(int x1, int y1, int x2, int y2, bool filled);
bool graphics_paint(int x, int y);
void graphics_flush();
bool graphics_point_nonblack(int x, int y);

} // namespace rmb::platform
