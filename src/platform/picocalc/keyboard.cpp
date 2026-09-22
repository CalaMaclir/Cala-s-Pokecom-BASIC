#include "picocalc_keyboard.hpp"

#include <cstdint>
#include "hardware/i2c.h"
#include "pico/stdlib.h"

namespace rmb::picocalc::keyboard {

namespace {
i2c_inst_t* const bus = i2c1;
constexpr uint sda_pin = 6;
constexpr uint scl_pin = 7;
constexpr uint32_t bus_hz = 10000;
constexpr uint8_t address = 0x1f;
constexpr uint8_t key_register = 0x09;
constexpr uint8_t lcd_backlight_register = 0x05;
constexpr uint8_t keyboard_backlight_register = 0x0a;
constexpr uint8_t battery_register = 0x0b;
constexpr uint8_t rtc_address = 0x51;

constexpr uint8_t key_mod_alt = 0xa1;
constexpr uint8_t key_mod_shl = 0xa2;
constexpr uint8_t key_mod_shr = 0xa3;
constexpr uint8_t key_mod_ctrl = 0xa5;
constexpr uint8_t key_caps_lock = 0xc1;

bool initialized = false;
bool ctrl_held = false;
bool shift_left_held = false;
bool shift_right_held = false;
bool alt_held = false;
bool caps_lock = false;
RtcError rtc_error = RtcError::None;

uint8_t bcd_to_bin(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) * 10u) + (value & 0x0fu));
}

uint8_t bin_to_bcd(int value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

void configure_i2c_bus() {
    i2c_init(bus, bus_hz);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
}

bool read_u8_register(uint8_t reg_id, uint8_t& value) {
    if (!initialized) return false;

    uint8_t reg = reg_id;
    if (i2c_write_timeout_us(
            bus, address, &reg, 1, false, 5000) < 0) {
        return false;
    }

    sleep_ms(2);

    uint8_t data[2] = {0, 0};
    if (i2c_read_timeout_us(
            bus, address, data, 2, false, 5000) < 0) {
        return false;
    }

    value = data[1];
    return true;
}

bool write_u8_register(uint8_t reg_id, uint8_t value) {
    if (!initialized) return false;

    uint8_t msg[2] = {
        static_cast<uint8_t>(reg_id | 0x80u),
        value
    };

    if (i2c_write_timeout_us(
            bus, address, msg, 2, false, 5000) < 0) {
        return false;
    }

    sleep_ms(2);
    return true;
}

void flush_key_fifo() {
    // The keyboard MCU survives an RP2350 reset and its event FIFO can contain
    // stale modifier/Caps events from before the firmware restart. Drain only
    // the keyboard FIFO; do not reset or reconfigure the MCU itself.
    for (int i = 0; i < 32; ++i) {
        uint8_t reg = key_register;
        if (i2c_write_timeout_us(
                bus, address, &reg, 1, false, 5000) < 0) {
            return;
        }
        sleep_ms(2);

        uint8_t data[2] = {0, 0};
        if (i2c_read_timeout_us(
                bus, address, data, 2, false, 5000) < 0) {
            return;
        }

        if (data[0] == 0) {
            return;
        }
    }
}

}

void init() {
    configure_i2c_bus();
    initialized = true;

    // Host-side Caps Lock deliberately starts OFF. The keyboard MCU has its
    // own Caps state and may survive an RP2350 reset, so mirroring that state
    // can leave RetroMiniBASIC permanently uppercase. We normalize case here.
    caps_lock = false;
    shift_left_held = false;
    shift_right_held = false;
    alt_held = false;
    ctrl_held = false;
    flush_key_fifo();
}

void reconfigure_bus_clock() {
    if (!initialized) return;

    // RP2350 I2C timing is derived from clk_sys. CPU speed changes therefore
    // require the divider to be recomputed even though the requested bus rate
    // remains unchanged.
    i2c_set_baudrate(bus, bus_hz);
}

