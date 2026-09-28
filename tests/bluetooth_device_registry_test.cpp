#include "bluetooth_device_registry.hpp"

#include <cassert>
#include <cstring>

using rmb::bluetooth::DeviceRecord;
using rmb::bluetooth::DeviceRegistryCore;
using rmb::bluetooth::Profile;

int main() {
    DeviceRegistryCore registry;
    const std::uint8_t legacy[6] = {1, 2, 3, 4, 5, 6};
    const std::uint8_t keyboard[6] = {6, 5, 4, 3, 2, 1};

    assert(!registry.upsert(
        legacy, "legacy transport", static_cast<Profile>(1)));
    assert(registry.upsert(keyboard, "K380", Profile::HidKeyboard));
    assert(registry.count() == 1);

    DeviceRecord found;
    assert(!registry.find(legacy, found));

    assert(registry.upsert(keyboard, "", Profile::HidKeyboard));
    assert(registry.find(keyboard, found));
    assert(found.profile == Profile::HidKeyboard);
    assert(std::strcmp(found.name, "K380") == 0);

    // An identical numeric address on Classic, LE public and LE random is
    // three different identities. Deleting one must not delete the others.
    assert(registry.upsert(keyboard, "LE public", Profile::BleKeyboard, 0));
    assert(registry.upsert(keyboard, "LE random", Profile::BleKeyboard, 1));
    assert(registry.count() == 3);
    assert(registry.find(keyboard, found, 0) && found.address_type == 0);
    assert(std::strcmp(found.name, "LE public") == 0);
    assert(registry.erase(keyboard, 1));
    assert(!registry.find(keyboard, found, 1));
    assert(registry.find(keyboard, found));
    assert(std::strcmp(found.name, "K380") == 0);

    for (int i = 2; i < 16; ++i) {
        std::uint8_t address[6] = {42, 0, 0, 0, 0, static_cast<std::uint8_t>(i)};
        assert(registry.upsert(address, "", Profile::BleKeyboard, 0));
        assert(registry.find(address, found, 0));
        assert(found.name[0] == 0);
    }
    const std::uint8_t overflow[6] = {99, 1, 2, 3, 4, 5};
    assert(!registry.upsert(overflow, "new", Profile::BleKeyboard, 0));
    assert(registry.find(keyboard, found));
    assert(std::strcmp(found.name, "K380") == 0);

    registry.clear();
    assert(registry.count() == 0);
    return 0;
}
