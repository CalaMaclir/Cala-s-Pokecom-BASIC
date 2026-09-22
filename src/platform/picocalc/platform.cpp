#include "platform.hpp"
#include "picocalc_display.hpp"
#include "picocalc_keyboard.hpp"
#include "serial_transfer.hpp"

#include "hardware/clocks.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/low_power.h"

namespace rmb::platform {

namespace {
StatusRefreshCallback status_refresh_callback = nullptr;
void* status_refresh_context = nullptr;
ScreenshotCallback screenshot_callback = nullptr;
void* screenshot_context = nullptr;
ServiceCallback background_service_callback = nullptr;
void* background_service_context = nullptr;
ServiceCallback sleep_prepare_callback = nullptr;
void* sleep_prepare_context = nullptr;

ConsoleMode console_mode = ConsoleMode::Both;
std::uint32_t console_background_color = 0x000000;

bool console_uses_lcd() {
    return console_mode != ConsoleMode::Serial;
}

bool console_uses_serial() {
    return !serial_transfer_active() && console_mode != ConsoleMode::Lcd;
}

bool software_clock_valid = false;
DateTime software_clock_base;
uint64_t software_clock_base_us = 0;
uint64_t next_hardware_rtc_probe_us = 0;
bool hardware_rtc_ok = false;
bool hardware_rtc_probed = false;
std::uint32_t cpu_clock_source_hz = 0;
bool peripheral_clock_isolated = false;

bool leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) ||
           (year % 400 == 0);
}

int month_days(int year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

void set_software_clock(const DateTime& value) {
    software_clock_base = value;
    software_clock_base_us = to_us_since_boot(get_absolute_time());
    software_clock_valid = true;
}

bool read_software_clock(DateTime& value) {
    if (!software_clock_valid) return false;

    value = software_clock_base;
    uint64_t elapsed =
        (to_us_since_boot(get_absolute_time()) - software_clock_base_us) /
        1000000u;

    value.second += static_cast<int>(elapsed % 60u);
    elapsed /= 60u;
    if (value.second >= 60) {
        value.second -= 60;
        ++elapsed;
    }

    value.minute += static_cast<int>(elapsed % 60u);
    elapsed /= 60u;
    if (value.minute >= 60) {
        value.minute -= 60;
        ++elapsed;
    }

    value.hour += static_cast<int>(elapsed % 24u);
    elapsed /= 24u;
    if (value.hour >= 24) {
        value.hour -= 24;
        ++elapsed;
    }

    while (elapsed > 0) {
        const int remaining =
            month_days(value.year, value.month) - value.day;
        if (elapsed <= static_cast<uint64_t>(remaining)) {
            value.day += static_cast<int>(elapsed);
            elapsed = 0;
            break;
        }

        elapsed -= static_cast<uint64_t>(remaining + 1);
        value.day = 1;
        ++value.month;
        if (value.month > 12) {
            value.month = 1;
            ++value.year;
        }
    }

    return true;
}
} // namespace

void init() {
    stdio_init_all();

    // Capture the boot-time PLL_SYS-backed clk_sys frequency before any user
    // profile is applied. CPU speed profiles vary clk_sys only; clk_peri and
    // the PLL remain untouched.
    cpu_clock_source_hz = clock_get_hz(clk_sys);

    picocalc::keyboard::init();
    picocalc::keyboard::set_lcd_backlight(160);

    picocalc::display::init();
    picocalc::display::set_text_color(0x00ff00, 0x000000);
}

void clear_lcd() {
    picocalc::display::clear(console_background_color);
}

void clear_lcd_color(std::uint32_t rgb) {
    picocalc::display::clear(rgb & 0x00ffffffu);
}

void clear_screen() {
    if (console_uses_lcd()) {
        picocalc::display::clear(console_background_color);
    }

    if (console_uses_serial()) {
        // ANSI clear-screen + home for USB/UART terminal consoles.
        serial_put_string_raw("\x1b[2J\x1b[H");
    }
}

void screen_put_char(char c) {
    picocalc::display::put_char(c);
}

void serial_put_char_raw(char c) {
    if (serial_transfer_active()) return;
    putchar_raw(c);
}

void serial_put_string_raw(const char* text) {
    if (serial_transfer_active()) return;
    if (!text) return;
    while (*text) {
        putchar_raw(*text++);
    }
}

void set_console_mode(ConsoleMode mode) {
    console_mode = mode;
}

void set_text_color(
    std::uint32_t foreground,
    std::uint32_t background
) {
    console_background_color = background & 0x00ffffffu;
    picocalc::display::set_text_color(
        foreground & 0x00ffffffu,
        console_background_color
    );
}

