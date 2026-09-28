#include "bluetooth_hid_ble_keyboard.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bluetooth_hid_keyboard.hpp"
#include "bluetooth_manager.hpp"
#include "wireless.hpp"

#include "btstack.h"
#include "ble/gatt-service/hids_host.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

namespace rmb::bluetooth_hid_ble {
namespace {

constexpr std::uint32_t kConnectTimeoutMs = 30000;

enum class State : std::uint8_t {
    Off,
    Idle,
    Scanning,
    Connecting,
    CancellingConnect,
    Pairing,
    DiscoveringReports,
    DiscoveringService,
    DiscoveringCharacteristics,
    EnablingNotifications,
    SwitchingProtocol,
    Ready,
    Disconnecting
};

bluetooth_hid::KeyboardCore keyboard;
State state = State::Off;
bool protocol_initialized = false;
bool bluetooth_enabled = false;

DiscoveredDevice discovered[kMaxDiscoveredDevices] = {};
std::size_t discovered_count = 0;
bool discovery_full = false;
// Classification/name survive scans, but only freshly observed devices are shown.
bool seen_in_scan[kMaxDiscoveredDevices] = {};

bd_addr_t target_address = {};
bd_addr_type_t target_address_type = static_cast<bd_addr_type_t>(0);
char target_name[40] = {};
hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
std::uint32_t connect_deadline_ms = 0;
std::uint32_t displayed_code = 0;
bool displayed_code_valid = false;
std::uint16_t hids_cid = 0;
std::uint8_t report_descriptors[1536] = {};
bool boot_fallback_pending = false;
bool report_mode = false;
bd_addr_t identity_address = {};
std::uint8_t identity_address_type = 0xff;

gatt_client_service_t hid_service = {};
gatt_client_characteristic_t boot_input_characteristic = {};
gatt_client_characteristic_t protocol_mode_characteristic = {};
gatt_client_notification_t keyboard_notifications = {};
bool notification_listener_active = false;
btstack_context_callback_registration_t gatt_request = {};

btstack_packet_callback_registration_t hci_events = {};
btstack_packet_callback_registration_t sm_events = {};

char status_text[48] = "OFF";
char error_text[96] = "OFF";
char status_snapshot[48] = "OFF";
char error_snapshot[96] = "OFF";
char name_snapshot[40] = {};

void set_status(const char* text) {
    std::snprintf(status_text, sizeof(status_text), "%s", text ? text : "");
}

void set_error(const char* text) {
    std::snprintf(error_text, sizeof(error_text), "%s", text ? text : "");
}

bool same_address(
    const std::uint8_t lhs[6],
    const std::uint8_t rhs[6]
) {
    return lhs && rhs && std::memcmp(lhs, rhs, 6) == 0;
}

void clean_name(char* output, std::size_t capacity,
                const std::uint8_t* input, std::size_t length) {
    if (!output || capacity == 0) return;
    std::size_t out = 0;
    if (input) {
        for (std::size_t i = 0; i < length && out + 1 < capacity; ++i) {
            const unsigned char ch = input[i];
            output[out++] = ch >= 0x20 && ch <= 0x7e
                ? static_cast<char>(ch) : '?';
        }
    }
    output[out] = '\0';
}

void copy_target_name(const char* name) {
    clean_name(
        target_name,
        sizeof(target_name),
        reinterpret_cast<const std::uint8_t*>(name),
        name ? std::strlen(name) : 0
    );
}

int find_discovered(
    const std::uint8_t address[6],
    std::uint8_t address_type
) {
    for (std::size_t i = 0; i < discovered_count; ++i) {
        if (discovered[i].address_type == address_type &&
            same_address(discovered[i].address, address)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void configure_security() {
    // Pico SDK 2.3.1 BTstack defaults to SC-only in sm_init(). AuthReq flags
    // do not change that policy. Advertise SC support and allow legacy peers.
    sm_set_secure_connections_only_mode(false);
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(
        SM_AUTHREQ_SECURE_CONNECTION |
        SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING);
}

void prepare_discovery_session() {
    // Keep only previously identified HID candidates. Never display a cached
    // device until an advertisement from the same address/type arrives again.
    std::size_t retained = 0;
    for (std::size_t i = 0; i < discovered_count; ++i) {
        if (!discovered[i].hid_hint) continue;
        discovered[retained++] = discovered[i];
    }
    discovered_count = retained;
    std::memset(seen_in_scan, 0, sizeof(seen_in_scan));
    discovery_full = false;
}

void forget_matching_bond(
    const std::uint8_t address[6],
    std::uint8_t address_type
) {
    const int count = le_device_db_max_count();
    for (int index = 0; index < count; ++index) {
        int stored_type = static_cast<int>(BD_ADDR_TYPE_UNKNOWN);
        bd_addr_t stored_address = {};
        le_device_db_info(
            index, &stored_type, stored_address, nullptr);
        if (stored_type == static_cast<int>(address_type) &&
            same_address(stored_address, address)) {
            le_device_db_remove(index);
        }
    }
}

const char* pairing_failure_text(std::uint8_t reason) {
    switch (reason) {
    case SM_REASON_PASSKEY_ENTRY_FAILED:
        return "PASSKEY REJECTED; RETRY PAIR MODE";
    case SM_REASON_AUTHENTHICATION_REQUIREMENTS:
        return "AUTH REQUIREMENTS NOT MET";
    case SM_REASON_CONFIRM_VALUE_FAILED:
    case SM_REASON_DHKEY_CHECK_FAILED:
    case SM_REASON_NUMERIC_COMPARISON_FAILED:
    case SM_REASON_KEY_REJECTED:
        return "KEY CHECK FAILED; RETRY PAIR MODE";
    case SM_REASON_PAIRING_NOT_SUPPORTED:
        return "PAIRING NOT SUPPORTED BY DEVICE";
    case SM_REASON_ENCRYPTION_KEY_SIZE:
        return "ENCRYPTION KEY SIZE REJECTED";
    case SM_REASON_REPEATED_ATTEMPTS:
        return "TOO MANY ATTEMPTS; WAIT AND RETRY";
    default:
        return nullptr;
    }
}

void extract_name(
    const std::uint8_t* data,
    std::uint8_t length,
    char* output,
    std::size_t capacity
) {
    if (!output || capacity == 0) return;
    ad_context_t iterator;
    ad_iterator_init(&iterator, length, data);
    while (ad_iterator_has_more(&iterator)) {
        ad_iterator_next(&iterator);
        const std::uint8_t type = ad_iterator_get_data_type(&iterator);
        if (type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME &&
            type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
            continue;
        }
        clean_name(
            output,
            capacity,
            ad_iterator_get_data(&iterator),
            ad_iterator_get_data_len(&iterator)
        );
        return;
    }
}

bool advertisement_has_keyboard_appearance(
    const std::uint8_t* data,
    std::uint8_t length
) {
    // Bluetooth Assigned Number 0x03C1 = HID Keyboard appearance.
    ad_context_t iterator;
    ad_iterator_init(&iterator, length, data);
    while (ad_iterator_has_more(&iterator)) {
        ad_iterator_next(&iterator);
        if (ad_iterator_get_data_type(&iterator) != 0x19 ||
            ad_iterator_get_data_len(&iterator) < 2) {
            continue;
        }
        return little_endian_read_16(
            ad_iterator_get_data(&iterator), 0) == 0x03c1;
    }
    return false;
}

void handle_advertisement(std::uint8_t* packet) {
    if (state != State::Scanning) return;

    bd_addr_t address = {};
    gap_event_advertising_report_get_address(packet, address);
    const std::uint8_t address_type =
        gap_event_advertising_report_get_address_type(packet);
    const std::uint8_t data_length =
        gap_event_advertising_report_get_data_length(packet);
    const std::uint8_t* data =
        gap_event_advertising_report_get_data(packet);
    const bool keyboard_hint =
        ad_data_contains_uuid16(
            data_length,
            data,
            ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE
        ) ||
        advertisement_has_keyboard_appearance(data, data_length);

    int index = find_discovered(address, address_type);
    if (index < 0) {
        if (discovered_count >= kMaxDiscoveredDevices) {
            if (!keyboard_hint) return;

            // Unrelated advertisements must not hide a later keyboard.
            std::size_t replacement = kMaxDiscoveredDevices;
            for (std::size_t i = 0; i < discovered_count; ++i) {
                if (!discovered[i].hid_hint) {
                    replacement = i;
                    break;
                }
            }
            if (replacement == kMaxDiscoveredDevices) {
                discovery_full = true;
                return;
            }
            index = static_cast<int>(replacement);
            discovered[index] = DiscoveredDevice{};
        } else {
            index = static_cast<int>(discovered_count++);
            discovered[index] = DiscoveredDevice{};
        }
        std::memcpy(
            discovered[index].address,
            address,
            sizeof(discovered[index].address)
        );
        discovered[index].address_type = address_type;
    }

    seen_in_scan[index] = true;
    auto& item = discovered[index];
    item.rssi = gap_event_advertising_report_get_rssi(packet);
    item.hid_hint = item.hid_hint || keyboard_hint;
    if (!item.name[0]) {
        extract_name(data, data_length, item.name, sizeof(item.name));
    }
}

void stop_notification_listener() {
    if (!notification_listener_active) return;
    gatt_client_stop_listening_for_characteristic_value_updates(
        &keyboard_notifications
    );
    notification_listener_active = false;
    std::memset(
        &keyboard_notifications,
        0,
        sizeof(keyboard_notifications)
    );
}

void clear_gatt_state() {
    stop_notification_listener();
    // HIDS owns its pending queries until the ACL disconnection event.
    boot_fallback_pending = false;
    std::memset(&hid_service, 0, sizeof(hid_service));
    std::memset(
        &boot_input_characteristic,
        0,
        sizeof(boot_input_characteristic)
    );
    std::memset(
        &protocol_mode_characteristic,
        0,
        sizeof(protocol_mode_characteristic)
    );
}

void reset_connection_state() {
    clear_gatt_state();
    hids_cid = 0;
    report_mode = false;
    identity_address_type = 0xff;
    connection_handle = HCI_CON_HANDLE_INVALID;
    connect_deadline_ms = 0;
    displayed_code = 0;
    displayed_code_valid = false;
    keyboard.reset();
}

void fail_connection(const char* message) {
    set_status("FAILED");
    set_error(message);
    keyboard.reset();
    displayed_code_valid = false;

    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gatt_client_remove_gatt_query(&gatt_request, connection_handle);
        clear_gatt_state();
        state = State::Disconnecting;
        gap_disconnect(connection_handle);
    } else if (state == State::Connecting) {
        state = State::CancellingConnect;
        gap_connect_cancel();
    } else {
        state = State::Idle;
    }
}

void request_gatt_query();

void mark_ready() {
    state = State::Ready;
    connect_deadline_ms = 0;
    displayed_code_valid = false;
    set_status(report_mode ? "CONNECTED / REPORT MODE" : "CONNECTED / BOOT MODE");
    std::memcpy(identity_address, target_address, sizeof(identity_address));
    identity_address_type = target_address_type;
    const int index = sm_le_device_index(connection_handle);
    if (index >= 0) {
        int type = 0xff;
        sm_key_t irk = {};
        le_device_db_info(index, &type, identity_address, irk);
        identity_address_type = static_cast<std::uint8_t>(type);
    }
    if (index < 0) {
        set_error("CONNECTED; NO SAVED BOND");
    } else if (!bluetooth::remember_device(identity_address, target_name,
                   bluetooth::Profile::BleKeyboard, identity_address_type)) {
        set_error("CONNECTED; NAME SAVE FAILED");
    } else {
        set_error("BLE KEYBOARD READY / BOND SAVED");
    }
}

void handle_gatt_event(
    std::uint8_t,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
);

void send_next_query_with_handler(void*) {
    std::uint8_t result = ERROR_CODE_COMMAND_DISALLOWED;
    switch (state) {
    case State::DiscoveringService:
        result = gatt_client_discover_primary_services_by_uuid16(
            handle_gatt_event,
            connection_handle,
            ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE
        );
        break;
    case State::DiscoveringCharacteristics:
        result = gatt_client_discover_characteristics_for_service(
            handle_gatt_event,
            connection_handle,
            &hid_service
        );
        break;
    case State::EnablingNotifications:
        result = gatt_client_write_client_characteristic_configuration(
            handle_gatt_event,
            connection_handle,
            &boot_input_characteristic,
            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION
        );
        break;
    default:
        return;
    }
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message,
            sizeof(message),
            "GATT START FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        fail_connection(message);
    }
}

void request_gatt_query() {
    gatt_request.callback = send_next_query_with_handler;
    const std::uint8_t result =
        gatt_client_request_to_send_gatt_query(
            &gatt_request,
            connection_handle
        );
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message,
            sizeof(message),
            "GATT QUEUE FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        fail_connection(message);
    }
}

void send_boot_protocol(void*) {
    static std::uint8_t boot_mode = 0;
    const std::uint8_t result =
        gatt_client_write_value_of_characteristic_without_response(
            connection_handle,
            protocol_mode_characteristic.value_handle,
            1,
            &boot_mode
        );
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message,
            sizeof(message),
            "BOOT MODE FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        fail_connection(message);
        return;
    }
    mark_ready();
}

void request_boot_protocol() {
    state = State::SwitchingProtocol;
    gatt_request.callback = send_boot_protocol;
    const std::uint8_t result =
        gatt_client_request_to_write_without_response(
            &gatt_request,
            connection_handle
        );
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message,
            sizeof(message),
            "BOOT MODE QUEUE FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        fail_connection(message);
    }
}