bool set_lcd_backlight(unsigned char value) {
    return write_u8_register(
        lcd_backlight_register,
        static_cast<uint8_t>(value)
    );
}

bool get_lcd_backlight(unsigned char& value) {
    uint8_t current = 0;
    if (!read_u8_register(lcd_backlight_register, current)) {
        return false;
    }
    value = current;
    return true;
}

bool set_keyboard_backlight(unsigned char value) {
    return write_u8_register(
        keyboard_backlight_register,
        static_cast<uint8_t>(value)
    );
}

bool get_keyboard_backlight(unsigned char& value) {
    uint8_t current = 0;
    if (!read_u8_register(keyboard_backlight_register, current)) {
        return false;
    }
    value = current;
    return true;
}

bool read_battery(int& percent, bool& charging) {
    percent = -1;
    charging = false;
    if (!initialized) return false;

    uint8_t reg = battery_register;
    if (i2c_write_timeout_us(bus, address, &reg, 1, false, 5000) < 0) {
        return false;
    }

    // Match ClockworkPi's reference battery-read settling time.
    sleep_ms(16);

    uint8_t data[2] = {0, 0};
    if (i2c_read_timeout_us(bus, address, data, 2, false, 5000) < 0) {
        return false;
    }

    // Controller returns register id in byte 0 and battery/charging in byte 1.
    const uint8_t raw = data[1];
    charging = (raw & 0x80u) != 0;
    percent = static_cast<int>(raw & 0x7fu);
    if (percent > 100) percent = 100;
    return true;
}

bool caps_lock_enabled() {
    return caps_lock;
}

bool shift_held() {
    return shift_left_held || shift_right_held;
}


bool read_rtc(RtcDateTime& value) {
    if (!initialized) {
        rtc_error = RtcError::NotInitialized;
        return false;
    }

    // The keyboard controller and battery gauge share this I2C bus. Never
    // deinit/reclock the bus just because the optional PCF8563 does not ACK.
    // A failed RTC probe must leave keyboard input completely untouched.
    constexpr uint32_t rtc_timeout_us = 20000;

    uint8_t reg = 0x02;
    const int wr = i2c_write_timeout_us(
        bus,
        rtc_address,
        &reg,
        1,
        false,
        rtc_timeout_us
    );

    if (wr != 1) {
        rtc_error = RtcError::PointerWriteFailed;
        return false;
    }

    sleep_ms(2);

    uint8_t data[7] = {};
    const int rd = i2c_read_timeout_us(
        bus,
        rtc_address,
        data,
        sizeof(data),
        false,
        rtc_timeout_us
    );

    if (rd != static_cast<int>(sizeof(data))) {
        rtc_error = RtcError::ReadFailed;
        return false;
    }

    RtcDateTime candidate;
    candidate.second = bcd_to_bin(data[0] & 0x7fu);
    candidate.minute = bcd_to_bin(data[1] & 0x7fu);
    candidate.hour = bcd_to_bin(data[2] & 0x3fu);
    candidate.day = bcd_to_bin(data[3] & 0x3fu);
    candidate.month = bcd_to_bin(data[5] & 0x1fu);
    candidate.year = 2000 + bcd_to_bin(data[6]);

    if (candidate.month < 1 || candidate.month > 12 ||
        candidate.day < 1 || candidate.day > 31 ||
        candidate.hour > 23 ||
        candidate.minute > 59 ||
        candidate.second > 59) {
        rtc_error = RtcError::ReadFailed;
        return false;
    }

    value = candidate;
    rtc_error = RtcError::None;
    return true;
}

