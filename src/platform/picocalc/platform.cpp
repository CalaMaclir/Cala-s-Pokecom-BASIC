#include "platform.hpp"
#include "picocalc_display.hpp"
#include "picocalc_keyboard.hpp"
#include "external_i2c.hpp"
#include "graphics_text.hpp"
#include "bluetooth_serial.hpp"
#include "serial_transfer.hpp"
#include "serial_crlf_filter.hpp"

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
detail::SerialCrLfFilter bluetooth_crlf_filter;
enum class CommandInputSource : std::uint8_t { None, Local, Serial, Bluetooth };
CommandInputSource command_input_source = CommandInputSource::None;
bool command_input_active = false;
enum class RuntimeTerminalSource : std::uint8_t { None, Serial, Bluetooth };
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

    picocalc::keyboard::init();
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

    if (console_uses_serial() || bluetooth_serial::console_enabled()) {
        // ANSI clear-screen + home for terminal consoles.
        serial_put_string_raw("\x1b[2J\x1b[H");
    }
}

void screen_put_char(char c) {
    picocalc::display::put_char(c);
}

void serial_put_char_raw(char c) {
    if (!serial_transfer_active() && console_uses_serial()) putchar_raw(c);
    if (bluetooth_serial::console_enabled()) bluetooth_serial::write_console_char(c);
}

void serial_put_string_raw(const char* text) {
    if (!text) return;
    if (!serial_transfer_active() && console_uses_serial()) {
        const char* p=text; while (*p) putchar_raw(*p++);
    }
    if (bluetooth_serial::console_enabled()) bluetooth_serial::write_console_text(text);
}

bool terminal_console_enabled(){return console_uses_serial()||bluetooth_serial::console_enabled();}
void begin_command_input(){command_input_active=true;command_input_source=CommandInputSource::None;}
void end_command_input(){
 command_input_active=false;
 if(command_input_source==CommandInputSource::Local)console_local_input();
 else if(command_input_source==CommandInputSource::Bluetooth)console_bluetooth_input();
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

    if (refresh_status && status_area_enabled() && status_refresh_callback) {
        status_refresh_callback(status_refresh_context);
    }

    serial_put_string_raw("[WAKE]\r\n");
}