void handle_boot_report(
    std::uint8_t packet_type,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
) {
    if (packet_type != HCI_EVENT_PACKET ||
        hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION ||
        state != State::Ready) {
        return;
    }
    const std::uint16_t length =
        gatt_event_notification_get_value_length(packet);
    const std::uint8_t* value =
        gatt_event_notification_get_value(packet);
    if (!value || length < 8) return;

    keyboard.handle_report(
        value[0],
        value + 2,
        bluetooth_hid::KeyboardCore::kMaxKeys,
        to_ms_since_boot(get_absolute_time())
    );
}

void handle_gatt_event(
    std::uint8_t packet_type,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
) {
    if (packet_type != HCI_EVENT_PACKET) return;
    const std::uint8_t event = hci_event_packet_get_type(packet);

    if (state == State::DiscoveringService) {
        if (event == GATT_EVENT_SERVICE_QUERY_RESULT) {
            gatt_event_service_query_result_get_service(packet, &hid_service);
            return;
        }
        if (event != GATT_EVENT_QUERY_COMPLETE) return;
        if (gatt_event_query_complete_get_att_status(packet) !=
            ATT_ERROR_SUCCESS) {
            fail_connection("HID SERVICE QUERY FAILED");
            return;
        }
        if (hid_service.start_group_handle == 0) {
            fail_connection("HID SERVICE 0x1812 NOT FOUND");
            return;
        }
        state = State::DiscoveringCharacteristics;
        set_status("READING HID CHARACTERISTICS");
        request_gatt_query();
        return;
    }

    if (state == State::DiscoveringCharacteristics) {
        if (event == GATT_EVENT_CHARACTERISTIC_QUERY_RESULT) {
            gatt_client_characteristic_t characteristic = {};
            gatt_event_characteristic_query_result_get_characteristic(
                packet,
                &characteristic
            );
            if (characteristic.uuid16 ==
                ORG_BLUETOOTH_CHARACTERISTIC_BOOT_KEYBOARD_INPUT_REPORT) {
                boot_input_characteristic = characteristic;
            } else if (characteristic.uuid16 ==
                       ORG_BLUETOOTH_CHARACTERISTIC_PROTOCOL_MODE) {
                protocol_mode_characteristic = characteristic;
            }
            return;
        }
        if (event != GATT_EVENT_QUERY_COMPLETE) return;
        if (gatt_event_query_complete_get_att_status(packet) !=
            ATT_ERROR_SUCCESS) {
            fail_connection("HID CHARACTERISTIC QUERY FAILED");
            return;
        }
        if (boot_input_characteristic.value_handle == 0) {
            fail_connection("NO USABLE KEYBOARD REPORT / BOOT INPUT");
            return;
        }
        state = State::EnablingNotifications;
        set_status("ENABLING BOOT REPORTS");
        request_gatt_query();
        return;
    }

    if (state == State::EnablingNotifications &&
        event == GATT_EVENT_QUERY_COMPLETE) {
        if (gatt_event_query_complete_get_att_status(packet) !=
            ATT_ERROR_SUCCESS) {
            fail_connection("BOOT NOTIFICATION ENABLE FAILED");
            return;
        }
        gatt_client_listen_for_characteristic_value_updates(
            &keyboard_notifications,
            handle_boot_report,
            connection_handle,
            &boot_input_characteristic
        );
        notification_listener_active = true;
        if (protocol_mode_characteristic.value_handle != 0) {
            set_status("SWITCHING TO BOOT MODE");
            request_boot_protocol();
        } else {
            // Some boot-only keyboards omit Protocol Mode.
            mark_ready();
        }
    }
}