ConsoleMode get_console_mode() {
    return console_mode;
}

void set_status_area_enabled(bool enabled) {
    picocalc::display::set_status_area_enabled(enabled);
}

bool status_area_enabled() {
    return picocalc::display::status_area_enabled();
}

void set_function_key_bar_enabled(bool enabled) {
    picocalc::display::set_function_key_bar_enabled(enabled);
}

bool function_key_bar_enabled() {
    return picocalc::display::function_key_bar_enabled();
}

void set_status_refresh_callback(
    StatusRefreshCallback callback,
    void* context
) {
    status_refresh_callback = callback;
    status_refresh_context = context;
}

void set_screenshot_callback(
    ScreenshotCallback callback,
    void* context
) {
    screenshot_callback = callback;
    screenshot_context = context;
}

void set_background_service_callback(ServiceCallback callback, void* context) {
    background_service_callback = callback;
    background_service_context = context;
}

void set_sleep_prepare_callback(ServiceCallback callback, void* context) {
    sleep_prepare_callback = callback;
    sleep_prepare_context = context;
}

void sleep_millis(std::uint32_t milliseconds) {
    sleep_ms(milliseconds);
}

void enter_sleep_mode() {
    if (sleep_prepare_callback) sleep_prepare_callback(sleep_prepare_context);
    unsigned char lcd_level = 160;
    unsigned char keyboard_level = 0;

    const bool have_lcd =
        picocalc::keyboard::get_lcd_backlight(lcd_level);
    const bool have_keyboard =
        picocalc::keyboard::get_keyboard_backlight(keyboard_level);

    // Reversible STANDBY: RAM/VM state remains live, both lights go dark,
    // and RP2350 spends almost all waiting time in Pico SDK deep sleep.
    // The keyboard controller is I2C-polled rather than wired as a wake GPIO,
    // so wake latency is bounded by the short periodic timer interval.
    serial_put_string_raw("[STANDBY] PRESS ANY KEY TO WAKE\r\n");

    picocalc::keyboard::set_lcd_backlight(0);
    picocalc::keyboard::set_keyboard_backlight(0);

    // Allow the triggering key to be released before wake polling begins.
    sleep_ms(180);

    while (true) {
        const int key = picocalc::keyboard::read_key();
        if (key >= 0) {
            console_local_input();
            break;
        }

        if (console_uses_serial()) {
            const int serial_char = console_serial_read(0);
            if (serial_char >= 0) {
                break;
            }
        }

        // Keep RAM and library-required clocks alive, gate the rest, and wake
        // only on the default timer. Exclusive sleep defers peripheral IRQs
        // until clocks are restored, avoiding ISR execution while gated.
        const int rc = low_power_sleep_for_ms(50, nullptr, true);
        if (rc != 0) {
            // If a hardware alarm resource is temporarily unavailable, fall
            // back to a normal short delay rather than trapping in STANDBY.
            sleep_ms(50);
        }
    }

    picocalc::keyboard::set_lcd_backlight(
        have_lcd ? lcd_level : static_cast<unsigned char>(160)
    );
    if (have_keyboard) {
        picocalc::keyboard::set_keyboard_backlight(keyboard_level);
    }

    if (status_refresh_callback) {
        status_refresh_callback(status_refresh_context);
    }

    serial_put_string_raw("[WAKE]\r\n");
}

void draw_text_row(
    int row,
    const char* text,
    std::uint32_t foreground,
    std::uint32_t background
) {
    picocalc::display::draw_text_row(row, text, foreground, background);
}

bool get_battery_status(int& percent, bool& charging) {
    return picocalc::keyboard::read_battery(percent, charging);
}

bool caps_lock_enabled() {
    return picocalc::keyboard::caps_lock_enabled();
}

bool shift_held() {
    return picocalc::keyboard::shift_held();
}

