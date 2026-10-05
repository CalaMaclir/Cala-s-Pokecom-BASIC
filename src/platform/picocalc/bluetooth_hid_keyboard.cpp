#include "bluetooth_hid_keyboard.hpp"
#include "bluetooth_hid_ble_keyboard.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bluetooth_device_registry.hpp"
#include "bluetooth_manager.hpp"
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "wireless.hpp"

namespace rmb::bluetooth_hid {

namespace {
constexpr std::size_t kDescriptorCapacity = 512;
constexpr std::uint32_t kConnectTimeoutMs = 30000;
constexpr std::uint32_t kInquiryLength = 5;

KeyboardCore keyboard;
std::uint8_t descriptor_storage[kDescriptorCapacity] = {};
DiscoveredDevice discovered[kMaxDiscoveredDevices] = {};
bool name_attempted[kMaxDiscoveredDevices] = {};
std::uint8_t discovered_page_scan[kMaxDiscoveredDevices] = {};
std::uint16_t discovered_clock_offset[kMaxDiscoveredDevices] = {};
std::size_t discovered_count = 0;
int pending_name_index = -1;

btstack_packet_callback_registration_t hci_events = {};
bool protocol_initialized = false;
bool bluetooth_enabled = false;
bool inquiry_active = false;
bool scan_requested = false;
bool connecting = false;
bool pairing = false;
std::uint32_t connect_deadline_ms = 0;
std::uint16_t hid_cid = 0;
bool descriptor_available = false;
std::uint8_t target_address[6] = {};
bool target_valid = false;
std::uint8_t connected_address[6] = {};
bool connected_address_valid = false;
char target_name[40] = {};
char connected_device_name[40] = {};
char status_text[64] = "DISCONNECTED";
char error_text[96] = "OFF";
char status_snapshot[64] = "DISCONNECTED";
char error_snapshot[96] = "OFF";
char name_snapshot[40] = {};
std::uint32_t displayed_code = 0;
bool displayed_code_valid = false;

bool same_address(
    const std::uint8_t lhs[6],
    const std::uint8_t rhs[6]
) {
    return lhs && rhs && std::memcmp(lhs, rhs, 6) == 0;
}

bool address_is_zero(const std::uint8_t address[6]) {
    if (!address) return true;
    for (int i = 0; i < 6; ++i) {
        if (address[i] != 0) return false;
    }
    return true;
}

void clean_name(char* output, std::size_t capacity, const char* input) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!input) return;
    std::size_t out = 0;
    for (std::size_t i = 0; input[i] && out + 1 < capacity; ++i) {
        const unsigned char value = static_cast<unsigned char>(input[i]);
        output[out++] = value >= 0x20 && value <= 0x7e
            ? static_cast<char>(value) : '?';
    }
    output[out] = '\0';
}

void set_status(const char* value) {
    std::snprintf(status_text, sizeof(status_text), "%s", value ? value : "");
}

void set_error(const char* value) {
    std::snprintf(error_text, sizeof(error_text), "%s", value ? value : "");
}

bool keyboard_class(std::uint32_t class_of_device) {
    const bool peripheral =
        (class_of_device & 0x001f00u) == 0x000500u;
    const bool keyboard_bit = (class_of_device & 0x000040u) != 0;
    return peripheral && keyboard_bit;
}

