#include "platform.hpp"
#include "psram.hpp"
#include "picocalc_display.hpp"
#include "picocalc_keyboard.hpp"
#include "unified_keyboard.hpp"
#include "external_i2c.hpp"
#include "system_controls.hpp"
#include "usb_msc.hpp"
#include "wireless.hpp"
#include "graphics_text.hpp"
#include "bluetooth_manager.hpp"
#include "serial_transfer.hpp"
#include "serial_crlf_filter.hpp"
#include "rtc_codec.hpp"

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
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
RtcSource configured_rtc_source = RtcSource::Auto;
std::uint8_t configured_rtc_address = 0x51;
enum class ActiveRtc : std::uint8_t { None, External, Internal };
ActiveRtc active_rtc = ActiveRtc::None;
RtcDevice active_rtc_device = RtcDevice::None;
std::uint8_t active_rtc_address = 0;
std::uint32_t cpu_clock_source_hz = 0;
bool peripheral_clock_isolated = false;

constexpr std::size_t runtime_key_queue_size = 8;
int runtime_key_queue[runtime_key_queue_size] = {};
std::size_t runtime_key_read = 0;
std::size_t runtime_key_write = 0;
std::uint32_t next_runtime_keyboard_poll_ms = 0;

enum class RuntimeSerialState : std::uint8_t {
    Idle,
    Escape
};
RuntimeSerialState runtime_serial_state = RuntimeSerialState::Idle;
char runtime_serial_sequence[6] = {};
std::size_t runtime_serial_length = 0;
std::uint32_t runtime_serial_deadline_ms = 0;
detail::SerialCrLfFilter serial_crlf_filter;
enum class CommandInputSource : std::uint8_t { None, Local, Serial };
CommandInputSource command_input_source = CommandInputSource::None;
bool command_input_active = false;
int pending_command_key = -1;
enum class RuntimeTerminalSource : std::uint8_t { None, Serial };
RuntimeTerminalSource runtime_terminal_source = RuntimeTerminalSource::None;

RuntimeKeyResult no_runtime_key() {
    return {RuntimeKeyType::None, 0};
}

RuntimeKeyResult runtime_key(int code) {
    return {RuntimeKeyType::Key, code};
}

RuntimeKeyResult runtime_break() {
    return {RuntimeKeyType::Break, 0};
}

bool queue_runtime_key(int code) {
    const std::size_t next =
        (runtime_key_write + 1u) % runtime_key_queue_size;
    if (next == runtime_key_read) return false;
    runtime_key_queue[runtime_key_write] = code;
    runtime_key_write = next;
    return true;
}

bool dequeue_runtime_key(int& code) {
    if (runtime_key_read == runtime_key_write) return false;
    code = runtime_key_queue[runtime_key_read];
    runtime_key_read =
        (runtime_key_read + 1u) % runtime_key_queue_size;
    return true;
}

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

    unified_keyboard::init();
    picocalc::keyboard::set_lcd_backlight(160);

    picocalc::display::init();
    picocalc::display::set_text_color(0x00ff00, 0x000000);
    audio_init();
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
        // ANSI clear-screen + home for terminal consoles.
        serial_put_string_raw("\x1b[2J\x1b[H");
    }
}

void screen_put_char(char c) {
    picocalc::display::put_char(c);
}

void serial_put_char_raw(char c) {
    if (!serial_transfer_active() && console_uses_serial()) putchar_raw(c);
}

void serial_put_string_raw(const char* text) {
    if (!text) return;
    if (!serial_transfer_active() && console_uses_serial()) {
        const char* p=text; while (*p) putchar_raw(*p++);
    }
}