void begin_boot_discovery() {
    clear_gatt_state();
    state = State::DiscoveringService;
    set_status("DISCOVERING HID SERVICE");
    set_error("SECURE LINK ESTABLISHED");
    request_gatt_query();
}

void handle_report_protocol(std::uint8_t service_index,
                            const std::uint8_t* report, std::uint16_t length) {
    const auto* descriptor = hids_host_descriptor_storage_get_descriptor_data(hids_cid, service_index);
    const auto descriptor_length = hids_host_descriptor_storage_get_descriptor_len(hids_cid, service_index);
    if (!descriptor || !descriptor_length || !report || length < 1) return;
    // This BTstack HIDS host prepends Report Reference's ID, including zero
    // for descriptors with no Report ID item.
    const bool has_id = btstack_hid_report_id_declared(descriptor, descriptor_length);
    const int expected = btstack_hid_get_report_size_for_id(
        has_id ? report[0] : HID_REPORT_ID_UNDEFINED, HID_REPORT_TYPE_INPUT,
        descriptor, descriptor_length);
    if (expected <= 0 || length < expected + 1) return;
    if (!has_id) {
        ++report;
        --length;
    }
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, descriptor, descriptor_length,
                           HID_REPORT_TYPE_INPUT, report, length);
    std::uint8_t modifiers = 0;
    std::uint8_t usages[bluetooth_hid::KeyboardCore::kMaxKeys] = {};
    std::size_t count = 0;
    bool keyboard_report = false;
    while (btstack_hid_parser_has_more(&parser)) {
        std::uint16_t page = 0, usage = 0;
        std::int32_t value = 0;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (page != HID_USAGE_PAGE_KEYBOARD) continue;
        keyboard_report = true;
        if (usage >= 0xe0 && usage <= 0xe7) {
            if (value) modifiers |= static_cast<std::uint8_t>(1u << (usage - 0xe0));
        } else if (value && usage && usage <= 0xff && count < sizeof(usages)) {
            usages[count++] = static_cast<std::uint8_t>(usage);
        }
    }
    // Consumer/mouse reports must not release keys from a keyboard report.
    if (keyboard_report) keyboard.handle_report(modifiers, usages, count,
                              to_ms_since_boot(get_absolute_time()));
}

