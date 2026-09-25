#pragma once

#include <cstdint>
#include "key_click.hpp"

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

enum class RtcSource : std::uint8_t { Auto, External, Internal, Off };
enum class I2cResult : std::uint8_t { Ok, Nack, Timeout, Busy, BadAddress };
I2cResult external_i2c_read(std::uint8_t address, std::uint8_t reg, std::uint8_t& value);
I2cResult external_i2c_write(std::uint8_t address, std::uint8_t reg, std::uint8_t value);
int external_i2c_scan(std::uint8_t* addresses, int capacity);
const char* i2c_result_text(I2cResult result);

// Background PCM audio on PicoCalc GP26/GP27.
void audio_init();
void audio_reconfigure_clock();
void audio_service();
void audio_set_key_click(audio::KeyClickMode mode);
void audio_key_click();
void audio_stop();
void audio_pause();
void audio_resume();
bool audio_playing();
bool audio_beep(int frequency_hz, int duration_ms);
bool audio_play_mml(const char* const* voices, int count);
bool audio_wavplay(const char* filename);
void audio_set_volume(int percent);
int audio_volume();
const char* audio_last_error();
void set_rtc_source(RtcSource source);
RtcSource rtc_source();
const char* rtc_source_name();
bool set_rtc_address(std::uint8_t address);
std::uint8_t rtc_address();
bool probe_rtc();
const char* rtc_location();

enum class RuntimeKeyType : std::uint8_t {
    None,
    Key,
    Break
};

struct RuntimeKeyResult {
    RuntimeKeyType type = RuntimeKeyType::None;
    int code = 0;
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
bool terminal_console_enabled();
void begin_command_input();
void end_command_input();

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
void enter_sleep_mode(bool refresh_status = true);
void enter_bootsel();
void reboot_system();
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
RuntimeKeyResult poll_runtime_key();
RuntimeKeyResult wait_runtime_key();
void reset_runtime_input();
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

// Graphics text has an independent 8x8 pixel cursor and never writes to the
// text console or serial output.
void graphics_text_locate(int x, int y);
void graphics_text_print(const char* text);
bool graphics_define(char character, const char* hex_pixels);
void graphics_define_clear();
bool graphics_palette_rgb(int index, int red, int green, int blue);
bool graphics_palette_rgb24(int index, std::uint32_t rgb);
void graphics_palette_reset();

} // namespace rmb::platform