void enter_bootsel() {
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

bool get_battery_status(int& percent, bool& charging) {
    return picocalc::keyboard::read_battery(percent, charging);
}

bool caps_lock_enabled() {
    return picocalc::keyboard::caps_lock_enabled();
}

bool shift_held() {
    return picocalc::keyboard::shift_held();
}

namespace {
std::uint8_t bcd(std::uint8_t v){return static_cast<std::uint8_t>((v>>4)*10+(v&15));}
std::uint8_t tobcd(int v){return static_cast<std::uint8_t>(((v/10)<<4)|(v%10));}
bool external_rtc_read(DateTime& out){std::uint8_t d[7]={};if(picocalc::external_i2c::read_registers(configured_rtc_address,2,d,7)!=picocalc::external_i2c::Result::Ok)return false;DateTime x{2000+bcd(d[6]),bcd(d[5]&31),bcd(d[3]&63),bcd(d[2]&63),bcd(d[1]&127),bcd(d[0]&127)};if(x.month<1||x.month>12||x.day<1||x.day>31||x.hour>23||x.minute>59||x.second>59)return false;out=x;return true;}
bool external_rtc_write(const DateTime& v){const std::uint8_t d[10]={0,0,0,tobcd(v.second),tobcd(v.minute),tobcd(v.hour),tobcd(v.day),1,tobcd(v.month),tobcd(v.year-2000)};return picocalc::external_i2c::write_bytes(configured_rtc_address,d,10)==picocalc::external_i2c::Result::Ok;}
bool probe_selected(DateTime& out){hardware_rtc_probed=true;hardware_rtc_ok=false;active_rtc=ActiveRtc::None;if(configured_rtc_source!=RtcSource::Off&&configured_rtc_source!=RtcSource::Internal&&external_rtc_read(out)){active_rtc=ActiveRtc::External;hardware_rtc_ok=true;return true;}picocalc::keyboard::RtcDateTime k;if(configured_rtc_source!=RtcSource::Off&&configured_rtc_source!=RtcSource::External&&picocalc::keyboard::read_rtc(k,configured_rtc_address)){out={k.year,k.month,k.day,k.hour,k.minute,k.second};active_rtc=ActiveRtc::Internal;hardware_rtc_ok=true;return true;}return false;}
}
void set_rtc_source(RtcSource source){configured_rtc_source=source;next_hardware_rtc_probe_us=0;hardware_rtc_probed=false;hardware_rtc_ok=false;active_rtc=ActiveRtc::None;}
RtcSource rtc_source(){return configured_rtc_source;}
const char* rtc_source_name(){switch(configured_rtc_source){case RtcSource::Auto:return "AUTO";case RtcSource::External:return "EXTERNAL";case RtcSource::Internal:return "INTERNAL";case RtcSource::Off:return "OFF";}return "AUTO";}
bool set_rtc_address(std::uint8_t v){if(v<8||v>0x77)return false;configured_rtc_address=v;set_rtc_source(configured_rtc_source);return true;}
std::uint8_t rtc_address(){return configured_rtc_address;}
bool probe_rtc(){DateTime d;const bool ok=probe_selected(d);if(ok){set_software_clock(d);next_hardware_rtc_probe_us=to_us_since_boot(get_absolute_time())+60000000u;}return ok;}
const char* rtc_location(){return active_rtc==ActiveRtc::External?"EXT":active_rtc==ActiveRtc::Internal?"INT":"OFF";}
I2cResult external_i2c_read(std::uint8_t a,std::uint8_t r,std::uint8_t& v){return static_cast<I2cResult>(picocalc::external_i2c::read_register(a,r,v));}
I2cResult external_i2c_write(std::uint8_t a,std::uint8_t r,std::uint8_t v){return static_cast<I2cResult>(picocalc::external_i2c::write_register(a,r,v));}
int external_i2c_scan(std::uint8_t* a,int cap){int count=0;picocalc::external_i2c::scan(a,cap,count);return count;}
const char* i2c_result_text(I2cResult x){switch(x){case I2cResult::Ok:return "OK";case I2cResult::Nack:return "I2C NACK";case I2cResult::Timeout:return "I2C TIMEOUT";case I2cResult::Busy:return "I2C BUSY";case I2cResult::BadAddress:return "BAD I2C ADDRESS";}return "I2C ERROR";}
bool get_datetime(DateTime& out){const uint64_t now=to_us_since_boot(get_absolute_time());if(now>=next_hardware_rtc_probe_us){if(probe_selected(out)){set_software_clock(out);next_hardware_rtc_probe_us=now+60000000u;return true;}next_hardware_rtc_probe_us=now+60000000u;}return read_software_clock(out);}
bool set_datetime(const DateTime& v){set_software_clock(v);if(!hardware_rtc_available())return true;bool ok=false;if(active_rtc==ActiveRtc::External)ok=external_rtc_write(v);else if(active_rtc==ActiveRtc::Internal){picocalc::keyboard::RtcDateTime k{v.year,v.month,v.day,v.hour,v.minute,v.second};ok=picocalc::keyboard::write_rtc(k,configured_rtc_address);}hardware_rtc_ok=ok;return true;}
bool hardware_rtc_available(){return hardware_rtc_probed&&hardware_rtc_ok;}
const char* datetime_last_error(){return hardware_rtc_available()?"NONE":"RTC NOT FOUND";}
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
    if (bluetooth_serial::console_enabled()) bluetooth_serial::write_console_char(c);
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
    if (bluetooth_serial::console_enabled()) bluetooth_serial::write_console_text(text);
}

