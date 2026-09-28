#pragma once

#include <cstddef>
#include <cstdint>

#include "bluetooth_device_registry.hpp"

namespace rmb::bluetooth_manager {

struct PairedDeviceInfo {
    std::uint8_t address[6] = {};
    char name[40] = {};
    std::uint8_t link_key_type = 0;
    std::uint8_t address_type = 0xff;
    bluetooth::Profile profile = bluetooth::Profile::Unknown;
    bool connected = false;
};

bool init();
bool enable();
void disable();
bool enabled();
const char* status();
const char* last_error();
void service();

// These functions are called only while the shared CYW43/BTstack lock is held.
bool keyboard_pairing_available_locked();
bool keyboard_reconnect_available_locked();
void restore_idle_classic_security_locked();

std::size_t paired_devices(PairedDeviceInfo* output, std::size_t capacity);
bool forget_paired_device(const std::uint8_t address[6]);
bool forget_paired_devices();

} // namespace rmb::bluetooth_manager
