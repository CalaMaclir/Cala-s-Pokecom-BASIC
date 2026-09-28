#include "bluetooth_device_registry.hpp"
#include "btstack_tlv.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <algorithm>
using namespace rmb::bluetooth;
static std::vector<std::uint8_t> bytes;
static int writes = 0;
static int get(void*, std::uint32_t, std::uint8_t* out, std::uint32_t size) {
    std::memcpy(out, bytes.data(), std::min<std::size_t>(size, bytes.size()));
    return static_cast<int>(bytes.size());
}
static int put(void*, std::uint32_t, const std::uint8_t* in, std::uint32_t size) {
    bytes.assign(in, in + size); ++writes; return 0;
}
static void remove_tag(void*, std::uint32_t) { bytes.clear(); }
static const btstack_tlv_t tlv{get, put, remove_tag};
void btstack_tlv_get_instance(const btstack_tlv_t** out, void** context) {
    *out = &tlv; *context = &bytes;
}
int main() {
    struct OldRecord { std::uint8_t address[6]; char name[40]; Profile profile; };
    struct OldStore { std::uint32_t magic; std::uint8_t version, reserved[3]; OldRecord records[16]; };
    OldStore old{}; old.magic = 0x43504452; old.version = 1;
    const std::uint8_t address[6] = {1,2,3,4,5,6};
    std::memcpy(old.records[0].address, address, 6);
    std::strcpy(old.records[0].name, "Legacy transport");
    old.records[0].profile = static_cast<Profile>(1);
    bytes.assign(reinterpret_cast<std::uint8_t*>(&old), reinterpret_cast<std::uint8_t*>(&old) + sizeof(old));
    DeviceRecord result;
    assert(!lookup_device(address, result));
    assert(writes == 0);
    assert(remember_device(address, "Keyboard", Profile::BleKeyboard, 1));
    assert(bytes[4] == 2 && writes == 1);
    assert(!lookup_device(address, result));
    assert(lookup_device(address, result, 1));
    assert(std::strcmp(result.name, "Keyboard") == 0);
    assert(remember_device(address, "Keyboard", Profile::BleKeyboard, 1));
    assert(writes == 1); // no flash wear on unchanged reconnect
    assert(forget_device(address, 1));
    assert(!lookup_device(address, result, 1));
    assert(!lookup_device(address, result));
    assert(clear_devices());
    assert(!lookup_device(address, result));
}