int discovered_index(const std::uint8_t address[6]) {
    for (std::size_t i = 0; i < discovered_count; ++i) {
        if (same_address(discovered[i].address, address)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void clear_target_locked() {
    std::memset(target_address, 0, sizeof(target_address));
    target_valid = false;
    target_name[0] = '\0';
    displayed_code = 0;
    displayed_code_valid = false;
    connect_deadline_ms = 0;
}

void finish_connect_failure_locked(const char* message) {
    if (hid_cid) {
        const std::uint16_t closing_cid = hid_cid;
        hid_cid = 0;
        hid_host_disconnect(closing_cid);
    }
    connecting = false;
    pairing = false;
    descriptor_available = false;
    clear_target_locked();
    bluetooth_manager::restore_idle_classic_security_locked();
    set_status("DISCONNECTED");
    set_error(message);
}

bool begin_connect_locked(
    const std::uint8_t address[6],
    bool pairing_request,
    const char* name
) {
    if (!bluetooth_enabled || !address || address_is_zero(address) ||
        hid_cid || connecting) {
        set_error("HID CONNECT NOT AVAILABLE");
        return false;
    }
    const bool manager_available = pairing_request
        ? bluetooth_manager::keyboard_pairing_available_locked()
        : bluetooth_manager::keyboard_reconnect_available_locked();
    if (!manager_available) {
        set_error("BLUETOOTH KEYBOARD NOT AVAILABLE");
        return false;
    }
    if (bluetooth_hid_ble::busy_locked()) {
        set_error("DISCONNECT BLE KEYBOARD FIRST");
        return false;
    }

    std::memcpy(target_address, address, sizeof(target_address));
    target_valid = true;
    clean_name(target_name, sizeof(target_name), name);
    connecting = true;
    pairing = pairing_request;
    descriptor_available = false;
    displayed_code = 0;
    displayed_code_valid = false;
    connect_deadline_ms =
        to_ms_since_boot(get_absolute_time()) + kConnectTimeoutMs;

    if (pairing_request) {
        gap_set_bondable_mode(1);
        gap_ssp_set_enable(1);
        gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
        gap_ssp_set_authentication_requirement(
            SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
        set_status("PAIRING");
        set_error("PAIRING - CONNECTING");
    } else {
        bluetooth_manager::restore_idle_classic_security_locked();
        set_status("CONNECTING");
        set_error("CONNECTING PAIRED KEYBOARD");
    }

    const std::uint8_t result = hid_host_connect(
        target_address,
        HID_PROTOCOL_MODE_REPORT,
        &hid_cid
    );
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message, sizeof(message),
            "HID CONNECT FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        finish_connect_failure_locked(message);
        return false;
    }
    return true;
}

void request_next_name_locked() {
    pending_name_index = -1;
    for (std::size_t i = 0; i < discovered_count; ++i) {
        if (discovered[i].name[0] || name_attempted[i]) continue;
        name_attempted[i] = true;
        const std::uint8_t result = gap_remote_name_request(
            discovered[i].address,
            discovered_page_scan[i],
            static_cast<std::uint16_t>(
                discovered_clock_offset[i] | 0x8000u)
        );
        if (result == ERROR_CODE_SUCCESS) {
            pending_name_index = static_cast<int>(i);
            set_status("READING DEVICE NAMES");
            return;
        }
    }
    if (scan_requested) {
        inquiry_active = true;
        set_status("SCANNING CLASSIC DEVICES");
        set_error("SEARCHING CLASSIC DEVICES");
        gap_inquiry_start(kInquiryLength);
        return;
    }
    set_status("SCAN COMPLETE");
    set_error(discovered_count ? "SELECT A KEYBOARD" : "NO DEVICES FOUND");
}

void try_auto_reconnect_locked() {
    if (!bluetooth_enabled || hci_get_state() != HCI_STATE_WORKING ||
        hid_cid || connecting) {
        return;
    }

    btstack_link_key_iterator_t iterator = {};
    if (!gap_link_key_iterator_init(&iterator)) return;

    bd_addr_t address = {};
    link_key_t key = {};
    link_key_type_t key_type = static_cast<link_key_type_t>(0);
    while (gap_link_key_iterator_get_next(
            &iterator, address, key, &key_type)) {
        bluetooth::DeviceRecord record;
        if (!bluetooth::lookup_device(address, record) ||
            record.profile != bluetooth::Profile::HidKeyboard) {
            continue;
        }
        gap_link_key_iterator_done(&iterator);
        begin_connect_locked(address, false, record.name);
        return;
    }
    gap_link_key_iterator_done(&iterator);
}

bool authentication_target_locked(const std::uint8_t address[6]) {
    return target_valid && (pairing || connecting) &&
           same_address(target_address, address);
}

void handle_hid_report_locked(
    const std::uint8_t* report,
    std::uint16_t report_length
) {
    if (!descriptor_available || !report || report_length < 1 ||
        report[0] != 0xa1) {
        return;
    }
    ++report;
    --report_length;

    btstack_hid_parser_t parser;
    btstack_hid_parser_init(
        &parser,
        hid_descriptor_storage_get_descriptor_data(hid_cid),
        hid_descriptor_storage_get_descriptor_len(hid_cid),
        HID_REPORT_TYPE_INPUT,
        report,
        report_length
    );

    std::uint8_t modifiers = 0;
    std::uint8_t usages[KeyboardCore::kMaxKeys] = {};
    std::size_t usage_count = 0;
    while (btstack_hid_parser_has_more(&parser)) {
        std::uint16_t usage_page = 0;
        std::uint16_t usage = 0;
        std::int32_t value = 0;
        btstack_hid_parser_get_field(
            &parser, &usage_page, &usage, &value);
        if (usage_page != HID_USAGE_PAGE_KEYBOARD) continue;
        if (usage >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
            usage <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
            if (value) {
                modifiers |= static_cast<std::uint8_t>(
                    1u << (usage - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL));
            }
            continue;
        }
        if (usage_count < KeyboardCore::kMaxKeys && usage <= 0xffu) {
            usages[usage_count++] = static_cast<std::uint8_t>(usage);
        }
    }

    keyboard.handle_report(
        modifiers,
        usages,
        usage_count,
        to_ms_since_boot(get_absolute_time())
    );
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
            try_auto_reconnect_locked();
        }
        break;

    case GAP_EVENT_INQUIRY_RESULT: {
        if (!inquiry_active) break;
        gap_event_inquiry_result_get_bd_addr(packet, address);
        int index = discovered_index(address);
        if (index < 0) {
            if (discovered_count >= kMaxDiscoveredDevices) break;
            index = static_cast<int>(discovered_count++);
            std::memcpy(
                discovered[index].address,
                address,
                sizeof(discovered[index].address)
            );
        }
        discovered_page_scan[index] =
            gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
        discovered_clock_offset[index] =
            gap_event_inquiry_result_get_clock_offset(packet);
        auto& item = discovered[index];
        item.class_of_device =
            gap_event_inquiry_result_get_class_of_device(packet);
        item.keyboard_hint = keyboard_class(item.class_of_device);
        item.rssi_available =
            gap_event_inquiry_result_get_rssi_available(packet) != 0;
        if (item.rssi_available) {
            item.rssi = gap_event_inquiry_result_get_rssi(packet);
        }
        if (gap_event_inquiry_result_get_name_available(packet)) {
            const std::uint8_t length =
                gap_event_inquiry_result_get_name_len(packet);
            char name[241] = {};
            const std::size_t copy =
                std::min<std::size_t>(length, sizeof(name) - 1);
            std::memcpy(
                name,
                gap_event_inquiry_result_get_name(packet),
                copy
            );
            clean_name(item.name, sizeof(item.name), name);
        }
        break;
    }

    case GAP_EVENT_INQUIRY_COMPLETE:
        if (inquiry_active) {
            inquiry_active = false;
            request_next_name_locked();
        }
        break;

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (pending_name_index >= 0) {
            hci_event_remote_name_request_complete_get_bd_addr(
                packet, address);
            auto& item = discovered[pending_name_index];
            if (same_address(item.address, address) &&
                hci_event_remote_name_request_complete_get_status(packet) ==
                    ERROR_CODE_SUCCESS) {
                clean_name(
                    item.name,
                    sizeof(item.name),
                    hci_event_remote_name_request_complete_get_remote_name(
                        packet)
                );
            }
            pending_name_index = -1;
            request_next_name_locked();
        } else if (connected_address_valid &&
                   connected_device_name[0] == '\0') {
            hci_event_remote_name_request_complete_get_bd_addr(
                packet, address);
            if (same_address(address, connected_address) &&
                hci_event_remote_name_request_complete_get_status(packet) ==
                    ERROR_CODE_SUCCESS) {
                clean_name(
                    connected_device_name,
                    sizeof(connected_device_name),
                    hci_event_remote_name_request_complete_get_remote_name(
                        packet)
                );
                bluetooth::remember_device(
                    connected_address,
                    connected_device_name,
                    bluetooth::Profile::HidKeyboard
                );
            }
        }
        break;

    case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
        hci_event_user_passkey_notification_get_bd_addr(packet, address);
        if (authentication_target_locked(address)) {
            displayed_code =
                hci_event_user_passkey_notification_get_numeric_value(packet);
            displayed_code_valid = true;
            set_status("ENTER CODE ON KEYBOARD");
            set_error("TYPE CODE THEN PRESS ENTER");
        }
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
        if (pairing && target_valid) {
            const std::uint8_t result =
                hci_event_authentication_complete_get_status(packet);
            if (result != ERROR_CODE_SUCCESS) {
                char message[64] = {};
                std::snprintf(
                    message, sizeof(message),
                    "KEYBOARD PAIRING FAILED 0x%02X",
                    static_cast<unsigned>(result)
                );
                finish_connect_failure_locked(message);
            }
        }
        break;

    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        // BTstack's generated event helpers expose this shared BD_ADDR
        // position through the Link Key Request accessor.
        hci_event_link_key_request_get_bd_addr(packet, address);
        if (authentication_target_locked(address)) {
            bluetooth::remember_device(
                address,
                target_name,
                bluetooth::Profile::HidKeyboard
            );
            set_error("PAIRING KEY SAVED");
        }
        break;

    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            const std::uint16_t incoming_cid =
                hid_subevent_incoming_connection_get_hid_cid(packet);
            hid_subevent_incoming_connection_get_address(packet, address);
            bluetooth::DeviceRecord record = {};
            const bool trusted =
                authentication_target_locked(address) ||
                (bluetooth::lookup_device(address, record) &&
                 record.profile == bluetooth::Profile::HidKeyboard);
            if (!bluetooth_enabled || hid_cid || !trusted ||
                hid_subevent_incoming_connection_get_status(packet) !=
                    ERROR_CODE_SUCCESS) {
                hid_host_decline_connection(incoming_cid);
                break;
            }
            std::memcpy(target_address, address, sizeof(target_address));
            target_valid = true;
            connecting = true;
            if (record.name[0]) {
                clean_name(target_name, sizeof(target_name), record.name);
            }
            hid_cid = incoming_cid;
            hid_host_accept_connection(
                incoming_cid, HID_PROTOCOL_MODE_REPORT);
            set_status("CONNECTING");
            break;
        }

        case HID_SUBEVENT_CONNECTION_OPENED: {
            const std::uint8_t result =
                hid_subevent_connection_opened_get_status(packet);
            if (result != ERROR_CODE_SUCCESS) {
                char message[64] = {};
                std::snprintf(
                    message, sizeof(message),
                    "HID OPEN FAILED 0x%02X",
                    static_cast<unsigned>(result)
                );
                finish_connect_failure_locked(message);
                break;
            }
            hid_cid =
                hid_subevent_connection_opened_get_hid_cid(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, address);
            std::memcpy(
                connected_address, address, sizeof(connected_address));
            connected_address_valid = true;
            connecting = false;
            pairing = false;
            descriptor_available = false;
            clean_name(
                connected_device_name,
                sizeof(connected_device_name),
                target_name
            );
            bluetooth::remember_device(
                connected_address,
                connected_device_name,
                bluetooth::Profile::HidKeyboard
            );
            clear_target_locked();
            bluetooth_manager::restore_idle_classic_security_locked();
            set_status("CONNECTED - READING DESCRIPTOR");
            set_error("HID CONNECTED");
            if (connected_device_name[0] == '\0') {
                gap_remote_name_request(connected_address, 0x01, 0);
            }
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            if (hid_subevent_descriptor_available_get_status(packet) ==
                    ERROR_CODE_SUCCESS) {
                descriptor_available = true;
                set_status("CONNECTED");
                set_error("KEYBOARD READY");
            } else {
                descriptor_available = false;
                set_status("CONNECTED - NO DESCRIPTOR");
                set_error("HID DESCRIPTOR UNAVAILABLE");
            }
            break;

        case HID_SUBEVENT_REPORT:
            handle_hid_report_locked(
                hid_subevent_report_get_report(packet),
                hid_subevent_report_get_report_len(packet)
            );
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            hid_cid = 0;
            descriptor_available = false;
            connecting = false;
            pairing = false;
            connected_address_valid = false;
            std::memset(
                connected_address, 0, sizeof(connected_address));
            connected_device_name[0] = '\0';
            keyboard.reset();
            bluetooth_manager::restore_idle_classic_security_locked();
            set_status("DISCONNECTED");
            set_error("KEYBOARD DISCONNECTED");
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
}

} // namespace

void stack_init_locked() {
    if (protocol_initialized) return;
    hid_host_init(descriptor_storage, sizeof(descriptor_storage));
    hid_host_register_packet_handler(packet_handler);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE |
        LM_LINK_POLICY_ENABLE_ROLE_SWITCH
    );
    hci_events.callback = packet_handler;
    hci_add_event_handler(&hci_events);
    protocol_initialized = true;
}

