#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::bluetooth {

enum class Profile : std::uint8_t {
    Unknown = 0,
    HidKeyboard = 2,
    BleKeyboard = 3
};

inline bool is_keyboard_profile(Profile profile) {
    return profile == Profile::HidKeyboard ||
           profile == Profile::BleKeyboard;
}

inline const char* profile_name(Profile profile) {
    switch (profile) {
    case Profile::HidKeyboard: return "KEYBD C";
    case Profile::BleKeyboard: return "KEYBD LE";
    default: return "UNKNOWN";
    }
}

struct DeviceRecord {
    std::uint8_t address[6] = {};
    char name[40] = {};
    Profile profile = Profile::Unknown;
    // 0/1: LE public/random identity; 0xff: Bluetooth Classic.
    std::uint8_t address_type = 0xff;
};

class DeviceRegistryCore {
public:
    static constexpr std::size_t kCapacity = 16;

    bool upsert(
        const std::uint8_t address[6],
        const char* name,
        Profile profile,
        std::uint8_t address_type = 0xff
    );
    bool erase(const std::uint8_t address[6], std::uint8_t address_type = 0xff);
    void clear();
    bool find(
        const std::uint8_t address[6],
        DeviceRecord& output,
        std::uint8_t address_type = 0xff
    ) const;
    std::size_t count() const;
    const DeviceRecord& record(std::size_t index) const {
        return records_[index];
    }

private:
    DeviceRecord records_[kCapacity] = {};
};

bool remember_device(
    const std::uint8_t address[6],
    const char* name,
    Profile profile,
    std::uint8_t address_type = 0xff
);
bool lookup_device(
    const std::uint8_t address[6],
    DeviceRecord& output,
    std::uint8_t address_type = 0xff
);
bool forget_device(const std::uint8_t address[6], std::uint8_t address_type = 0xff);
bool clear_devices();

} // namespace rmb::bluetooth