bool write_rtc(const RtcDateTime& value) {
    if (!initialized) {
        rtc_error = RtcError::NotInitialized;
        return false;
    }
    if (value.year < 2000 || value.year > 2099 ||
        value.month < 1 || value.month > 12 ||
        value.day < 1 || value.day > 31 ||
        value.hour < 0 || value.hour > 23 ||
        value.minute < 0 || value.minute > 59 ||
        value.second < 0 || value.second > 59) {
        rtc_error = RtcError::InvalidValue;
        return false;
    }

    uint8_t data[10] = {
        0x00,
        0x00,
        0x00,
        bin_to_bcd(value.second),
        bin_to_bcd(value.minute),
        bin_to_bcd(value.hour),
        bin_to_bcd(value.day),
        0x01,
        bin_to_bcd(value.month),
        bin_to_bcd(value.year - 2000)
    };

    constexpr uint32_t rtc_timeout_us = 20000;
    const int written = i2c_write_timeout_us(
        bus,
        rtc_address,
        data,
        sizeof(data),
        false,
        rtc_timeout_us
    );

    if (written != static_cast<int>(sizeof(data))) {
        rtc_error = RtcError::WriteFailed;
        return false;
    }

    sleep_ms(20);

    RtcDateTime verify;
    if (!read_rtc(verify)) {
        rtc_error = RtcError::ReadbackFailed;
        return false;
    }

    if (verify.year != value.year ||
        verify.month != value.month ||
        verify.day != value.day ||
        verify.hour != value.hour ||
        verify.minute != value.minute) {
        rtc_error = RtcError::ReadbackMismatch;
        return false;
    }

    rtc_error = RtcError::None;
    return true;
}

RtcError last_rtc_error() {
    return rtc_error;
}

int read_key() {
    if (!initialized) {
        return -1;
    }

    uint8_t reg = key_register;
    const int wr = i2c_write_timeout_us(bus, address, &reg, 1, false, 5000);
    if (wr < 0) {
        return -1;
    }

    sleep_ms(2);

    uint8_t data[2] = {0, 0};
    const int rd = i2c_read_timeout_us(bus, address, data, 2, false, 5000);
    if (rd < 0) {
        return -1;
    }

    const uint8_t state = data[0];
    int c = data[1];

    if (state == 0 || c == 0) {
        return -1;
    }


    // Modifier events are independent events in the official PicoCalc
    // keyboard firmware. Track them host-side so character case is reliable
    // even if the MCU's Caps/Shift state has become confusing.
    if (c == key_mod_ctrl || c == 0x7e) {
        if (state == 1) ctrl_held = true;
        else if (state == 3) ctrl_held = false;
        return -1;
    }
    if (c == key_mod_shl) {
        if (state == 1) shift_left_held = true;
        else if (state == 3) shift_left_held = false;
        return -1;
    }
    if (c == key_mod_shr) {
        if (state == 1) shift_right_held = true;
        else if (state == 3) shift_right_held = false;
        return -1;
    }
    if (c == key_mod_alt) {
        if (state == 1) alt_held = true;
        else if (state == 3) alt_held = false;
        return -1;
    }
    if (c == key_caps_lock) {
        if (state == 1) {
            caps_lock = !caps_lock;
            return key_caps_lock;
        }
        return -1;
    }

    if (state != 1) {
        return -1;
    }

    // The current PicoCalc keyboard firmware emits KEY_POWER on a short
    // press. Older firmware does not, so Alt+P is kept as a host-side
    // fallback. Alt+Space is deliberately not used here: the keyboard MCU
    // reserves it for cycling the keyboard backlight.
    if (c == key_power ||
        (alt_held && (c == 'P' || c == 'p'))) {
        return key_hotkey_sleep;
    }

    // Alt+S is a global screenshot hotkey. Consume it before normal
    // Caps/Shift case normalization so the shortcut is layout-independent.
    // Alt+B is reserved by the PicoCalc keyboard MCU for battery display.
    if (alt_held && (c == 'S' || c == 's')) {
        return key_hotkey_screenshot;
    }

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        const bool shift_held = shift_left_held || shift_right_held;
        const bool uppercase = caps_lock != shift_held; // Caps XOR Shift

        if (uppercase && c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        } else if (!uppercase && c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }

    if (ctrl_held && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;
    } else if (ctrl_held && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 1;
    }

    return c;
}

} // namespace rmb::picocalc::keyboard