void hids_packet_handler(std::uint8_t packet_type, std::uint16_t,
                        std::uint8_t* packet, std::uint16_t size) {
    // The SDK sends report notifications with GATTSERVICE_META as packet_type.
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) ||
        size < 5 || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META ||
        little_endian_read_16(packet, 3) != hids_cid) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
        if (state != State::DiscoveringReports || size < 8) return;
        if (gattservice_subevent_hid_service_connected_get_status(packet) != ERROR_CODE_SUCCESS) {
            // The SDK finalizes after this callback; defer fallback until then.
            hids_cid = 0;
            boot_fallback_pending = true;
            set_status("TRYING BOOT KEYBOARD");
            return;
        }
        {
            bool keyboard_found = false;
            const auto instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);
            for (std::uint8_t i = 0; i < instances; ++i) {
                const auto* descriptor = hids_host_descriptor_storage_get_descriptor_data(hids_cid, i);
                const auto length = hids_host_descriptor_storage_get_descriptor_len(hids_cid, i);
                if (!descriptor || !length) continue;
                btstack_hid_usage_iterator_t it;
                btstack_hid_usage_iterator_init(&it, descriptor, length, HID_REPORT_TYPE_INPUT);
                while (btstack_hid_usage_iterator_has_more(&it)) {
                    btstack_hid_usage_item_t item;
                    btstack_hid_usage_iterator_get_item(&it, &item);
                    if (item.usage_page == HID_USAGE_PAGE_KEYBOARD) keyboard_found = true;
                }
            }
            if (!keyboard_found) {
                boot_fallback_pending = true;
                set_status("TRYING BOOT KEYBOARD");
                return;
            }
            report_mode = true;
            mark_ready();
        }
        break;
    case GATTSERVICE_SUBEVENT_HID_REPORT:
        if (state != State::Ready || !report_mode || size < 9) return;
        if (gattservice_subevent_hid_report_get_report_len(packet) > size - 9) return;
        handle_report_protocol(gattservice_subevent_hid_report_get_service_index(packet),
                               gattservice_subevent_hid_report_get_report(packet),
                               gattservice_subevent_hid_report_get_report_len(packet));
        break;
    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
        hids_cid = 0;
        break;
    default: break;
    }
}