bool terminal_console_enabled(){return console_uses_serial();}
void begin_command_input(){command_input_active=true;command_input_source=CommandInputSource::None;}
void end_command_input(){
 command_input_active=false;
 if(command_input_source==CommandInputSource::Local)console_local_input();
 command_input_source=CommandInputSource::None;
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

void enter_sleep_mode(bool refresh_status) {
    const bool bluetooth_was_enabled = bluetooth_manager::enabled();
    wireless::suspend_board_led();
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

    // BIOS 1.7 can participate in standby through the keyboard controller,
    // but CPB must also stop the LCD controller itself. Older keyboard BIOS
    // revisions may clamp LCD brightness to a non-zero minimum, so the panel
    // sleep sequence is deliberately independent of that behavior.
    picocalc::keyboard::set_lcd_backlight(0);
    picocalc::keyboard::set_keyboard_backlight(0);
    picocalc::display::enter_standby();

    // Allow the triggering key to be released before wake polling begins.
    sleep_ms(180);

    while (true) {
        const int key = unified_keyboard::read_key();
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

    picocalc::display::leave_standby();
    picocalc::keyboard::set_lcd_backlight(
        have_lcd ? lcd_level : static_cast<unsigned char>(160)
    );
    if (have_keyboard) {
        picocalc::keyboard::set_keyboard_backlight(keyboard_level);
    }

    wireless::resume_board_led();

    // STANDBY intentionally cannot wake from Bluetooth, but the pre-sleep
    // radio state is restored and paired keyboards may reconnect.
    if (bluetooth_was_enabled) bluetooth_manager::enable();

    if (refresh_status && status_area_enabled() && status_refresh_callback) {
        status_refresh_callback(status_refresh_context);
    }

    serial_put_string_raw("[WAKE]\r\n");
}

void enter_bootsel() {
    wireless::suspend_board_led();
    if (sleep_prepare_callback) {
        sleep_prepare_callback(sleep_prepare_context);
    }

    // Enter the RP2350 ROM USB boot path without requiring the physical
    // RESET/BOOTSEL key sequence. The call does not normally return.
    reset_usb_boot(0, 0);
    while (true) {
        tight_loop_contents();
    }
}

void reboot_system() {
    wireless::suspend_board_led();
    if (sleep_prepare_callback) {
        sleep_prepare_callback(sleep_prepare_context);
    }

    // Use the watchdog reset path for a clean restart from flash.
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

void draw_text_row(
    int row,
    const char* text,
    std::uint32_t foreground,
    std::uint32_t background
) {
    picocalc::display::draw_text_row(row, text, foreground, background);
}

void draw_text_span(
    int row,
    int first_column,
    const char* text,
    int columns,
    std::uint32_t foreground,
    std::uint32_t background
) {
    picocalc::display::draw_text_span(
        row, first_column, text, columns, foreground, background);
}

bool get_battery_status(int& percent, bool& charging) {
    return picocalc::keyboard::read_battery(percent, charging);
}

InternalKeyboardDiagnostics get_internal_keyboard_diagnostics() {
    unsigned char bios_version = 0;
    const bool bios_reply_received =
        picocalc::keyboard::read_bios_version(bios_version);
    const auto& state = picocalc::keyboard::diagnostics();
    return {
        picocalc::keyboard::health_name(state.health),
        picocalc::keyboard::error_name(state.last_error),
        state.total_errors,
        state.consecutive_errors,
        state.recovery_attempts,
        state.recoveries,
        bios_reply_received,
        bios_version,
        picocalc::keyboard::startup_phase_name(state.startup_phase),
        state.ever_ready,
        state.sda_high,
        state.scl_high,
        state.first_try_recorded,
        state.first_ack_recorded,
        state.first_try_ms,
        state.first_ack_ms,
        state.startup_attempts
    };
}

bool caps_lock_enabled() {
    return unified_keyboard::caps_lock_enabled();
}

bool shift_held() {
    return unified_keyboard::shift_held();
}

namespace {

RtcDevice device_for_address(std::uint8_t address) {
    return address == rtc::default_address(RtcDevice::Ds3231)
        ? RtcDevice::Ds3231 : RtcDevice::Pcf8563;
}

bool external_rtc_read_at(
    RtcDevice device,
    std::uint8_t address,
    DateTime& out
) {
    std::uint8_t data[7] = {};
    if (picocalc::external_i2c::read_registers(
            address,
            rtc::first_register(device),
            data,
            sizeof(data)
        ) != picocalc::external_i2c::Result::Ok) {
        return false;
    }
    return rtc::decode(device, data, sizeof(data), out);
}

bool external_rtc_read(DateTime& out) {
    struct Candidate {
        RtcDevice device;
        std::uint8_t address;
    };
    const Candidate candidates[] = {
        {device_for_address(configured_rtc_address), configured_rtc_address},
        {RtcDevice::Pcf8563, rtc::default_address(RtcDevice::Pcf8563)},
        {RtcDevice::Ds3231, rtc::default_address(RtcDevice::Ds3231)}
    };

    for (std::size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (candidates[j].device == candidates[i].device &&
                candidates[j].address == candidates[i].address) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        DateTime candidate;
        if (!external_rtc_read_at(
                candidates[i].device,
                candidates[i].address,
                candidate)) {
            continue;
        }
        out = candidate;
        active_rtc_device = candidates[i].device;
        active_rtc_address = candidates[i].address;
        return true;
    }
    return false;
}

bool external_rtc_write(const DateTime& value) {
    if (active_rtc_device == RtcDevice::None || active_rtc_address == 0)
        return false;

    std::uint8_t payload[7] = {};
    if (!rtc::encode(
            active_rtc_device, value, payload, sizeof(payload))) {
        return false;
    }

    std::uint8_t packet[8] = {};
    packet[0] = rtc::first_register(active_rtc_device);
    for (std::size_t i = 0; i < sizeof(payload); ++i)
        packet[i + 1] = payload[i];

    if (picocalc::external_i2c::write_bytes(
            active_rtc_address,
            packet,
            sizeof(packet)
        ) != picocalc::external_i2c::Result::Ok) {
        return false;
    }

    DateTime verify;
    return external_rtc_read_at(
        active_rtc_device, active_rtc_address, verify);
}

bool probe_selected(DateTime& out) {
    hardware_rtc_probed = true;
    hardware_rtc_ok = false;
    active_rtc = ActiveRtc::None;
    active_rtc_device = RtcDevice::None;
    active_rtc_address = 0;

    if (configured_rtc_source != RtcSource::Off &&
        configured_rtc_source != RtcSource::Internal &&
        external_rtc_read(out)) {
        active_rtc = ActiveRtc::External;
        hardware_rtc_ok = true;
        return true;
    }

    picocalc::keyboard::RtcDateTime keyboard_time;
    if (configured_rtc_source != RtcSource::Off &&
        configured_rtc_source != RtcSource::External &&
        picocalc::keyboard::read_rtc(
            keyboard_time, configured_rtc_address)) {
        out = {
            keyboard_time.year,
            keyboard_time.month,
            keyboard_time.day,
            keyboard_time.hour,
            keyboard_time.minute,
            keyboard_time.second
        };
        active_rtc = ActiveRtc::Internal;
        active_rtc_device = RtcDevice::Pcf8563;
        active_rtc_address = configured_rtc_address;
        hardware_rtc_ok = true;
        return true;
    }
    return false;
}

} // namespace

void set_rtc_source(RtcSource source) {
    configured_rtc_source = source;
    next_hardware_rtc_probe_us = 0;
    hardware_rtc_probed = false;
    hardware_rtc_ok = false;
    active_rtc = ActiveRtc::None;
    active_rtc_device = RtcDevice::None;
    active_rtc_address = 0;
}

RtcSource rtc_source() { return configured_rtc_source; }

const char* rtc_source_name() {
    switch (configured_rtc_source) {
    case RtcSource::Auto: return "AUTO";
    case RtcSource::External: return "EXTERNAL";
    case RtcSource::Internal: return "INTERNAL";
    case RtcSource::Off: return "OFF";
    }
    return "AUTO";
}

bool set_rtc_address(std::uint8_t value) {
    if (value < 8 || value > 0x77) return false;
    configured_rtc_address = value;
    set_rtc_source(configured_rtc_source);
    return true;
}

std::uint8_t rtc_address() { return configured_rtc_address; }

bool probe_rtc() {
    DateTime value;
    const bool ok = probe_selected(value);
    if (ok) {
        set_software_clock(value);
        next_hardware_rtc_probe_us =
            to_us_since_boot(get_absolute_time()) + 60000000u;
    }
    return ok;
}

const char* rtc_location() {
    return active_rtc == ActiveRtc::External ? "EXT" :
           active_rtc == ActiveRtc::Internal ? "INT" : "OFF";
}

RtcDevice rtc_device() { return active_rtc_device; }

const char* rtc_device_name() {
    return rtc::device_name(active_rtc_device);
}

std::uint8_t rtc_active_address() {
    return active_rtc_address ? active_rtc_address : configured_rtc_address;
}

I2cResult external_i2c_read(
    std::uint8_t address,
    std::uint8_t reg,
    std::uint8_t& value
) {
    return static_cast<I2cResult>(
        picocalc::external_i2c::read_register(address, reg, value));
}

I2cResult external_i2c_write(
    std::uint8_t address,
    std::uint8_t reg,
    std::uint8_t value
) {
    return static_cast<I2cResult>(
        picocalc::external_i2c::write_register(address, reg, value));
}

int external_i2c_scan(std::uint8_t* addresses, int capacity) {
    int count = 0;
    picocalc::external_i2c::scan(addresses, capacity, count);
    return count;
}

const char* i2c_result_text(I2cResult result) {
    switch (result) {
    case I2cResult::Ok: return "OK";
    case I2cResult::Nack: return "I2C NACK";
    case I2cResult::Timeout: return "I2C TIMEOUT";
    case I2cResult::Busy: return "I2C BUSY";
    case I2cResult::BadAddress: return "BAD I2C ADDRESS";
    }
    return "I2C ERROR";
}

bool get_datetime(DateTime& out) {
    const uint64_t now = to_us_since_boot(get_absolute_time());
    if (now >= next_hardware_rtc_probe_us) {
        if (probe_selected(out)) {
            set_software_clock(out);
            next_hardware_rtc_probe_us = now + 60000000u;
            return true;
        }
        next_hardware_rtc_probe_us = now + 60000000u;
    }
    return read_software_clock(out);
}

bool set_datetime(const DateTime& value) {
    set_software_clock(value);
    if (!hardware_rtc_available()) return true;

    bool ok = false;
    if (active_rtc == ActiveRtc::External) {
        ok = external_rtc_write(value);
    } else if (active_rtc == ActiveRtc::Internal) {
        picocalc::keyboard::RtcDateTime keyboard_time{
            value.year, value.month, value.day,
            value.hour, value.minute, value.second
        };
        ok = picocalc::keyboard::write_rtc(
            keyboard_time, active_rtc_address);
    }
    hardware_rtc_ok = ok;
    return true;
}

bool hardware_rtc_available() {
    return hardware_rtc_probed && hardware_rtc_ok;
}

const char* datetime_last_error() {
    return hardware_rtc_available() ? "NONE" : "RTC NOT FOUND";
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

void set_cursor_visible(bool visible) {
    picocalc::display::set_cursor_visible(visible);
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

void graphics_text_locate(int x, int y) {
    graphics_text::set_cursor(x, y);
}

void graphics_text_print(const char* text) {
    if (!text) return;
    picocalc::display::set_function_key_bar_enabled(false);

    while (*text) {
        unsigned char character = static_cast<unsigned char>(*text++);
        if (character < graphics_text::first_character ||
            character > graphics_text::last_character) {
            character = '?';
        }

        const int x = graphics_text::cursor_x();
        const int y = graphics_text::cursor_y();
        const std::uint8_t* pixels = nullptr;
        const graphics_text::GlyphKind kind =
            graphics_text::glyph(static_cast<char>(character), pixels);

        if (kind == graphics_text::GlyphKind::Builtin) {
            picocalc::display::graphics_draw_builtin8(
                x, y, static_cast<char>(character), graphics_color());
        } else {
            picocalc::display::graphics_draw_glyph8(
                x,
                y,
                pixels,
                graphics_text::palette(),
                kind == graphics_text::GlyphKind::Indexed,
                graphics_color()
            );
        }
        graphics_text::advance();
    }
}

bool graphics_define(char character, const char* hex_pixels) {
    return graphics_text::define(character, hex_pixels) ==
        graphics_text::DefineResult::Ok;
}

void graphics_define_clear() {
    graphics_text::clear_definitions();
}

bool graphics_palette_rgb(int index, int red, int green, int blue) {
    return graphics_text::set_palette_rgb(index, red, green, blue);
}

bool graphics_palette_rgb24(int index, std::uint32_t rgb) {
    return graphics_text::set_palette_rgb24(index, rgb);
}

void graphics_palette_reset() {
    graphics_text::reset_palette();
}

std::uint32_t monotonic_millis() {
    return to_ms_since_boot(get_absolute_time());
}

std::uint64_t monotonic_micros() {
    return to_us_since_boot(get_absolute_time());
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
    if (!system_controls::cpu_profile_supported(mhz)) {
        return false;
    }
    if (rmb::psram::busy()) {
        return false;
    }
    if (usb_msc::active()) {
        return false;
    }

    if (cpu_clock_source_hz == 0) {
        cpu_clock_source_hz = clock_get_hz(clk_sys);
    }

    // Once the experimental profile has changed PLL_SYS to 200 MHz, do not
    // leave that PLL configuration behind for the rated profiles. In
    // particular, deriving 75 MHz from 200 MHz requires a fractional 8/3
    // clk_sys divider and has been observed to stop the PicoCalc display.
    // Restore the rated 150 MHz PLL first; 150, 100, and 75 MHz then use the
    // same proven source/divider combinations as a fresh boot.
    if (mhz != 200u && cpu_clock_source_hz > 150000000u) {
        // Reprogramming PLL_SYS after CYW43's PIO transport has been
        // initialized is unsafe. Fail the requested change rather than
        // risking a system or display lockup; reboot remains the recovery
        // path and starts at 150 MHz.
        if (wireless::initialized()) {
            return false;
        }

        const std::uint32_t previous_source_hz = cpu_clock_source_hz;
        const std::uint32_t usb_pll_hz = clock_get_hz(clk_usb);

        clock_configure_undivided(
            clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
            usb_pll_hz
        );

        if (!set_sys_clock_khz(150000u, false)) {
            clock_configure_undivided(
                clk_peri,
                0,
                CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                previous_source_hz
            );
            return false;
        }

        cpu_clock_source_hz = clock_get_hz(clk_sys);
        if (cpu_clock_source_hz != 150000000u) {
            return false;
        }

        clock_configure_undivided(
            clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
            cpu_clock_source_hz
        );
        peripheral_clock_isolated = true;
    }

    // Entering 200 MHz is the only transition that raises PLL_SYS.
    // Never do that after CYW43 has been initialized: its PIO transport was
    // configured against the previous clk_sys. Select 200 MHz first, then
    // enable the board LED, Wi-Fi, or Bluetooth.
    if (mhz == 200u && cpu_clock_source_hz < 200000000u) {
        if (wireless::initialized()) {
            return false;
        }

        uint vco_hz = 0;
        uint post_div1 = 0;
        uint post_div2 = 0;
        if (!check_sys_clock_khz(
                200000u, &vco_hz, &post_div1, &post_div2)) {
            return false;
        }

        const std::uint32_t previous_source_hz = cpu_clock_source_hz;
        const std::uint32_t usb_pll_hz = clock_get_hz(clk_usb);

        // Keep peripheral clocks alive while PLL_SYS is reprogrammed. No
        // storage or CYW43 transaction is allowed across this short window.
        clock_configure_undivided(
            clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
            usb_pll_hz
        );

        // The SDK chooses the validated PLL parameters. VREG is deliberately
        // left untouched: 200 MHz remains an experimental session-only mode.
        if (!set_sys_clock_khz(200000u, false)) {
            clock_configure_undivided(
                clk_peri,
                0,
                CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                previous_source_hz
            );
            return false;
        }

        cpu_clock_source_hz = clock_get_hz(clk_sys);
        if (cpu_clock_source_hz < 200000000u) {
            return false;
        }

        // LCD/SD SPI and UART were initialized from a 150 MHz peripheral
        // clock. RP2350's 16-bit fractional divider keeps clk_peri at the same
        // reported rate while PLL_SYS supplies 200 MHz to clk_sys.
        if (!clock_configure(
                clk_peri,
                0,
                CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                cpu_clock_source_hz,
                150000000u)) {
            return false;
        }
        peripheral_clock_isolated = true;
    }

    const std::uint32_t source_mhz = cpu_clock_source_hz / 1000000u;
    if (source_mhz < mhz || source_mhz == 0) {
        return false;
    }

    // Before the first overclock request, retain the existing 150 MHz PLL and
    // isolate clk_peri from later clk_sys division.
    if (!peripheral_clock_isolated) {
        clock_configure_undivided(
            clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
            cpu_clock_source_hz
        );
        peripheral_clock_isolated = true;
    }

    if (!clock_configure_mhz(
            clk_sys,
            CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
            CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
            source_mhz,
            mhz)) {
        return false;
    }

    // RP2350 I2C and PWM audio timing follow clk_sys. Recompute their SDK
    // divisors after every profile change; clk_usb and clk_peri remain stable.
    picocalc::keyboard::reconfigure_bus_clock();
    picocalc::external_i2c::reconfigure_bus_clock();
    audio_reconfigure_clock();
    psram::clock_changed();

    return true;
}

namespace {

int poll_runtime_terminal_byte(){
 if(runtime_terminal_source==RuntimeTerminalSource::Serial)return console_uses_serial()?console_serial_read(0,true):-1;
 int v=console_uses_serial()?console_serial_read(0):-1;
 if(v>=0){runtime_terminal_source=RuntimeTerminalSource::Serial;return v;}
 return v;
}
RuntimeKeyResult poll_runtime_terminal(){
 const std::uint32_t now=to_ms_since_boot(get_absolute_time());
 int c=poll_runtime_terminal_byte();
 if(c<0){
  if(runtime_serial_state==RuntimeSerialState::Escape&&static_cast<std::int32_t>(now-runtime_serial_deadline_ms)>=0){runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;runtime_terminal_source=RuntimeTerminalSource::None;return runtime_break();}
  return no_runtime_key();
 }
 auto& filter=serial_crlf_filter;
 if(runtime_serial_state==RuntimeSerialState::Idle){
  if(filter.should_ignore(c)){runtime_terminal_source=RuntimeTerminalSource::None;return no_runtime_key();}
  if(c==0x03){runtime_terminal_source=RuntimeTerminalSource::None;return runtime_break();}
  if(c!=0x1b){runtime_terminal_source=RuntimeTerminalSource::None;if(c==0x7f)c=0x08;return runtime_key(c);}
  runtime_serial_state=RuntimeSerialState::Escape;runtime_serial_sequence[0]=static_cast<char>(c);runtime_serial_length=1;runtime_serial_deadline_ms=now+25u;return no_runtime_key();
 }
 if(runtime_serial_length<sizeof(runtime_serial_sequence))runtime_serial_sequence[runtime_serial_length++]=static_cast<char>(c);
 else{runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;runtime_terminal_source=RuntimeTerminalSource::None;return runtime_break();}
 runtime_serial_deadline_ms=now+25u;
 const char* q=runtime_serial_sequence;const std::size_t n=runtime_serial_length;int decoded=-1;bool complete=false,valid=false;
 if(n>=2&&q[1]=='O'){valid=n==2;if(n==3&&q[2]>='P'&&q[2]<='S'){decoded=0x81+(q[2]-'P');complete=true;}}
 else if(n>=2&&q[1]=='['){
  valid=n==2;
  if(n==3){switch(q[2]){case 'A':decoded=0xb5;complete=true;break;case 'B':decoded=0xb6;complete=true;break;case 'C':decoded=0xb7;complete=true;break;case 'D':decoded=0xb4;complete=true;break;case 'H':decoded=0xd2;complete=true;break;case 'F':decoded=0xd5;complete=true;break;case '1':case '2':case '3':valid=true;break;default:break;}}
  else if(n==4&&q[2]=='3'&&q[3]=='~'){decoded=0xd4;complete=true;}
  else if(n==4&&(q[2]=='1'||q[2]=='2')&&q[3]>='0'&&q[3]<='9')valid=true;
  else if(n==5&&q[4]=='~'){const int code=(q[2]-'0')*10+(q[3]-'0');if(code==15)decoded=0x85;else if(code==17)decoded=0x86;else if(code==18)decoded=0x87;else if(code==19)decoded=0x88;else if(code==20)decoded=0x89;else if(code==21)decoded=0x90;complete=decoded>=0;}
 }
 if(complete){runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;runtime_terminal_source=RuntimeTerminalSource::None;return runtime_key(decoded);}
 if(valid)return no_runtime_key();
 runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;runtime_terminal_source=RuntimeTerminalSource::None;return runtime_break();
}

RuntimeKeyResult poll_runtime_source() {
    if (serial_transfer_active()) return no_runtime_key();
    if (background_service_callback)
        background_service_callback(background_service_context);

    const RuntimeKeyResult terminal = poll_runtime_terminal();
    if (terminal.type != RuntimeKeyType::None) return terminal;

    // PicoCalc keyboard reads use the 10 kHz I2C controller and include a
    // short settling delay. Runtime input and BREAK share the same 5 Hz poll,
    // while USB/UART remains non-blocking on every call.
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    if (static_cast<int32_t>(now - next_runtime_keyboard_poll_ms) < 0 &&
        !picocalc::keyboard::poll_pending()) {
        return no_runtime_key();
    }

    next_runtime_keyboard_poll_ms = now + 200;

    const int key = unified_keyboard::read_key();
    // read_key() updates modifier/Caps state internally. Runtime input does
    // not redraw the status area because a full-screen graphics program may
    // be paused underneath it; the REPL refreshes status after RUN returns.
    if (key == picocalc::keyboard::key_hotkey_screenshot) {
        if (screenshot_callback) {
            screenshot_callback(screenshot_context);
        }
        return no_runtime_key();
    }
    if (key == picocalc::keyboard::key_hotkey_sleep) {
        enter_sleep_mode(false);
        return no_runtime_key();
    }
    if (key == 0xc1) {
        return no_runtime_key();
    }
    if (key == 0x03 || key == 0xb1 || key == 0xd0) {
        return runtime_break();
    }
    return key >= 0 ? runtime_key(key) : no_runtime_key();
}

} // namespace

RuntimeKeyResult poll_runtime_key() {
    int queued = 0;
    if (dequeue_runtime_key(queued)) return runtime_key(queued);
    return poll_runtime_source();
}

RuntimeKeyResult wait_runtime_key() {
    while (true) {
        const RuntimeKeyResult result = poll_runtime_key();
        if (result.type != RuntimeKeyType::None) return result;
        sleep_ms(10);
    }
}

void reset_runtime_input() {
    runtime_key_read = 0;
    runtime_key_write = 0;
    runtime_serial_state = RuntimeSerialState::Idle;
    runtime_serial_length = 0;
    runtime_terminal_source = RuntimeTerminalSource::None;
    command_input_source = CommandInputSource::None;
    next_runtime_keyboard_poll_ms = 0;
}

bool break_requested() {
    // The VM calls this cooperatively every RMB_BREAK_DISPATCH_INTERVAL
    // dispatches. Service background work here as well as checking BREAK so
    // asynchronous audio, networking and Bluetooth keep progressing while a
    // BASIC program is running.
    if (background_service_callback) {
        background_service_callback(background_service_context);
    }
    const RuntimeKeyResult result = poll_runtime_source();
    if (result.type == RuntimeKeyType::Key) {
        queue_runtime_key(result.code);
        return false;
    }
    return result.type == RuntimeKeyType::Break;
}

int read_command_source(CommandInputSource source,unsigned timeout_us){
 if(source==CommandInputSource::Serial)return console_uses_serial()?console_serial_read(timeout_us,true):-1;
 return -1;
}
int decode_terminal_key(int c,CommandInputSource source){
 if(c==0x7f)c=0x08;if(c!=0x1b)return c;
 const int c2=read_command_source(source,30000);
 if(c2=='O'){const int c3=read_command_source(source,30000);return c3>='P'&&c3<='S'?0x81+(c3-'P'):0x1b;}
 if(c2!='[')return 0x1b;const int c3=read_command_source(source,30000);
 if(c3=='A')return 0xb5;if(c3=='B')return 0xb6;if(c3=='C')return 0xb7;if(c3=='D')return 0xb4;if(c3=='H')return 0xd2;if(c3=='F')return 0xd5;
 if(c3=='3')return read_command_source(source,30000)=='~'?0xd4:0x1b;
 if(c3=='5')return read_command_source(source,30000)=='~'?0xd6:0x1b;
 if(c3=='6')return read_command_source(source,30000)=='~'?0xd7:0x1b;
 if(c3=='1'||c3=='2'){const int c4=read_command_source(source,30000),c5=read_command_source(source,30000);if(c4>='0'&&c4<='9'&&c5=='~'){const int code=(c3-'0')*10+(c4-'0');if(code==15)return 0x85;if(code==17)return 0x86;if(code==18)return 0x87;if(code==19)return 0x88;if(code==20)return 0x89;if(code==21)return 0x90;}}
 return 0x1b;
}
void collect_navigation_burst(
    int key,
    NavigationStepCallback callback,
    void* context
) {
    if (!callback || !picocalc::keyboard::navigation_key_held(key)) return;

    bool saw_repeat = false;
    std::uint32_t last_activity = to_ms_since_boot(get_absolute_time());
    while (picocalc::keyboard::navigation_key_held(key)) {
        if (background_service_callback) {
            background_service_callback(background_service_context);
        }

        const int next = unified_keyboard::read_key();
        if (next == key) {
            callback(next, context);
            saw_repeat = true;
            last_activity = to_ms_since_boot(get_absolute_time());
        } else if (next >= 0) {
            // Preserve a key pressed during the navigation burst. The matching
            // release normally ends the loop before the following key is read.
            if (pending_command_key < 0) pending_command_key = next;
            break;
        }

        const std::uint32_t now = to_ms_since_boot(get_absolute_time());
        const std::uint32_t idle_limit_ms = saw_repeat ? 200u : 700u;
        if (static_cast<std::uint32_t>(now - last_activity) >= idle_limit_ms) {
            break;
        }
        sleep_ms(1);
    }
}

int get_char(){
 if(serial_transfer_active())return -1;constexpr uint32_t blink_ms=500;bool cursor_visible=true;if(console_uses_lcd())picocalc::display::set_cursor_visible(true);uint32_t next_blink=to_ms_since_boot(get_absolute_time())+blink_ms;static bool last_shift_held=false;
 while(true){
  // Poll USB/UART before I2C work and background callbacks.
  int c=-1;CommandInputSource source=command_input_source;
  // A lost built-in controller must not leave a half-entered local line
  // permanently owning the console. Serial can take over that line.
  if(source==CommandInputSource::Local &&
     picocalc::keyboard::diagnostics().health==picocalc::keyboard::Health::Lost)
      source=CommandInputSource::None;
  if(source==CommandInputSource::None||source==CommandInputSource::Serial){c=console_uses_serial()?console_serial_read(0,source==CommandInputSource::Serial):-1;if(c>=0)source=CommandInputSource::Serial;}
  if(c>=0){auto& filter=serial_crlf_filter;if(filter.should_ignore(c))continue;if(command_input_active)command_input_source=source;c=decode_terminal_key(c,source);if(console_uses_lcd())picocalc::display::set_cursor_visible(false);return c;}
  if(background_service_callback)background_service_callback(background_service_context);
  int key=-1;if(pending_command_key>=0){key=pending_command_key;pending_command_key=-1;}else if(command_input_source==CommandInputSource::None||command_input_source==CommandInputSource::Local)key=unified_keyboard::read_key();
  const bool shift=unified_keyboard::shift_held();if(shift!=last_shift_held){last_shift_held=shift;if(status_refresh_callback)status_refresh_callback(status_refresh_context);}
  if(key==picocalc::keyboard::key_hotkey_screenshot){if(screenshot_callback)screenshot_callback(screenshot_context);continue;}
  if(key==picocalc::keyboard::key_hotkey_sleep){if(console_uses_lcd())picocalc::display::set_cursor_visible(false);enter_sleep_mode();cursor_visible=true;if(console_uses_lcd())picocalc::display::set_cursor_visible(true);next_blink=to_ms_since_boot(get_absolute_time())+blink_ms;continue;}
  if(key==0xc1){if(status_refresh_callback)status_refresh_callback(status_refresh_context);continue;}
  if(key>=0){audio_key_click();if(command_input_active&&command_input_source==CommandInputSource::None)command_input_source=CommandInputSource::Local;if(console_uses_lcd())picocalc::display::set_cursor_visible(false);return key;}
  const uint32_t now=to_ms_since_boot(get_absolute_time());if(static_cast<int32_t>(now-next_blink)>=0){cursor_visible=!cursor_visible;if(console_uses_lcd())picocalc::display::set_cursor_visible(cursor_visible);next_blink=now+blink_ms;}sleep_ms(4);
 }
}

} // namespace rmb::platform