bool get_datetime(DateTime& value) {
    const uint64_t now_us = to_us_since_boot(get_absolute_time());

    if (now_us >= next_hardware_rtc_probe_us) {
        picocalc::keyboard::RtcDateTime rtc;
        if (picocalc::keyboard::read_rtc(rtc)) {
            value.year = rtc.year;
            value.month = rtc.month;
            value.day = rtc.day;
            value.hour = rtc.hour;
            value.minute = rtc.minute;
            value.second = rtc.second;
            set_software_clock(value);
            hardware_rtc_ok = true;
            hardware_rtc_probed = true;

            // Once loaded, the monotonic software clock is sufficient for UI
            // updates. Re-check the external RTC only occasionally.
            next_hardware_rtc_probe_us = now_us + 60000000u;
            return true;
        }

        hardware_rtc_ok = false;
        hardware_rtc_probed = true;

        // Do not automatically touch the RTC again after a failed probe.
        // The keyboard controller and battery monitor share the same physical
        // I2C bus and are essential PicoCalc input devices. A manual time set
        // or an NTP sync may still try write_rtc() explicitly.
        next_hardware_rtc_probe_us = ~uint64_t{0};
    }

    return read_software_clock(value);
}

bool set_datetime(const DateTime& value) {
    // The PicoCalc does not require an onboard RTC. The software clock is the
    // primary clock and is valid after manual input or NTP synchronization.
    set_software_clock(value);

    // Only write an external PCF8563 when one was actually detected. This
    // avoids treating a perfectly normal "no external RTC fitted" PicoCalc as
    // an error and avoids unnecessary traffic on the keyboard I2C bus.
    if (!hardware_rtc_available()) {
        return true;
    }

    picocalc::keyboard::RtcDateTime rtc;
    rtc.year = value.year;
    rtc.month = value.month;
    rtc.day = value.day;
    rtc.hour = value.hour;
    rtc.minute = value.minute;
    rtc.second = value.second;

    const bool written = picocalc::keyboard::write_rtc(rtc);
    hardware_rtc_ok = written;
    hardware_rtc_probed = true;
    next_hardware_rtc_probe_us = written
        ? to_us_since_boot(get_absolute_time()) + 60000000u
        : ~uint64_t{0};

    // The software clock remains valid even if an optional external RTC write
    // subsequently fails.
    return true;
}

bool hardware_rtc_available() {
    return hardware_rtc_probed && hardware_rtc_ok;
}

const char* datetime_last_error() {
    using Error = picocalc::keyboard::RtcError;
    switch (picocalc::keyboard::last_rtc_error()) {
        case Error::None: return "NONE";
        case Error::NotInitialized: return "RTC DRIVER NOT INITIALIZED";
        case Error::InvalidValue: return "INVALID DATE/TIME VALUE";
        case Error::PointerWriteFailed: return "RTC POINTER WRITE FAILED";
        case Error::ReadFailed: return "RTC READ FAILED";
        case Error::WriteFailed: return "RTC WRITE FAILED";
        case Error::ReadbackFailed: return "RTC READBACK FAILED";
        case Error::ReadbackMismatch: return "RTC READBACK MISMATCH";
    }
    return "RTC ERROR";
}

bool set_lcd_backlight(std::uint8_t value) {
    return picocalc::keyboard::set_lcd_backlight(value);
}

bool get_lcd_backlight(std::uint8_t& value) {
    unsigned char current = 0;
    if (!picocalc::keyboard::get_lcd_backlight(current)) {
        return false;
    }
    value = static_cast<std::uint8_t>(current);
    return true;
}

void put_char(char c) {
    if (console_uses_lcd()) {
        screen_put_char(c);
    }

    if (console_uses_serial()) {
        putchar_raw(c);
    }
}

void put_string(const char* text) {
    if (!text) return;

    if (console_uses_lcd()) {
        picocalc::display::put_string(text);
    }

    if (console_uses_serial()) {
        const char* p = text;
        while (*p) {
            putchar_raw(*p++);
        }
    }
}

void clear_to_eol() {
    if (console_uses_lcd()) {
        picocalc::display::clear_to_eol();
    }

    if (console_uses_serial()) {
        // ANSI erase from cursor to end of line.
        serial_put_string_raw("\x1b[K");
    }
}

int cursor_column() {
    return picocalc::display::cursor_column();
}

int cursor_row() {
    return picocalc::display::cursor_row();
}

int text_columns() {
    return picocalc::display::text_columns;
}

int text_rows() {
    return picocalc::display::text_rows -
        (picocalc::display::function_key_bar_enabled() ? 1 : 0);
}

void set_cursor_position(int column, int row) {
    picocalc::display::set_cursor_position(column, row);
}

void scroll_text_rows(int rows) {
    picocalc::display::scroll_text_rows(rows);
}

void set_graphics_color(std::uint32_t rgb) {
    picocalc::display::set_graphics_color(rgb);
}

std::uint32_t graphics_color() {
    return picocalc::display::graphics_color();
}

void graphics_clear(std::uint32_t rgb) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_clear(rgb);
}

void graphics_pixel(int x, int y) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_pixel(x, y);
}

