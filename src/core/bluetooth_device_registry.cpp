#include "bluetooth_device_registry.hpp"

#include <cstdio>
#include <cstring>

namespace rmb::bluetooth {

namespace {
bool same_address(
    const std::uint8_t lhs[6],
    const std::uint8_t rhs[6]
) {
    return lhs && rhs && std::memcmp(lhs, rhs, 6) == 0;
}

bool empty_address(const std::uint8_t address[6]) {
    if (!address) return true;
    for (int i = 0; i < 6; ++i) {
        if (address[i] != 0) return false;
    }
    return true;
}
} // namespace

bool DeviceRegistryCore::upsert(
    const std::uint8_t address[6],
    const char* name,
    Profile profile,
    std::uint8_t address_type
) {
    if (!address || empty_address(address) || !is_keyboard_profile(profile)) {
        return false;
    }

    std::size_t index = kCapacity;
    std::size_t empty = kCapacity;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        if (records_[i].address_type == address_type &&
            same_address(records_[i].address, address)) {
            index = i;
            break;
        }
        if (empty == kCapacity && empty_address(records_[i].address)) {
            empty = i;
        }
    }
    if (index == kCapacity) index = empty;
    if (index == kCapacity) return false;

    DeviceRecord& record = records_[index];
    std::memcpy(record.address, address, sizeof(record.address));
    record.address_type = address_type;
    if (name && *name) {
        std::snprintf(record.name, sizeof(record.name), "%s", name);
    }
    if (profile != Profile::Unknown || record.profile == Profile::Unknown) {
        record.profile = profile;
    }
    return true;
}

bool DeviceRegistryCore::erase(const std::uint8_t address[6], std::uint8_t address_type) {
    if (!address) return false;
    for (auto& record : records_) {
        if (record.address_type != address_type ||
            !same_address(record.address, address)) continue;
        record = DeviceRecord{};
        return true;
    }
    return false;
}

void DeviceRegistryCore::clear() {
    for (auto& record : records_) record = DeviceRecord{};
}

bool DeviceRegistryCore::find(
    const std::uint8_t address[6],
    DeviceRecord& output,
    std::uint8_t address_type
) const {
    if (!address || empty_address(address)) return false;
    for (const auto& record : records_) {
        if (record.address_type != address_type ||
            !same_address(record.address, address)) continue;
        output = record;
        return true;
    }
    return false;
}

std::size_t DeviceRegistryCore::count() const {
    std::size_t result = 0;
    for (const auto& record : records_) {
        if (!empty_address(record.address)) ++result;
    }
    return result;
}

} // namespace rmb::bluetooth
