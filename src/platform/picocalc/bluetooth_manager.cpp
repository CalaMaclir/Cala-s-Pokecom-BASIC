#include "bluetooth_manager.hpp"

#include <cstdio>
#include <cstring>

#include "bluetooth_hid_ble_keyboard.hpp"
#include "bluetooth_hid_keyboard.hpp"
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "wireless.hpp"

namespace rmb::bluetooth_manager {
namespace {

constexpr char kDeviceName[] = "CPB-PicoCalc";
constexpr std::uint32_t kClassOfDevice = 0x001f00;

bool stack_initialized = false;
bool bluetooth_enabled = false;
char error_text[96] = "OFF";
char error_snapshot[96] = "OFF";
btstack_packet_callback_registration_t hci_events = {};

void set_error(const char* text) {
    std::snprintf(error_text, sizeof(error_text), "%s", text ? text : "ERROR");
}

void packet_handler(
    std::uint8_t packet_type,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
) {
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t address = {};
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (bluetooth_enabled &&
            btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            set_error("READY");
        }
        break;

    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, address);
        if (!bluetooth_hid::handle_pin_code_request_locked(address)) {
            gap_pin_code_negative(address);
            set_error("NON-KEYBOARD PAIRING BLOCKED");
        }
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, address);
        if (!bluetooth_hid::handle_user_confirmation_request_locked(
                address,
                hci_event_user_confirmation_request_get_numeric_value(
                    packet))) {
            gap_ssp_confirmation_negative(address);
            set_error("NON-KEYBOARD PAIRING BLOCKED");
        }
        break;

    default:
        break;
    }
}

void setup_stack_locked() {
    if (stack_initialized) return;
    l2cap_init();
    sdp_init();
    bluetooth_hid::stack_init_locked();
    bluetooth_hid_ble::stack_init_locked();
    hci_events.callback = packet_handler;
    hci_add_event_handler(&hci_events);
    gap_set_class_of_device(kClassOfDevice);
    gap_set_local_name(kDeviceName);
    gap_ssp_set_enable(1);
    gap_ssp_set_auto_accept(0);
    restore_idle_classic_security_locked();
    stack_initialized = true;
}

} // namespace

bool init() {
    if (!bluetooth_enabled) set_error("OFF");
    return true;
}

bool enable() {
    if (bluetooth_enabled) return true;
    if (!wireless::init()) {
        set_error(wireless::last_error());
        return false;
    }

    cyw43_arch_lwip_begin();
    setup_stack_locked();
    bluetooth_enabled = true;
    restore_idle_classic_security_locked();
    gap_connectable_control(1);
    gap_discoverable_control(0);
    bluetooth_hid::on_bluetooth_enabled_locked();
    bluetooth_hid_ble::on_bluetooth_enabled_locked();
    hci_power_control(HCI_POWER_ON);
    set_error("READY");
    cyw43_arch_lwip_end();
    return true;
}

void disable() {
    if (!bluetooth_enabled) return;
    if (!wireless::initialized()) {
        bluetooth_enabled = false;
        set_error("OFF");
        return;
    }

    cyw43_arch_lwip_begin();
    bluetooth_hid_ble::on_bluetooth_disabled_locked();
    bluetooth_hid::on_bluetooth_disabled_locked();
    gap_set_bondable_mode(0);
    gap_discoverable_control(0);
    gap_connectable_control(0);
    hci_power_control(HCI_POWER_OFF);
    bluetooth_enabled = false;
    set_error("OFF");
    cyw43_arch_lwip_end();
}

bool enabled() { return bluetooth_enabled; }

const char* status() { return bluetooth_enabled ? "ON" : "OFF"; }

const char* last_error() {
    if (!wireless::initialized()) return "OFF";
    cyw43_arch_lwip_begin();
    std::snprintf(error_snapshot, sizeof(error_snapshot), "%s", error_text);
    cyw43_arch_lwip_end();
    return error_snapshot;
}

void service() {
    if (!wireless::initialized() || !bluetooth_enabled) return;
    cyw43_arch_lwip_begin();
    bluetooth_hid::service_locked();
    bluetooth_hid_ble::service_locked();
    cyw43_arch_lwip_end();
}

bool keyboard_pairing_available_locked() { return bluetooth_enabled; }

bool keyboard_reconnect_available_locked() { return bluetooth_enabled; }

void restore_idle_classic_security_locked() {
    gap_set_bondable_mode(0);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
}

std::size_t paired_devices(PairedDeviceInfo* output, std::size_t capacity) {
    if (!output || capacity == 0 || !wireless::initialized() ||
        !bluetooth_enabled) {
        return 0;
    }

    cyw43_arch_lwip_begin();
    if (hci_get_state() != HCI_STATE_WORKING) {
        set_error("BLUETOOTH NOT READY");
        cyw43_arch_lwip_end();
        return 0;
    }

    std::size_t count = 0;
    btstack_link_key_iterator_t iterator = {};
    if (gap_link_key_iterator_init(&iterator)) {
        bd_addr_t address = {};
        link_key_t key = {};
        link_key_type_t type = static_cast<link_key_type_t>(0);
        while (count < capacity &&
               gap_link_key_iterator_get_next(&iterator, address, key, &type)) {
            bluetooth::DeviceRecord record = {};
            if (!bluetooth::lookup_device(address, record) ||
                record.profile != bluetooth::Profile::HidKeyboard) {
                continue;
            }
            auto& item = output[count++];
            item = PairedDeviceInfo{};
            std::memcpy(item.address, address, sizeof(item.address));
            std::snprintf(item.name, sizeof(item.name), "%s", record.name);
            item.link_key_type = static_cast<std::uint8_t>(type);
            item.profile = bluetooth::Profile::HidKeyboard;
            item.connected = bluetooth_hid::connected_to_locked(address);
        }
        gap_link_key_iterator_done(&iterator);
    }

    count += bluetooth_hid_ble::paired_devices_locked(
        output + count, capacity - count);
    cyw43_arch_lwip_end();
    return count;
}

bool forget_paired_device(const std::uint8_t address[6]) {
    if (!address || !wireless::initialized() || !bluetooth_enabled) return false;
    cyw43_arch_lwip_begin();
    bluetooth::DeviceRecord record = {};
    if (!bluetooth::lookup_device(address, record) ||
        record.profile != bluetooth::Profile::HidKeyboard) {
        set_error("NO PAIRED CLASSIC KEYBOARD");
        cyw43_arch_lwip_end();
        return false;
    }
    bluetooth_hid::disconnect_address_locked(address);
    bd_addr_t classic_address = {};
    std::memcpy(classic_address, address, sizeof(classic_address));
    gap_drop_link_key_for_bd_addr(classic_address);
    const bool result = bluetooth::forget_device(address);
    set_error(result ? "KEYBOARD FORGOTTEN" : "FORGET FAILED");
    cyw43_arch_lwip_end();
    return result;
}

bool forget_paired_devices() {
    if (!wireless::initialized() || !bluetooth_enabled) return false;
    cyw43_arch_lwip_begin();
    bluetooth_hid::disconnect_address_locked(nullptr);
    bluetooth_hid_ble::forget_all_bonds_locked();
    gap_delete_all_link_keys();
    const bool result = bluetooth::clear_devices();
    restore_idle_classic_security_locked();
    set_error(result ? "KEYBOARD PAIRINGS CLEARED" : "PAIRING CLEAR FAILED");
    cyw43_arch_lwip_end();
    return result;
}

} // namespace rmb::bluetooth_manager
