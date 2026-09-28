#include "bluetooth_hid_keyboard_core.hpp"

namespace rmb::bluetooth_hid {

void set_layout(KeyboardLayout) {
    // Host-side REPL tests do not initialize the Pico W Bluetooth stack.
}

} // namespace rmb::bluetooth_hid