void clear_to_eol() {
    if (console_uses_lcd()) {
        picocalc::display::clear_to_eol();
    }

    if (console_uses_serial() || bluetooth_serial::console_enabled()) {
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
    picocalc::external_i2c::reconfigure_bus_clock();
    audio_reconfigure_clock();

    return true;
}

namespace {

int poll_runtime_terminal_byte(){
 if(runtime_terminal_source==RuntimeTerminalSource::Serial)return console_uses_serial()?console_serial_read(0,true):-1;
 if(runtime_terminal_source==RuntimeTerminalSource::Bluetooth){
  if(!bluetooth_serial::console_enabled()||!bluetooth_serial::connected()){runtime_terminal_source=RuntimeTerminalSource::None;runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;return -1;}
  return bluetooth_serial::read_console();
 }
 int v=console_uses_serial()?console_serial_read(0):-1;
 if(v>=0){runtime_terminal_source=RuntimeTerminalSource::Serial;return v;}
 v=bluetooth_serial::console_enabled()?bluetooth_serial::read_console():-1;
 if(v>=0)runtime_terminal_source=RuntimeTerminalSource::Bluetooth;
 return v;
}
RuntimeKeyResult poll_runtime_terminal(){
 const std::uint32_t now=to_ms_since_boot(get_absolute_time());
 int c=poll_runtime_terminal_byte();
 if(c<0){
  if(runtime_serial_state==RuntimeSerialState::Escape&&static_cast<std::int32_t>(now-runtime_serial_deadline_ms)>=0){runtime_serial_state=RuntimeSerialState::Idle;runtime_serial_length=0;runtime_terminal_source=RuntimeTerminalSource::None;return runtime_break();}
  return no_runtime_key();
 }
 auto& filter=runtime_terminal_source==RuntimeTerminalSource::Bluetooth?bluetooth_crlf_filter:serial_crlf_filter;
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

    if (static_cast<int32_t>(now - next_runtime_keyboard_poll_ms) < 0) {
        return no_runtime_key();
    }

    next_runtime_keyboard_poll_ms = now + 200;

    const int key = picocalc::keyboard::read_key();
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
 if(source!=CommandInputSource::Bluetooth||!bluetooth_serial::console_enabled()||!bluetooth_serial::connected())return -1;
 const auto deadline=make_timeout_time_us(timeout_us);do{const int v=bluetooth_serial::read_console();if(v>=0)return v;if(timeout_us)sleep_us(50);}while(!time_reached(deadline));return -1;
}
int decode_terminal_key(int c,CommandInputSource source){
 if(c==0x7f)c=0x08;if(c!=0x1b)return c;
 const int c2=read_command_source(source,30000);
 if(c2=='O'){const int c3=read_command_source(source,30000);return c3>='P'&&c3<='S'?0x81+(c3-'P'):0x1b;}
 if(c2!='[')return 0x1b;const int c3=read_command_source(source,30000);
 if(c3=='A')return 0xb5;if(c3=='B')return 0xb6;if(c3=='C')return 0xb7;if(c3=='D')return 0xb4;if(c3=='H')return 0xd2;if(c3=='F')return 0xd5;
 if(c3=='3')return read_command_source(source,30000)=='~'?0xd4:0x1b;
 if(c3=='1'||c3=='2'){const int c4=read_command_source(source,30000),c5=read_command_source(source,30000);if(c4>='0'&&c4<='9'&&c5=='~'){const int code=(c3-'0')*10+(c4-'0');if(code==15)return 0x85;if(code==17)return 0x86;if(code==18)return 0x87;if(code==19)return 0x88;if(code==20)return 0x89;if(code==21)return 0x90;}}
 return 0x1b;
}
int get_char(){
 if(serial_transfer_active())return -1;constexpr uint32_t blink_ms=500;bool cursor_visible=true;if(console_uses_lcd())picocalc::display::set_cursor_visible(true);uint32_t next_blink=to_ms_since_boot(get_absolute_time())+blink_ms;static bool last_shift_held=false;
 while(true){
  if(background_service_callback)background_service_callback(background_service_context);
  if(command_input_source==CommandInputSource::Bluetooth&&(!bluetooth_serial::console_enabled()||!bluetooth_serial::connected()))command_input_source=CommandInputSource::None;
  int key=-1;if(command_input_source==CommandInputSource::None||command_input_source==CommandInputSource::Local)key=picocalc::keyboard::read_key();
  const bool shift=picocalc::keyboard::shift_held();if(shift!=last_shift_held){last_shift_held=shift;if(status_refresh_callback)status_refresh_callback(status_refresh_context);}
  if(key==picocalc::keyboard::key_hotkey_screenshot){if(screenshot_callback)screenshot_callback(screenshot_context);continue;}
  if(key==picocalc::keyboard::key_hotkey_sleep){if(console_uses_lcd())picocalc::display::set_cursor_visible(false);enter_sleep_mode();cursor_visible=true;if(console_uses_lcd())picocalc::display::set_cursor_visible(true);next_blink=to_ms_since_boot(get_absolute_time())+blink_ms;continue;}
  if(key==0xc1){if(status_refresh_callback)status_refresh_callback(status_refresh_context);continue;}
  if(key>=0){audio_key_click();if(command_input_active&&command_input_source==CommandInputSource::None)command_input_source=CommandInputSource::Local;if(console_uses_lcd())picocalc::display::set_cursor_visible(false);return key;}
  int c=-1;CommandInputSource source=command_input_source;
  if(source==CommandInputSource::None||source==CommandInputSource::Serial){c=console_uses_serial()?console_serial_read(0,source==CommandInputSource::Serial):-1;if(c>=0)source=CommandInputSource::Serial;}
  if(c<0&&(command_input_source==CommandInputSource::None||command_input_source==CommandInputSource::Bluetooth)){c=bluetooth_serial::console_enabled()?bluetooth_serial::read_console():-1;if(c>=0)source=CommandInputSource::Bluetooth;}
  if(c>=0){auto& filter=source==CommandInputSource::Bluetooth?bluetooth_crlf_filter:serial_crlf_filter;if(filter.should_ignore(c))continue;if(command_input_active&&command_input_source==CommandInputSource::None)command_input_source=source;c=decode_terminal_key(c,source);if(console_uses_lcd())picocalc::display::set_cursor_visible(false);return c;}
  const uint32_t now=to_ms_since_boot(get_absolute_time());if(static_cast<int32_t>(now-next_blink)>=0){cursor_visible=!cursor_visible;if(console_uses_lcd())picocalc::display::set_cursor_visible(cursor_visible);next_blink=now+blink_ms;}sleep_ms(4);
 }
}

} // namespace rmb::platform
