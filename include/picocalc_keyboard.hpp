#pragma once
#include "input_hotkeys.hpp"

#include "keyboard_i2c_recovery.hpp"

namespace rmb::picocalc::keyboard {

constexpr int key_hotkey_screenshot = input_hotkeys::Screenshot;
constexpr int key_hotkey_sleep = input_hotkeys::Sleep;
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
bool last_key_repeat();
bool poll_pending();
bool navigation_key_held(int key);
bool set_lcd_backlight(unsigned char value);
bool get_lcd_backlight(unsigned char& value);
bool set_keyboard_backlight(unsigned char value);
bool get_keyboard_backlight(unsigned char& value);
bool read_battery(int& percent, bool& charging);
// Safe, non-FIFO version query. Zero means the MCU did not report a version.
// Caches the reply until init/recovery; a pending key read schedules a later
// bounded diagnostic phase instead of overwriting its response.
bool read_bios_version(unsigned char& value);
bool cached_bios_version_value(unsigned char& value);
bool caps_lock_enabled();
void set_caps_lock(bool enabled);
bool shift_held();
bool read_rtc(RtcDateTime& value, unsigned char address = 0x51);
bool write_rtc(const RtcDateTime& value, unsigned char address = 0x51);
RtcError last_rtc_error();
const Diagnostics& diagnostics();
const char* health_name(Health health);
const char* startup_phase_name(StartupPhase phase);
const char* error_name(I2cError error);

} // namespace rmb::picocalc::keyboard