void on_bluetooth_enabled_locked() {
    bluetooth_enabled = true;
    inquiry_active = false;
    scan_requested = false;
    pending_name_index = -1;
    connecting = false;
    pairing = false;
    hid_cid = 0;
    descriptor_available = false;
    connected_address_valid = false;
    connected_device_name[0] = '\0';
    clear_target_locked();
    keyboard.reset();
    set_status("DISCONNECTED");
    set_error("READY");
}

void on_bluetooth_disabled_locked() {
    if (inquiry_active) gap_inquiry_stop();
    if (hid_cid) hid_host_disconnect(hid_cid);
    bluetooth_enabled = false;
    inquiry_active = false;
    scan_requested = false;
    pending_name_index = -1;
    connecting = false;
    pairing = false;
    hid_cid = 0;
    descriptor_available = false;
    connected_address_valid = false;
    std::memset(connected_address, 0, sizeof(connected_address));
    connected_device_name[0] = '\0';
    clear_target_locked();
    keyboard.reset();
    set_status("DISCONNECTED");
    set_error("OFF");
}

void service_locked() {
    if (!bluetooth_enabled) return;
    if (connecting && static_cast<std::int32_t>(
            to_ms_since_boot(get_absolute_time()) -
            connect_deadline_ms) >= 0) {
        finish_connect_failure_locked("KEYBOARD CONNECT TIMEOUT");
    }
}