void begin_hid_discovery() {
    clear_gatt_state();
    state = State::DiscoveringReports;
    set_status("DISCOVERING HID REPORTS");
    set_error("SECURE LINK ESTABLISHED");
    const auto result = hids_host_connect(connection_handle, hids_packet_handler,
                                          HID_PROTOCOL_MODE_REPORT, &hids_cid);
    if (result != ERROR_CODE_SUCCESS) {
        hids_cid = 0;
        fail_connection("HIDS CLIENT START FAILED");
    }
}

void sm_packet_handler(
    std::uint8_t packet_type,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
) {
    if (packet_type != HCI_EVENT_PACKET || state != State::Pairing ||
        connection_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_JUST_WORKS_REQUEST:
        if (sm_event_just_works_request_get_handle(packet) ==
            connection_handle) {
            sm_just_works_confirm(connection_handle);
            set_status("PAIRING / JUST WORKS");
        }
        break;
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        if (sm_event_numeric_comparison_request_get_handle(packet) ==
            connection_handle) {
            displayed_code =
                sm_event_numeric_comparison_request_get_passkey(packet);
            displayed_code_valid = true;
            sm_numeric_comparison_confirm(connection_handle);
            set_status("PAIRING / CONFIRM CODE");
        }
        break;
    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        if (sm_event_passkey_display_number_get_handle(packet) ==
            connection_handle) {
            displayed_code =
                sm_event_passkey_display_number_get_passkey(packet);
            displayed_code_valid = true;
            connect_deadline_ms = to_ms_since_boot(get_absolute_time()) + 60000;
            set_status("ENTER CODE ON KEYBOARD");
            set_error("TYPE CODE THEN PRESS ENTER");
        }
        break;
    case SM_EVENT_PAIRING_COMPLETE:
        if (sm_event_pairing_complete_get_handle(packet) !=
            connection_handle) {
            break;
        }
        {
            const std::uint8_t pairing_status =
                sm_event_pairing_complete_get_status(packet);
            if (pairing_status == ERROR_CODE_SUCCESS) {
                begin_hid_discovery();
            } else if (pairing_status ==
                       ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION) {
                fail_connection(
                    "REMOTE ENDED PAIRING; RE-ENTER PAIR MODE"
                );
            } else if (pairing_status ==
                       ERROR_CODE_AUTHENTICATION_FAILURE) {
                const std::uint8_t reason =
                    sm_event_pairing_complete_get_reason(packet);
                const char* detail = pairing_failure_text(reason);
                char message[96] = {};
                if (detail) {
                    std::snprintf(
                        message, sizeof(message),
                        "%s (SMP 0x%02X)",
                        detail,
                        static_cast<unsigned>(reason)
                    );
                } else {
                    std::snprintf(
                        message, sizeof(message),
                        "AUTH FAILED SMP 0x%02X; RETRY PAIR MODE",
                        static_cast<unsigned>(reason)
                    );
                }
                fail_connection(message);
            } else {
                char message[64] = {};
                std::snprintf(
                    message,
                    sizeof(message),
                    "BLE PAIRING FAILED 0x%02X",
                    static_cast<unsigned>(pairing_status)
                );
                fail_connection(message);
            }
        }
        break;
    case SM_EVENT_REENCRYPTION_COMPLETE:
        if (sm_event_reencryption_complete_get_handle(packet) !=
            connection_handle) {
            break;
        }
        if (sm_event_reencryption_complete_get_status(packet) ==
            ERROR_CODE_SUCCESS) {
            begin_hid_discovery();
        } else {
            fail_connection("BLE RE-ENCRYPTION FAILED");
        }
        break;
    default:
        break;
    }
}

