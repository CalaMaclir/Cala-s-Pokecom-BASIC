#include "unified_keyboard.hpp"

#include "bluetooth_hid_ble_keyboard.hpp"
#include "bluetooth_hid_keyboard.hpp"
#include "picocalc_keyboard.hpp"

namespace rmb::unified_keyboard {
namespace { bool returned_repeat = false; }
bool last_key_repeat() { return returned_repeat; }

void init() {
    picocalc::keyboard::init();
    bluetooth_hid::set_caps_lock(false);
    bluetooth_hid_ble::set_caps_lock(false);
}

int read_key() {
    returned_repeat = false;
    const int internal = picocalc::keyboard::read_key();
    if (internal == 0xc1) {
        const bool enabled = picocalc::keyboard::caps_lock_enabled();
        bluetooth_hid::set_caps_lock(enabled);
        bluetooth_hid_ble::set_caps_lock(enabled);
    }
    if (internal >= 0) { returned_repeat = picocalc::keyboard::last_key_repeat(); return internal; }

    const int classic = bluetooth_hid::read_key();
    if (classic == 0xc1) {
        const bool enabled = bluetooth_hid::caps_lock_enabled();
        picocalc::keyboard::set_caps_lock(enabled);
        bluetooth_hid_ble::set_caps_lock(enabled);
    }
    if (classic >= 0) { returned_repeat = bluetooth_hid::last_key_repeat(); return classic; }

    const int ble = bluetooth_hid_ble::read_key();
    if (ble == 0xc1) {
        const bool enabled = bluetooth_hid_ble::caps_lock_enabled();
        picocalc::keyboard::set_caps_lock(enabled);
        bluetooth_hid::set_caps_lock(enabled);
    }
    if (ble >= 0) returned_repeat = bluetooth_hid_ble::last_key_repeat();
    return ble;
}

bool caps_lock_enabled() {
    return picocalc::keyboard::caps_lock_enabled();
}

bool shift_held() {
    return picocalc::keyboard::shift_held() ||
           bluetooth_hid::shift_held() ||
           bluetooth_hid_ble::shift_held();
}

} // namespace rmb::unified_keyboard