bool busy_locked() {
    return scan_requested || inquiry_active || pending_name_index >= 0 ||
           connecting || pairing || hid_cid != 0;
}

bool scanning_locked() {
    return scan_requested || inquiry_active || pending_name_index >= 0;
}

bool connected_to_locked(const std::uint8_t address[6]) {
    return address && hid_cid != 0 && connected_address_valid &&
           same_address(address, connected_address);
}

bool disconnect_address_locked(const std::uint8_t address[6]) {
    if (!hid_cid || (address && !connected_to_locked(address))) return false;
    hid_host_disconnect(hid_cid);
    set_status("DISCONNECTING");
    set_error("DISCONNECTING KEYBOARD");
    return true;
}

bool handle_pin_code_request_locked(const std::uint8_t address[6]) {
    if (!authentication_target_locked(address)) return false;

    displayed_code = 100000u + (get_rand_32() % 900000u);
    displayed_code_valid = true;
    char pin[7] = {};
    std::snprintf(
        pin, sizeof(pin), "%06lu",
        static_cast<unsigned long>(displayed_code)
    );
    bd_addr_t remote = {};
    std::memcpy(remote, address, sizeof(remote));
    gap_pin_code_response(remote, pin);
    set_status("ENTER CODE ON KEYBOARD");
    set_error("TYPE CODE THEN PRESS ENTER");
    return true;
}