void hci_packet_handler(
    std::uint8_t packet_type,
    std::uint16_t,
    std::uint8_t* packet,
    std::uint16_t
) {
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case GAP_EVENT_ADVERTISING_REPORT:
        handle_advertisement(packet);
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) !=
            GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            break;
        }
        if (state == State::CancellingConnect) {
            if (gap_subevent_le_connection_complete_get_status(packet) ==
                ERROR_CODE_SUCCESS) {
                connection_handle =
                    gap_subevent_le_connection_complete_get_connection_handle(packet);
                state = State::Disconnecting;
                gap_disconnect(connection_handle);
            } else {
                reset_connection_state();
                state = bluetooth_enabled ? State::Idle : State::Off;
            }
            break;
        }
        if (state != State::Connecting) break;
        if (gap_subevent_le_connection_complete_get_status(packet) !=
            ERROR_CODE_SUCCESS) {
            const unsigned failure =
                gap_subevent_le_connection_complete_get_status(packet);
            reset_connection_state();
            state = bluetooth_enabled ? State::Idle : State::Off;
            char message[64] = {};
            std::snprintf(message, sizeof(message),
                          "BLE CONNECT FAILED HCI 0x%02X", failure);
            set_status("FAILED");
            set_error(message);
            break;
        }
        connection_handle =
            gap_subevent_le_connection_complete_get_connection_handle(packet);
        state = State::Pairing;
        set_status("PAIRING / ENCRYPTING");
        set_error("WAITING FOR BLE SECURITY");
        sm_request_pairing(connection_handle);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        const hci_con_handle_t disconnected =
            hci_event_disconnection_complete_get_connection_handle(packet);
        if (disconnected != connection_handle) break;
        const bool was_ready = state == State::Ready;
        const bool keep_error = state == State::Disconnecting;
        reset_connection_state();
        state = bluetooth_enabled ? State::Idle : State::Off;
        set_status("DISCONNECTED");
        if (!keep_error) {
            set_error(
                was_ready ? "BLE KEYBOARD DISCONNECTED" : "BLE LINK CLOSED"
            );
        }
        break;
    }
    default:
        break;
    }
}

bool is_connect_active() {
    switch (state) {
    case State::Connecting:
    case State::Pairing:
    case State::DiscoveringReports:
    case State::DiscoveringService:
    case State::DiscoveringCharacteristics:
    case State::EnablingNotifications:
    case State::SwitchingProtocol:
        return true;
    default:
        return false;
    }
}

} // namespace

void stack_init_locked() {
    if (protocol_initialized) return;
    gatt_client_init();
    sm_init();
    configure_security();

    hids_host_init(report_descriptors, sizeof(report_descriptors));

    hci_events.callback = hci_packet_handler;
    hci_add_event_handler(&hci_events);
    sm_events.callback = sm_packet_handler;
    sm_add_event_handler(&sm_events);
    protocol_initialized = true;
}

void on_bluetooth_enabled_locked() {
    bluetooth_enabled = true;
    state = State::Idle;
    discovered_count = 0;
    discovery_full = false;
    reset_connection_state();
    set_status("DISCONNECTED");
    set_error("READY - BLE SCAN AVAILABLE");
}

void on_bluetooth_disabled_locked() {
    if (state == State::Scanning) gap_stop_scan();
    if (state == State::Connecting) gap_connect_cancel();
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    bluetooth_enabled = false;
    state = State::Off;
    discovered_count = 0;
    discovery_full = false;
    reset_connection_state();
    set_status("OFF");
    set_error("OFF");
}

