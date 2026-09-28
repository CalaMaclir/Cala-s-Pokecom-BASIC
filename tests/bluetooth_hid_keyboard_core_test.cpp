#include "bluetooth_hid_keyboard_core.hpp"

#include <cassert>
#include <cstdint>

using rmb::bluetooth_hid::KeyboardCore;
using rmb::bluetooth_hid::KeyboardLayout;

int main() {
    KeyboardCore core;

    const std::uint8_t a[] = {0x04};
    core.handle_report(0, a, 1, 0);
    assert(core.read_key(0) == 'a');
    assert(core.read_key(399) == -1);
    assert(core.read_key(400) == 'a');
    assert(core.read_key(449) == -1);
    assert(core.read_key(450) == 'a');

    core.handle_report(0, nullptr, 0, 451);
    assert(core.read_key(1000) == -1);

    core.handle_report(0x02, a, 1, 1100);
    assert(core.read_key(1100) == 'A');
    core.handle_report(0, nullptr, 0, 1101);

    const std::uint8_t caps[] = {0x39};
    core.handle_report(0, caps, 1, 1200);
    assert(core.read_key(1200) == 0xc1);
    core.handle_report(0, nullptr, 0, 1201);
    core.handle_report(0, a, 1, 1202);
    assert(core.read_key(1202) == 'A');
    core.handle_report(0, nullptr, 0, 1203);
    core.handle_report(0x02, a, 1, 1204);
    assert(core.read_key(1204) == 'a');
    core.handle_report(0, nullptr, 0, 1205);

    core.set_caps_lock(false);
    core.handle_report(0x01, a, 1, 1300);
    assert(core.read_key(1300) == 1);
    core.handle_report(0, nullptr, 0, 1301);

    const std::uint8_t s[] = {0x16};
    core.handle_report(0x04, s, 1, 1400);
    assert(core.read_key(1400) == 0xe2);
    assert(core.read_key(2000) == -1);
    core.handle_report(0, nullptr, 0, 2001);

    const std::uint8_t f10[] = {0x43};
    core.handle_report(0, f10, 1, 2100);
    assert(core.read_key(2100) == 0x90);
    assert(core.read_key(3000) == -1);
    core.handle_report(0, nullptr, 0, 3001);

    const std::uint8_t left[] = {0x50};
    core.handle_report(0, left, 1, 3100);
    assert(core.read_key(3100) == 0xb4);
    assert(core.read_key(3500) == 0xb4);
    core.handle_report(0, nullptr, 0, 3501);

    core.set_layout(KeyboardLayout::Us);
    const std::uint8_t usage2[] = {0x1f};
    core.handle_report(0x02, usage2, 1, 3600);
    assert(core.read_key(3600) == '@');
    core.handle_report(0, nullptr, 0, 3601);

    core.set_layout(KeyboardLayout::Jis);
    core.handle_report(0x02, usage2, 1, 3700);
    assert(core.read_key(3700) == '"');
    core.handle_report(0, nullptr, 0, 3701);

    const std::uint8_t jis_at[] = {0x2f};
    core.handle_report(0, jis_at, 1, 3800);
    assert(core.read_key(3800) == '@');
    core.handle_report(0, nullptr, 0, 3801);

    const std::uint8_t jis_ro[] = {0x87};
    core.handle_report(0x02, jis_ro, 1, 3900);
    assert(core.read_key(3900) == '_');

    return 0;
}