bool handle_user_confirmation_request_locked(
    const std::uint8_t address[6],
    std::uint32_t numeric_value
) {
    if (!authentication_target_locked(address)) return false;
    displayed_code = numeric_value;
    displayed_code_valid = true;
    bd_addr_t remote = {};
    std::memcpy(remote, address, sizeof(remote));
    gap_ssp_confirmation_response(remote);
    set_status("PAIRING CONFIRMATION");
    set_error("PAIRING CONFIRMED");
    return true;
}

bool start_scan() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (!bluetooth_enabled || hci_get_state() != HCI_STATE_WORKING ||
        hid_cid || connecting) {
        set_error("SCAN NOT AVAILABLE");
        cyw43_arch_lwip_end();
        return false;
    }
    if (!bluetooth_manager::keyboard_pairing_available_locked()) {
        set_error("BLUETOOTH KEYBOARD NOT AVAILABLE");
        cyw43_arch_lwip_end();
        return false;
    }
    if (bluetooth_hid_ble::busy_locked() && !bluetooth_hid_ble::scanning_locked()) {
        set_error("DISCONNECT BLE KEYBOARD FIRST");
        cyw43_arch_lwip_end();
        return false;
    }
    if (scan_requested || inquiry_active || pending_name_index >= 0) {
        cyw43_arch_lwip_end();
        return true;
    }

    discovered_count = 0;
    pending_name_index = -1;
    std::memset(discovered, 0, sizeof(discovered));
    std::memset(name_attempted, 0, sizeof(name_attempted));
    std::memset(discovered_page_scan, 0, sizeof(discovered_page_scan));
    std::memset(discovered_clock_offset, 0, sizeof(discovered_clock_offset));
    scan_requested = true;
    inquiry_active = true;
    set_status("SCANNING CLASSIC DEVICES");
    set_error("SEARCHING CLASSIC DEVICES");
    gap_inquiry_start(kInquiryLength);
    cyw43_arch_lwip_end();
    return true;
}