void graphics_line(int x1, int y1, int x2, int y2) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_line(x1, y1, x2, y2);
}

void graphics_line_to(int x, int y) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_line_to(x, y);
}

void graphics_circle(int cx, int cy, int radius) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_circle(cx, cy, radius);
}

void graphics_box(int x1, int y1, int x2, int y2, bool filled) {
    picocalc::display::set_function_key_bar_enabled(false);
    picocalc::display::graphics_box(x1, y1, x2, y2, filled);
}

bool graphics_paint(int x, int y) {
    picocalc::display::set_function_key_bar_enabled(false);
    return picocalc::display::graphics_paint(x, y);
}

void graphics_flush() {
    picocalc::display::graphics_flush();
}

bool graphics_point_nonblack(int x, int y) {
    picocalc::display::set_function_key_bar_enabled(false);
    return picocalc::display::graphics_point_nonblack(x, y);
}

std::uint32_t monotonic_millis() {
    return to_ms_since_boot(get_absolute_time());
}

std::uint32_t system_clock_hz() {
    return clock_get_hz(clk_sys);
}

std::uint32_t full_cpu_clock_hz() {
    return cpu_clock_source_hz
        ? cpu_clock_source_hz
        : clock_get_hz(clk_sys);
}

bool set_cpu_clock_mhz(std::uint32_t mhz) {
    if (mhz != 150u && mhz != 100u && mhz != 75u) {
        return false;
    }

    if (cpu_clock_source_hz == 0) {
        cpu_clock_source_hz = clock_get_hz(clk_sys);
    }

    const std::uint32_t source_mhz = cpu_clock_source_hz / 1000000u;
    if (source_mhz < mhz || source_mhz == 0) {
        return false;
    }

    // At reset clk_peri normally follows clk_sys. If clk_sys is divided
    // without first separating clk_peri, UART/SPI baud rates silently scale
    // with the CPU clock even though their SDK divisors still assume the old
    // frequency. Pin clk_peri directly to PLL_SYS once, at the same 150 MHz
    // boot frequency, so UART, LCD SPI and SD SPI remain stable.
    if (!peripheral_clock_isolated) {
        clock_configure_undivided(
            clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
            cpu_clock_source_hz
        );
        peripheral_clock_isolated = true;
    }

    const bool ok = clock_configure_mhz(
        clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        source_mhz,
        mhz
    );
    if (!ok) {
        return false;
    }

    // I2C is clocked from clk_sys on RP2350 rather than clk_peri. Recompute
    // its timing registers after every CPU clock change. This keeps the
    // PicoCalc keyboard/RTC bus at its requested 10 kHz and, importantly,
    // keeps transactions within the existing 5 ms timeout at 75 MHz.
    picocalc::keyboard::reconfigure_bus_clock();

    return true;
}

bool break_requested() {
    if (serial_transfer_active()) return false;
    if (background_service_callback)
        background_service_callback(background_service_context);
    // USB/UART polling is effectively free, so keep it responsive when
    // serial input is enabled.
    if (console_uses_serial()) {
        const int debug_char = console_serial_read(0);
        if (debug_char == 0x03 || debug_char == 0x1b) {
            return true;
        }
    }

    // PicoCalc keyboard reads use the 10 kHz I2C controller and include a
    // short settling delay. For long numeric/graphics loops, 5 Hz BREAK
    // sampling is sufficient and removes nearly all I2C polling overhead.
    static uint32_t next_keyboard_poll_ms = 0;
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    if (static_cast<int32_t>(now - next_keyboard_poll_ms) < 0) {
        return false;
    }

    next_keyboard_poll_ms = now + 200;

    const int key = picocalc::keyboard::read_key();
    if (key == picocalc::keyboard::key_hotkey_screenshot) {
        if (screenshot_callback) {
            screenshot_callback(screenshot_context);
        }
        return false;
    }
    if (key == picocalc::keyboard::key_hotkey_sleep) {
        enter_sleep_mode();
        return false;
    }
    return key == 0x03 || key == 0xb1 || key == 0xd0;
}

