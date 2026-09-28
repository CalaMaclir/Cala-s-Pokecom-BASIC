#include "bluetooth_device_registry.hpp"

#include <cstring>

#include "btstack_tlv.h"

namespace rmb::bluetooth {

namespace {
constexpr std::uint32_t kRegistryTag =
    (static_cast<std::uint32_t>('C') << 24) |
    (static_cast<std::uint32_t>('P') << 16) |
    (static_cast<std::uint32_t>('D') << 8) |
    static_cast<std::uint32_t>('1');
constexpr std::uint32_t kRegistryMagic = 0x43504452u; // "CPDR"
constexpr std::uint8_t kRegistryVersion = 2;

struct LegacyRecord {
    std::uint8_t address[6];
    char name[40];
    Profile profile;
};
struct LegacyRegistry {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t reserved[3];
    LegacyRecord records[DeviceRegistryCore::kCapacity];
};

struct PersistedRegistry {
    std::uint32_t magic = kRegistryMagic;
    std::uint8_t version = kRegistryVersion;
    std::uint8_t reserved[3] = {};
    DeviceRecord records[DeviceRegistryCore::kCapacity] = {};
};

bool tlv_instance(const btstack_tlv_t*& impl, void*& context) {
    impl = nullptr;
    context = nullptr;
    btstack_tlv_get_instance(&impl, &context);
    return impl && context;
}

bool load_registry(DeviceRegistryCore& registry) {
    registry.clear();
    const btstack_tlv_t* impl = nullptr;
    void* context = nullptr;
    if (!tlv_instance(impl, context)) return false;

    PersistedRegistry stored;
    const int size = impl->get_tag(
        context,
        kRegistryTag,
        reinterpret_cast<std::uint8_t*>(&stored),
        sizeof(stored)
    );
    if (size == static_cast<int>(sizeof(LegacyRegistry)) &&
        stored.magic == kRegistryMagic && stored.version == 1) {
        LegacyRegistry legacy;
        std::memcpy(&legacy, &stored, sizeof(legacy));
        for (auto& record : legacy.records) {
            record.name[sizeof(record.name) - 1] = '\0';
            if (!is_keyboard_profile(record.profile)) continue;
            registry.upsert(record.address, record.name, record.profile);
        }
        return true;
    }
    if (size != static_cast<int>(sizeof(stored)) ||
        stored.magic != kRegistryMagic ||
        stored.version != kRegistryVersion) {
        return true;
    }

    for (auto& record : stored.records) {
        record.name[sizeof(record.name) - 1] = '\0';
        if (!is_keyboard_profile(record.profile)) continue;
        registry.upsert(record.address, record.name, record.profile, record.address_type);
    }
    return true;
}

bool save_registry(const DeviceRegistryCore& registry) {
    const btstack_tlv_t* impl = nullptr;
    void* context = nullptr;
    if (!tlv_instance(impl, context)) return false;

    PersistedRegistry stored;
    std::size_t output = 0;
    for (std::size_t i = 0;
         i < DeviceRegistryCore::kCapacity &&
         output < DeviceRegistryCore::kCapacity;
         ++i) {
        const DeviceRecord& record = registry.record(i);
        bool populated = false;
        for (std::uint8_t byte : record.address) {
            if (byte != 0) populated = true;
        }
        if (!populated) continue;
        stored.records[output++] = record;
    }

    return impl->store_tag(
        context,
        kRegistryTag,
        reinterpret_cast<const std::uint8_t*>(&stored),
        sizeof(stored)
    ) == 0;
}
} // namespace

bool remember_device(
    const std::uint8_t address[6],
    const char* name,
    Profile profile,
    std::uint8_t address_type
) {
    DeviceRegistryCore registry;
    if (!load_registry(registry)) return false;
    DeviceRecord old;
    if (registry.find(address, old, address_type) && old.profile == profile &&
        (!name || !*name || std::strncmp(old.name, name, sizeof(old.name)) == 0)) return true;
    if (!registry.upsert(address, name, profile, address_type)) return false;
    return save_registry(registry);
}

bool lookup_device(
    const std::uint8_t address[6],
    DeviceRecord& output,
    std::uint8_t address_type
) {
    DeviceRegistryCore registry;
    return load_registry(registry) && registry.find(address, output, address_type);
}

bool forget_device(const std::uint8_t address[6], std::uint8_t address_type) {
    DeviceRegistryCore registry;
    if (!load_registry(registry)) return false;
    registry.erase(address, address_type);
    return save_registry(registry);
}

bool clear_devices() {
    const btstack_tlv_t* impl = nullptr;
    void* context = nullptr;
    if (!tlv_instance(impl, context)) return false;
    impl->delete_tag(context, kRegistryTag);
    return true;
}

} // namespace rmb::bluetooth
