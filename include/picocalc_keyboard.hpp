#pragma once

namespace rmb::picocalc::keyboard {

constexpr int key_hotkey_screenshot = 0xe2;
constexpr int key_hotkey_sleep = 0xe3;
constexpr int key_power = 0x91;

struct RtcDateTime {
    int year = 2000;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

enum class RtcError {
    None,
    NotInitialized,
    InvalidValue,
    PointerWriteFailed,
    ReadFailed,
    WriteFailed,
    ReadbackFailed,
    ReadbackMismatch
};

void init();
void reconfigure_bus_clock();
int read_key();
bool set_lcd_backlight(unsigned char value);
bool get_lcd_backlight(unsigned char& value);
bool set_keyboard_backlight(unsigned char value);
bool get_keyboard_backlight(unsigned char& value);
bool read_battery(int& percent, bool& charging);
bool caps_lock_enabled();
bool shift_held();
bool read_rtc(RtcDateTime& value);
bool write_rtc(const RtcDateTime& value);
RtcError last_rtc_error();

} // namespace rmb::picocalc::keyboard