int get_char() {
    if (serial_transfer_active()) return -1;
    constexpr uint32_t blink_ms = 500;
    static bool swallow_serial_lf = false;

    bool cursor_visible = true;
    if (console_uses_lcd()) {
        picocalc::display::set_cursor_visible(true);
    }
    uint32_t next_blink =
        to_ms_since_boot(get_absolute_time()) + blink_ms;

    static bool last_shift_held = false;

    while (true) {
        if (background_service_callback)
            background_service_callback(background_service_context);
        const int key = picocalc::keyboard::read_key();

        const bool current_shift_held = picocalc::keyboard::shift_held();
        if (current_shift_held != last_shift_held) {
            last_shift_held = current_shift_held;
            if (status_refresh_callback) {
                status_refresh_callback(status_refresh_context);
            }
        }

        if (key == picocalc::keyboard::key_hotkey_screenshot) {
            if (screenshot_callback) {
                screenshot_callback(screenshot_context);
            }
            continue;
        }

        if (key == picocalc::keyboard::key_hotkey_sleep) {
            if (console_uses_lcd()) {
                picocalc::display::set_cursor_visible(false);
            }
            enter_sleep_mode();
            cursor_visible = true;
            if (console_uses_lcd()) {
                picocalc::display::set_cursor_visible(true);
            }
            next_blink =
                to_ms_since_boot(get_absolute_time()) + blink_ms;
            continue;
        }

        // Caps Lock is a state-change event, not a text character. Refresh the
        // status rows immediately and keep waiting for the user's next key.
        if (key == 0xc1) {
            if (status_refresh_callback) {
                status_refresh_callback(status_refresh_context);
            }
            continue;
        }

        if (key >= 0) {
            if (console_uses_lcd()) {
                picocalc::display::set_cursor_visible(false);
            }
            return key;
        }

        // USB CDC and UART stdio are both enabled. Treat either as a complete
        // terminal input path when serial console input is enabled.
        int serial_char = console_uses_serial()
            ? console_serial_read(0)
            : -1;
        if (serial_char >= 0) {
            if (swallow_serial_lf && serial_char == '\n') {
                swallow_serial_lf = false;
                continue;
            }
            swallow_serial_lf = false;

            if (serial_char == '\r') {
                swallow_serial_lf = true;
            }

            // Most terminal emulators send DEL for Backspace.
            if (serial_char == 0x7f) {
                serial_char = 0x08;
            }

            // Minimal ANSI cursor-key decoding. Full line editing remains
            // available from the PicoCalc keyboard, while serial terminals get
            // the common arrow/home/end/delete keys as well.
            if (serial_char == 0x1b) {
                const int c2 = console_serial_read(3000, true);

                if (c2 == 'O') {
                    // Common VT/xterm F1-F4 sequences: ESC O P..S.
                    const int c3 = console_serial_read(3000, true);
                    if (c3 >= 'P' && c3 <= 'S') {
                        serial_char = 0x81 + (c3 - 'P');
                    } else {
                        serial_char = 0x1b;
                    }
                } else if (c2 == '[') {
                    const int c3 = console_serial_read(3000, true);
                    if (c3 == 'A') serial_char = 0xb5;      // up
                    else if (c3 == 'B') serial_char = 0xb6; // down
                    else if (c3 == 'C') serial_char = 0xb7; // right
                    else if (c3 == 'D') serial_char = 0xb4; // left
                    else if (c3 == 'H') serial_char = 0xd2; // home
                    else if (c3 == 'F') serial_char = 0xd5; // end
                    else if (c3 == '3') {
                        const int c4 = console_serial_read(3000, true);
                        serial_char = c4 == '~' ? 0xd4 : 0x1b;
                    } else if (c3 == '1' || c3 == '2') {
                        const int c4 = console_serial_read(3000, true);
                        const int c5 = console_serial_read(3000, true);
                        if (c4 >= '0' && c4 <= '9' && c5 == '~') {
                            const int code =
                                (c3 - '0') * 10 + (c4 - '0');
                            if (code == 15) serial_char = 0x85;      // F5
                            else if (code == 17) serial_char = 0x86; // F6
                            else if (code == 18) serial_char = 0x87; // F7
                            else if (code == 19) serial_char = 0x88; // F8
                            else if (code == 20) serial_char = 0x89; // F9
                            else if (code == 21) serial_char = 0x90; // F10
                            else serial_char = 0x1b;
                        } else {
                            serial_char = 0x1b;
                        }
                    } else {
                        serial_char = 0x1b;
                    }
                }
            }

            if (console_uses_lcd()) {
                picocalc::display::set_cursor_visible(false);
            }
            return serial_char;
        }

        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (static_cast<int32_t>(now - next_blink) >= 0) {
            cursor_visible = !cursor_visible;
            if (console_uses_lcd()) {
                picocalc::display::set_cursor_visible(cursor_visible);
            }
            next_blink = now + blink_ms;
        }

        sleep_ms(4);
    }
}

} // namespace rmb::platform