void cancel_scan() {
    if (!wireless::initialized()) return;
    cyw43_arch_lwip_begin();
    scan_requested = false;
    if (inquiry_active) gap_inquiry_stop();
    inquiry_active = false;
    pending_name_index = -1;
    set_status("SCAN CANCELLED");
    set_error("SCAN CANCELLED");
    cyw43_arch_lwip_end();
}

bool scanning() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = scan_requested;
    cyw43_arch_lwip_end();
    return value;
}

std::size_t discovered_devices(
    DiscoveredDevice* output,
    std::size_t capacity
) {
    if (!output || capacity == 0 || !wireless::initialized()) return 0;
    cyw43_arch_lwip_begin();
    std::size_t count = 0;
    for (int pass = 0; pass < 2 && count < capacity; ++pass) {
        const bool keyboard_pass = pass == 0;
        for (std::size_t i = 0;
             i < discovered_count && count < capacity;
             ++i) {
            if (discovered[i].keyboard_hint != keyboard_pass) continue;
            output[count++] = discovered[i];
        }
    }
    cyw43_arch_lwip_end();
    return count;
}

bool connect_keyboard(const std::uint8_t address[6]) {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const int index = discovered_index(address);
    const char* name =
        index >= 0 ? discovered[index].name : nullptr;
    const bool result = begin_connect_locked(address, true, name);
    cyw43_arch_lwip_end();
    return result;
}

bool connect_paired_keyboard(const std::uint8_t address[6]) {
    if (!address || !wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    bluetooth::DeviceRecord record;
    const bool known = bluetooth::lookup_device(address, record) &&
        record.profile == bluetooth::Profile::HidKeyboard;
    const bool result = known &&
        begin_connect_locked(address, false, record.name);
    if (!known) set_error("NO PAIRED KEYBOARD");
    cyw43_arch_lwip_end();
    return result;
}

bool reconnect_paired_keyboard() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (!bluetooth_enabled || hid_cid || connecting) {
        set_error("RECONNECT NOT AVAILABLE");
        cyw43_arch_lwip_end();
        return false;
    }
    try_auto_reconnect_locked();
    const bool result = connecting || hid_cid != 0;
    if (!result) set_error("NO PAIRED KEYBOARD");
    cyw43_arch_lwip_end();
    return result;
}

