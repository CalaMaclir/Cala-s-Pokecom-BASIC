#pragma once

#include <cstddef>
#include <cstdint>

#include "bluetooth_hid_keyboard_core.hpp"
#include "bluetooth_manager.hpp"

namespace rmb::bluetooth_hid_ble {

inline constexpr std::size_t kMaxDiscoveredDevices = 64;

// Active-scan result for advertisements that identify as a keyboard through
// HID Service UUID 0x1812 or the Bluetooth Keyboard Appearance value.
struct DiscoveredDevice {
    std::uint8_t address[6] = {};
    std::uint8_t address_type = 0;
    char name[40] = {};
    std::int8_t rssi = 0;
    bool hid_hint = false;
};

// Called with the CYW43/lwIP lock already held by bluetooth_manager.
void stack_init_locked();
void on_bluetooth_enabled_locked();
void on_bluetooth_disabled_locked();
void service_locked();
bool busy_locked();
bool scanning_locked();
void disconnect_locked();
void forget_all_bonds_locked();
std::size_t paired_devices_locked(bluetooth_manager::PairedDeviceInfo* output, std::size_t capacity);
bool connected_to_locked(const std::uint8_t address[6], std::uint8_t address_type);
bool forget_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type);
bool connect_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type, const char* name);

// Stage 1: active BLE diagnostic scan.
bool start_scan();
void cancel_scan();
bool scanning();
std::size_t discovered_devices(
    DiscoveredDevice* output,
    std::size_t capacity
);
bool discovery_capacity_reached();

// Fresh pairing; reconnect uses the separate API above to preserve bond keys.
bool connect_keyboard(
    const std::uint8_t address[6],
    std::uint8_t address_type,
    const char* name
);
void cancel_connect();
bool connect_active();
bool pairing_active();
bool pairing_code_available();
std::uint32_t pairing_code();

bool connected();
bool disconnect();
const char* connected_name();
const char* status();
const char* last_error();

void set_layout(bluetooth_hid::KeyboardLayout layout);
bluetooth_hid::KeyboardLayout layout();
int read_key();
bool caps_lock_enabled();
bool shift_held();
void set_caps_lock(bool enabled);
std::uint32_t key_overflow_count();

} // namespace rmb::bluetooth_hid_ble
