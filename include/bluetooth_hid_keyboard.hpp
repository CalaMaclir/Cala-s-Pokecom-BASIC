#pragma once

#include <cstddef>
#include <cstdint>

#include "bluetooth_hid_keyboard_core.hpp"

namespace rmb::bluetooth_hid {

inline constexpr std::size_t kMaxDiscoveredDevices = 64;

struct DiscoveredDevice {
    std::uint8_t address[6] = {};
    char name[40] = {};
    std::uint32_t class_of_device = 0;
    std::int8_t rssi = 0;
    bool rssi_available = false;
    bool keyboard_hint = false;
};

// The following lifecycle functions are called by the shared Classic
// Bluetooth owner while the CYW43/BTstack lock is already held.
void stack_init_locked();
void on_bluetooth_enabled_locked();
void on_bluetooth_disabled_locked();
void service_locked();
bool busy_locked();
bool scanning_locked();
bool connected_to_locked(const std::uint8_t address[6]);
bool disconnect_address_locked(const std::uint8_t address[6]);
bool handle_pin_code_request_locked(const std::uint8_t address[6]);
bool handle_user_confirmation_request_locked(
    const std::uint8_t address[6],
    std::uint32_t numeric_value
);

bool start_scan();
void cancel_scan();
bool scanning();
std::size_t discovered_devices(
    DiscoveredDevice* output,
    std::size_t capacity
);

bool connect_keyboard(const std::uint8_t address[6]);
bool connect_paired_keyboard(const std::uint8_t address[6]);
bool reconnect_paired_keyboard();
void cancel_connect();
bool connect_active();
bool pairing_active();
bool pairing_code_available();
std::uint32_t pairing_code();

bool connected();
bool connected_to(const std::uint8_t address[6]);
bool disconnect();
const char* connected_name();
const char* status();
const char* last_error();

void set_layout(KeyboardLayout layout);
KeyboardLayout layout();
int read_key();
bool caps_lock_enabled();
bool shift_held();
void set_caps_lock(bool enabled);
std::uint32_t key_overflow_count();

} // namespace rmb::bluetooth_hid