void cancel_connect() {
    if (!wireless::initialized()) return;
    cyw43_arch_lwip_begin();
    finish_connect_failure_locked("KEYBOARD CONNECT CANCELLED");
    cyw43_arch_lwip_end();
}

bool connect_active() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = connecting;
    cyw43_arch_lwip_end();
    return value;
}

bool pairing_active() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = pairing;
    cyw43_arch_lwip_end();
    return value;
}

bool pairing_code_available() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = displayed_code_valid;
    cyw43_arch_lwip_end();
    return value;
}

std::uint32_t pairing_code() {
    if (!wireless::initialized()) return 0;
    cyw43_arch_lwip_begin();
    const std::uint32_t value = displayed_code;
    cyw43_arch_lwip_end();
    return value;
}

bool connected() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = hid_cid != 0 && connected_address_valid;
    cyw43_arch_lwip_end();
    return value;
}

bool connected_to(const std::uint8_t address[6]) {
    if (!address || !wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value =
        hid_cid != 0 && connected_address_valid &&
        same_address(address, connected_address);
    cyw43_arch_lwip_end();
    return value;
}

bool disconnect() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (!disconnect_address_locked(nullptr)) {
        set_error("KEYBOARD NOT CONNECTED");
        cyw43_arch_lwip_end();
        return false;
    }
    cyw43_arch_lwip_end();
    return true;
}

const char* connected_name() {
    if (!wireless::initialized()) return "";
    cyw43_arch_lwip_begin();
    std::snprintf(
        name_snapshot, sizeof(name_snapshot), "%s",
        connected_device_name
    );
    cyw43_arch_lwip_end();
    return name_snapshot;
}

const char* status() {
    if (!wireless::initialized()) return "OFF";
    cyw43_arch_lwip_begin();
    std::snprintf(status_snapshot, sizeof(status_snapshot), "%s", status_text);
    cyw43_arch_lwip_end();
    return status_snapshot;
}

const char* last_error() {
    if (!wireless::initialized()) return "OFF";
    cyw43_arch_lwip_begin();
    std::snprintf(error_snapshot, sizeof(error_snapshot), "%s", error_text);
    cyw43_arch_lwip_end();
    return error_snapshot;
}

void set_layout(KeyboardLayout value) {
    if (!wireless::initialized()) {
        keyboard.set_layout(value);
        return;
    }
    cyw43_arch_lwip_begin();
    keyboard.set_layout(value);
    cyw43_arch_lwip_end();
}

KeyboardLayout layout() {
    if (!wireless::initialized()) return keyboard.layout();
    cyw43_arch_lwip_begin();
    const KeyboardLayout value = keyboard.layout();
    cyw43_arch_lwip_end();
    return value;
}

bool last_key_repeat() { return keyboard.last_key_repeat(); }

int read_key() {
    if (!wireless::initialized()) return -1;
    cyw43_arch_lwip_begin();
    const int value =
        keyboard.read_key(to_ms_since_boot(get_absolute_time()));
    cyw43_arch_lwip_end();
    return value;
}

bool caps_lock_enabled() {
    if (!wireless::initialized()) return keyboard.caps_lock_enabled();
    cyw43_arch_lwip_begin();
    const bool value = keyboard.caps_lock_enabled();
    cyw43_arch_lwip_end();
    return value;
}

bool shift_held() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = keyboard.shift_held();
    cyw43_arch_lwip_end();
    return value;
}

void set_caps_lock(bool enabled) {
    if (!wireless::initialized()) {
        keyboard.set_caps_lock(enabled);
        return;
    }
    cyw43_arch_lwip_begin();
    keyboard.set_caps_lock(enabled);
    cyw43_arch_lwip_end();
}

std::uint32_t key_overflow_count() {
    if (!wireless::initialized()) return 0;
    cyw43_arch_lwip_begin();
    const std::uint32_t value = keyboard.overflow_count();
    cyw43_arch_lwip_end();
    return value;
}

} // namespace rmb::bluetooth_hid
