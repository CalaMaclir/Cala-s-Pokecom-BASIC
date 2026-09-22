// Host-only regression test. No Pico SDK or hardware is required.
// g++ -std=c++17 -ffunction-sections -fdata-sections -Iinclude \
//   tests/status_layout_test.cpp -Wl,--gc-sections -o /tmp/status-test
#include <cassert>
#include <string>
#include <array>
#define private public
#include "repl.hpp"
#undef private
#include "../src/core/repl.cpp"

namespace {
std::array<std::string, 40> rows;
std::array<std::uint32_t, 40> foregrounds, backgrounds;
bool status_on = true, footer_on = true, caps = false;
bool battery_ok = true, charging = true, rtc_ok = true;
bool card = true, mounted = true, connected = false;
rmb::storage::Owner sd_owner = rmb::storage::Owner::Firmware;
int battery = 87, cursor = 0;
std::uint32_t clock_hz = 75000000;
}
namespace rmb::platform {
void set_status_area_enabled(bool b) { status_on = b; }
void set_function_key_bar_enabled(bool b) { footer_on = b; }
bool function_key_bar_enabled() { return footer_on; }
bool shift_held() { return false; }
bool caps_lock_enabled() { return caps; }
bool get_battery_status(int& p, bool& c) {
    p = battery; c = charging; return battery_ok;
}
bool get_datetime(DateTime& d) { d = {2026, 9, 21, 15, 44, 32}; return rtc_ok; }
std::uint32_t system_clock_hz() { return clock_hz; }
int cursor_row() { return cursor; }
void set_cursor_position(int, int row) { cursor = row; }
void clear_lcd_color(std::uint32_t) { rows.fill(""); }
void clear_lcd() { rows.fill(""); }
void draw_text_row(int row, const char* text, std::uint32_t fg, std::uint32_t bg) {
    assert(row >= 0 && row < 40);
    assert(std::strlen(text) <= 53);
    rows[row] = text;
    foregrounds[row] = fg; backgrounds[row] = bg;
}
}
namespace rmb::storage {
Owner owner() { return sd_owner; }
bool card_present() { return card; }
bool available() { return mounted; }
}
namespace rmb::network { bool connected() { return ::connected; } }

int main() {
    static rmb::Repl repl;
    static_assert(rmb::console_layout::status_rows == 3);
    std::strcpy(repl.current_filename_, "PICOCALC_MAND.BAS");
    repl.program_dirty_ = true;
    repl.last_run_ms_ = 4709;
    repl.render_status();
    assert(rows[0].find("PICOCALC_MAND.BAS*") != std::string::npos);
    assert(rows[0].find("BAT: 87%+") != std::string::npos);
    assert(rows[1] == "2026-09-21 15:44:32 WiFi:- CAPS:a");
    assert(rows[2] == "CPU:ECO 75MHz CON:BOTH RUN:4.709s");
    sd_owner = rmb::storage::Owner::UsbHost;
    repl.render_status();
    assert(rows[0].find("SD:USB") != std::string::npos);
    sd_owner = rmb::storage::Owner::Firmware;
    for (int theme = 0; theme < 3; ++theme) {
        repl.settings_.theme = theme;
        repl.render_status();
        assert(backgrounds[0] != 0);
        for (int i = 1; i < 3; ++i) {
            assert(backgrounds[i] == backgrounds[0]);
            assert(foregrounds[i] == foregrounds[0]);
        }
    }
    for (auto hz : {75000000u, 100000000u, 150000000u}) {
        clock_hz = hz;
        for (auto mode : {rmb::platform::ConsoleMode::Lcd,
                          rmb::platform::ConsoleMode::Serial,
                          rmb::platform::ConsoleMode::Both}) {
            repl.settings_.console_mode = mode;
            repl.last_run_ms_ = 0xffffffffu;
            repl.render_status(); // Includes maximum duration/width.
            assert(rows[2].find(hz == 75000000 ? "CPU:ECO 75MHz" :
                   hz == 100000000 ? "CPU:NORMAL 100MHz" :
                   "CPU:FULL 150MHz") == 0);
        }
    }
    std::memset(repl.current_filename_, 'X', 79);
    repl.current_filename_[79] = 0;
    repl.render_status();
    assert(rows[0].find("XXXXXXXXXXXXXXXXXX*") != std::string::npos);
    repl.settings_.wifi_enabled = true;
    caps = true;
    repl.render_status();
    assert(rows[1].find("WiFi:* CAPS:A") != std::string::npos);
    connected = true;
    repl.render_status();
    assert(rows[1].find("WiFi:+") != std::string::npos);
    battery_ok = rtc_ok = mounted = false;
    repl.render_status();
    assert(rows[0].find("SD:ERR") != std::string::npos);
    assert(rows[0].find("BAT: --") != std::string::npos);
    card = false;
    repl.render_status();
    assert(rows[0].find("SD:NO") != std::string::npos);
    for (bool enabled : {true, false, true}) {
        repl.settings_.status_enabled = enabled;
        repl.draw_menu_header(nullptr, nullptr);
        repl.draw_menu_header("SYSTEM MENU", "ENTER SELECT");
        int top = enabled ? 3 : 0;
        assert(rows[top] == "SYSTEM MENU");
        assert(rows[top + 1] == "ENTER SELECT");
        assert(rows[39].size() == 53);
        repl.leave_menu_screen();
        assert(cursor == top);
        assert(status_on == enabled);
        for (cursor = 0; cursor < top; ++cursor) {
            repl.ensure_body_cursor();
            assert(cursor == top);
        }
    }
    std::puts("status layout tests passed");
}