void service_locked() {
    if (!bluetooth_enabled) return;
    if (boot_fallback_pending && state == State::DiscoveringReports) {
        boot_fallback_pending = false;
        if (hids_cid) {
            hids_host_disconnect(hids_cid);
            hids_cid = 0;
        }
        report_mode = false;
        begin_boot_discovery();
    }
    const std::uint32_t now = to_ms_since_boot(get_absolute_time());
    if (is_connect_active() &&
        static_cast<std::int32_t>(now - connect_deadline_ms) >= 0) {
        fail_connection("BLE KEYBOARD CONNECT TIMEOUT");
    }
}

bool busy_locked() {
    return state == State::Scanning ||
           state == State::CancellingConnect ||
           is_connect_active() ||
           state == State::Ready ||
           state == State::Disconnecting ||
           connection_handle != HCI_CON_HANDLE_INVALID;
}

bool scanning_locked() { return state == State::Scanning; }

bool manager_allows_keyboard_request(bool fresh_pairing) {
    return fresh_pairing
        ? bluetooth_manager::keyboard_pairing_available_locked()
        : bluetooth_manager::keyboard_reconnect_available_locked();
}

void disconnect_locked() {
    if (state == State::Scanning) {
        gap_stop_scan();
        state = State::Idle;
    }
    if (state == State::Connecting) {
        state = State::CancellingConnect;
        gap_connect_cancel();
    }
    if (connection_handle != HCI_CON_HANDLE_INVALID &&
        state != State::Disconnecting) {
        gatt_client_remove_gatt_query(&gatt_request, connection_handle);
        clear_gatt_state();
        state = State::Disconnecting;
        gap_disconnect(connection_handle);
        set_status("DISCONNECTING");
    }
    keyboard.reset();
}

void forget_all_bonds_locked() {
    disconnect_locked();
    const int count = le_device_db_max_count();
    for (int index = 0; index < count; ++index) {
        le_device_db_remove(index);
    }
}

bool start_scan() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (!bluetooth_enabled || hci_get_state() != HCI_STATE_WORKING ||
        busy_locked()) {
        set_error("BLE SCAN NOT AVAILABLE");
        cyw43_arch_lwip_end();
        return false;
    }
    if (!bluetooth_manager::keyboard_pairing_available_locked() ||
        (bluetooth_hid::busy_locked() && !bluetooth_hid::scanning_locked())) {
        set_error("DISCONNECT CLASSIC KEYBOARD FIRST");
        cyw43_arch_lwip_end();
        return false;
    }

    prepare_discovery_session();
    gap_set_scan_duplicate_filter(false);
    gap_set_scan_parameters(1, 48, 48);
    state = State::Scanning;
    gap_start_scan();
    set_status("SCANNING BLE ADVERTISEMENTS");
    set_error("ACTIVE SCAN: BLE KEYBOARDS");
    cyw43_arch_lwip_end();
    return true;
}

void cancel_scan() {
    if (!wireless::initialized()) return;
    cyw43_arch_lwip_begin();
    if (state == State::Scanning) {
        gap_stop_scan();
        state = State::Idle;
        set_status("SCAN CANCELLED");
        set_error("BLE SCAN CANCELLED");
    }
    cyw43_arch_lwip_end();
}

bool scanning() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = state == State::Scanning;
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
    for (std::size_t i = 0;
         i < discovered_count && count < capacity;
         ++i) {
        if (!discovered[i].hid_hint || !seen_in_scan[i]) continue;
        output[count++] = discovered[i];
    }
    cyw43_arch_lwip_end();
    return count;
}

bool discovery_capacity_reached() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = discovery_full;
    cyw43_arch_lwip_end();
    return value;
}

static bool connect_keyboard_impl(
    const std::uint8_t address[6],
    std::uint8_t address_type,
    const char* name,
    bool fresh_pairing
) {
    if (!address || !wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (!bluetooth_enabled || hci_get_state() != HCI_STATE_WORKING ||
        busy_locked() ||
        !manager_allows_keyboard_request(fresh_pairing) ||
        bluetooth_hid::busy_locked()) {
        set_error("BLE CONNECT NOT AVAILABLE");
        cyw43_arch_lwip_end();
        return false;
    }

    // Pair New Device is an explicit fresh-pair operation. Remove a stale
    // key for this address without disturbing bonds for other BLE devices.
    if (fresh_pairing) forget_matching_bond(address, address_type);

    std::memcpy(target_address, address, sizeof(target_address));
    target_address_type = static_cast<bd_addr_type_t>(address_type);
    copy_target_name(name);
    displayed_code = 0;
    displayed_code_valid = false;
    report_mode = false;
    hids_cid = 0;
    keyboard.reset();
    clear_gatt_state();
    connect_deadline_ms =
        to_ms_since_boot(get_absolute_time()) + kConnectTimeoutMs;

    configure_security();
    const std::uint8_t result =
        gap_connect(target_address, target_address_type);
    if (result != ERROR_CODE_SUCCESS) {
        char message[64] = {};
        std::snprintf(
            message,
            sizeof(message),
            "BLE CONNECT START FAILED 0x%02X",
            static_cast<unsigned>(result)
        );
        set_error(message);
        cyw43_arch_lwip_end();
        return false;
    }

    state = State::Connecting;
    set_status("CONNECTING BLE");
    set_error("WAITING FOR LE CONNECTION");
    cyw43_arch_lwip_end();
    return true;
}

bool connect_keyboard(const std::uint8_t address[6], std::uint8_t address_type, const char* name) {
    return connect_keyboard_impl(address, address_type, name, true);
}

bool connect_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type, const char* name) {
    return connect_keyboard_impl(address, address_type, name, false);
}

bool connected_to_locked(const std::uint8_t address[6], std::uint8_t address_type) {
    return state == State::Ready && identity_address_type == address_type &&
           same_address(identity_address, address);
}

std::size_t paired_devices_locked(bluetooth_manager::PairedDeviceInfo* output, std::size_t capacity) {
    std::size_t count = 0;
    for (int i = 0; i < le_device_db_max_count() && count < capacity; ++i) {
        int type = 0xff;
        bd_addr_t address = {};
        sm_key_t irk = {};
        le_device_db_info(i, &type, address, irk);
        if (type != 0 && type != 1) continue;
        int key_size = 0;
        le_device_db_encryption_get(i, nullptr, nullptr, nullptr, &key_size, nullptr, nullptr, nullptr);
        if (!key_size) continue;
        bluetooth::DeviceRecord record;
        const bool known_ble =
            bluetooth::lookup_device(
                address, record, static_cast<std::uint8_t>(type)) &&
            record.profile == bluetooth::Profile::BleKeyboard;
        if (!known_ble) continue;
        auto& item = output[count++];
        item = bluetooth_manager::PairedDeviceInfo{};
        std::memcpy(item.address, address, sizeof(item.address));
        item.address_type = static_cast<std::uint8_t>(type);
        item.profile = bluetooth::Profile::BleKeyboard;
        item.connected = connected_to_locked(address, item.address_type);
        if (known_ble)
            std::snprintf(item.name, sizeof(item.name), "%s", record.name);
        if (!item.name[0]) std::snprintf(item.name, sizeof(item.name), "%s",
                                       item.connected && target_name[0] ? target_name : "BLE keyboard");
    }
    return count;
}

bool forget_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type) {
    if (!address || !wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    if (connected_to_locked(address, address_type)) disconnect_locked();
    forget_matching_bond(address, address_type);
    const bool result = bluetooth::forget_device(address, address_type);
    cyw43_arch_lwip_end();
    return result;
}

void cancel_connect() {
    if (!wireless::initialized()) return;
    cyw43_arch_lwip_begin();
    if (state == State::Connecting) {
        state = State::CancellingConnect;
        gap_connect_cancel();
        set_status("CONNECT CANCELLED");
        set_error("BLE CONNECT CANCELLED");
    } else if (is_connect_active()) {
        fail_connection("BLE CONNECT CANCELLED");
    }
    cyw43_arch_lwip_end();
}

bool connect_active() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = is_connect_active() ||
        state == State::Disconnecting || state == State::CancellingConnect;
    cyw43_arch_lwip_end();
    return value;
}

bool pairing_active() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool value = state == State::Pairing;
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
    const bool value = state == State::Ready;
    cyw43_arch_lwip_end();
    return value;
}

bool disconnect() {
    if (!wireless::initialized()) return false;
    cyw43_arch_lwip_begin();
    const bool had_connection =
        connection_handle != HCI_CON_HANDLE_INVALID ||
        state == State::Connecting;
    disconnect_locked();
    if (had_connection) set_error("BLE KEYBOARD DISCONNECT REQUESTED");
    cyw43_arch_lwip_end();
    return had_connection;
}

const char* connected_name() {
    if (!wireless::initialized()) return "";
    cyw43_arch_lwip_begin();
    std::snprintf(name_snapshot, sizeof(name_snapshot), "%s", target_name);
    cyw43_arch_lwip_end();
    return name_snapshot;
}

const char* status() {
    if (!wireless::initialized()) return "OFF";
    cyw43_arch_lwip_begin();
    std::snprintf(
        status_snapshot,
        sizeof(status_snapshot),
        "%s",
        status_text
    );
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

void set_layout(bluetooth_hid::KeyboardLayout value) {
    if (!wireless::initialized()) {
        keyboard.set_layout(value);
        return;
    }
    cyw43_arch_lwip_begin();
    keyboard.set_layout(value);
    cyw43_arch_lwip_end();
}

bluetooth_hid::KeyboardLayout layout() {
    if (!wireless::initialized()) return keyboard.layout();
    cyw43_arch_lwip_begin();
    const auto value = keyboard.layout();
    cyw43_arch_lwip_end();
    return value;
}

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

} // namespace rmb::bluetooth_hid_ble
