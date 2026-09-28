#!/usr/bin/env python3
"""Compile production BLE policy/recovery functions with deterministic radio mocks.

This is a host regression harness, not an over-the-air Bluetooth emulator.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/platform/picocalc/bluetooth_hid_ble_keyboard.cpp").read_text()


def function(name):
    # Functions selected below contain balanced braces, including comments.
    import re
    match = re.search(r"^(?:static )?(?:void|bool|int|std::size_t) " + name + r"\(", SOURCE, re.M)
    assert match, name
    opening = SOURCE.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end]


PRELUDE = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
constexpr std::size_t kMaxDiscoveredDevices = 64;
struct DiscoveredDevice {
    std::uint8_t address[6]{}, address_type{};
    char name[40]{};
    std::int8_t rssi{};
    bool hid_hint{};
};
enum class State { Off, Idle, Scanning, Connecting, CancellingConnect, Pairing,
    DiscoveringReports, DiscoveringService, DiscoveringCharacteristics, EnablingNotifications,
    SwitchingProtocol, Ready, Disconnecting };
State state = State::Idle;
bool bluetooth_enabled = true;
DiscoveredDevice discovered[64]{};
std::size_t discovered_count = 0;
bool discovery_full = false, seen_in_scan[64]{}, displayed_code_valid = false;
using bd_addr_t = std::uint8_t[6];
using hci_con_handle_t = std::uint16_t;
constexpr auto HCI_CON_HANDLE_INVALID = 0xffff;
hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
constexpr int HCI_STATE_WORKING = 2, ERROR_CODE_SUCCESS = 0;
constexpr int HCI_EVENT_PACKET = 4, GAP_EVENT_ADVERTISING_REPORT = 10;
constexpr int HCI_EVENT_META_GAP = 11, GAP_SUBEVENT_LE_CONNECTION_COMPLETE = 12;
constexpr int HCI_EVENT_DISCONNECTION_COMPLETE = 13;
constexpr int ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE = 0x1812;
constexpr int IO_CAPABILITY_DISPLAY_ONLY = 0;
constexpr int SM_AUTHREQ_BONDING = 1, SM_AUTHREQ_MITM_PROTECTION = 4;
constexpr int SM_AUTHREQ_SECURE_CONNECTION = 8;
bool sc_only = true; // Exact default in Pico SDK 2.3.1 BTstack sm_init().
int auth = 8, io = -1, scans = 0, disconnects = 0, cancels = 0, pairings = 0;
bool duplicates = true, manager_available = true, reconnect_available = true;
bool classic_busy = false, classic_scanning = false;
char error_text[96]{}, status_text[96]{};
int gatt_request = 0;
struct { void reset() {} } keyboard;
void set_error(const char* s) { std::snprintf(error_text, sizeof(error_text), "%s", s); }
void set_status(const char* s) { std::snprintf(status_text, sizeof(status_text), "%s", s); }
void sm_set_secure_connections_only_mode(bool v) { sc_only = v; }
void sm_set_io_capabilities(int v) { io = v; }
void sm_set_authentication_requirements(int v) { auth = v; }
namespace wireless { bool initialized() { return true; } }
namespace bluetooth_manager {
bool keyboard_pairing_available_locked() { return manager_available; }
bool keyboard_reconnect_available_locked() { return reconnect_available; }
}
namespace bluetooth_hid {
bool busy_locked() { return classic_busy; }
bool scanning_locked() { return classic_scanning; }
}
void cyw43_arch_lwip_begin() {}
void cyw43_arch_lwip_end() {}
int hci_get_state() { return HCI_STATE_WORKING; }
void gap_set_scan_duplicate_filter(bool v) { duplicates = v; }
void gap_set_scan_parameters(int, int, int) {}
void gap_start_scan() { ++scans; }
void gap_stop_scan() {}
void gap_disconnect(hci_con_handle_t) { ++disconnects; }
void gap_connect_cancel() { ++cancels; }
void sm_request_pairing(hci_con_handle_t) { ++pairings; }
void gatt_client_remove_gatt_query(int*, hci_con_handle_t) {}
void clear_gatt_state() {}
void reset_connection_state() {
    connection_handle = HCI_CON_HANDLE_INVALID;
    displayed_code_valid = false;
}
void gap_event_advertising_report_get_address(std::uint8_t* p, std::uint8_t* a) {
    std::memcpy(a, p + 1, 6);
}
std::uint8_t gap_event_advertising_report_get_address_type(std::uint8_t* p) { return p[7]; }
std::uint8_t gap_event_advertising_report_get_data_length(std::uint8_t*) { return 2; }
const std::uint8_t* gap_event_advertising_report_get_data(std::uint8_t* p) { return p + 8; }
std::int8_t gap_event_advertising_report_get_rssi(std::uint8_t*) { return -40; }
bool ad_data_contains_uuid16(int, const std::uint8_t* p, int) { return p[0] == 1; }
bool advertisement_has_keyboard_appearance(const std::uint8_t* p, int) { return p[0] == 2; }
void extract_name(const std::uint8_t* p, int, char* out, std::size_t n) {
    if (p[1]) std::snprintf(out, n, "Keyboard");
}
std::uint8_t hci_event_packet_get_type(std::uint8_t* p) { return p[0]; }
std::uint8_t hci_event_gap_meta_get_subevent_code(std::uint8_t* p) { return p[1]; }
std::uint8_t gap_subevent_le_connection_complete_get_status(std::uint8_t* p) { return p[2]; }
hci_con_handle_t gap_subevent_le_connection_complete_get_connection_handle(std::uint8_t* p) { return p[3]; }
hci_con_handle_t hci_event_disconnection_complete_get_connection_handle(std::uint8_t* p) { return p[3]; }
"""

FUNCTIONS = [
    "same_address", "configure_security", "prepare_discovery_session",
    "find_discovered", "handle_advertisement", "fail_connection",
    "hci_packet_handler", "is_connect_active", "busy_locked", "start_scan",
    "cancel_scan", "scanning", "discovered_devices", "connect_active",
    "manager_allows_keyboard_request",
    "cancel_connect",
]

TEST = r"""
void advertise(int address, int hint, int name = 0, int type = 0) {
    std::uint8_t p[10]{};
    p[1] = address; p[7] = type; p[8] = hint; p[9] = name;
    handle_advertisement(p);
}
void event(int kind, int status, int handle) {
    std::uint8_t p[4] = {static_cast<std::uint8_t>(kind),
        GAP_SUBEVENT_LE_CONNECTION_COMPLETE, static_cast<std::uint8_t>(status),
        static_cast<std::uint8_t>(handle)};
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
int main() {
    configure_security();
    assert(!sc_only && auth == 13 && io == IO_CAPABILITY_DISPLAY_ONLY);
    // Both peer capabilities are negotiable; local policy no longer rejects legacy.
    for (bool peer_sc : {false, true}) {
        bool negotiated_sc = (auth & SM_AUTHREQ_SECURE_CONNECTION) && peer_sc;
        assert(!(sc_only && !negotiated_sc));
    }
    DiscoveredDevice output[64]{};
    manager_available = false; reconnect_available = true;
    assert(!manager_allows_keyboard_request(true));
    assert(manager_allows_keyboard_request(false));
    reconnect_available = false;
    assert(!manager_allows_keyboard_request(false));
    manager_available = reconnect_available = true;
    classic_busy = classic_scanning = true;
    assert(start_scan()); // Unified UI may run both discovery transports.
    cancel_scan();
    classic_scanning = false;
    assert(!start_scan()); // A Classic connection/connect attempt remains exclusive.
    classic_busy = false;
    assert(start_scan() && !duplicates);
    advertise(1, 1); // HID advertising packet, followed by name-only scan response.
    advertise(1, 0, 1);
    advertise(2, 0, 1); // Same name is NOT evidence that another address is HID.
    assert(discovered_devices(output, 64) == 1);
    assert(std::strcmp(output[0].name, "Keyboard") == 0);
    cancel_scan();
    assert(start_scan());
    assert(discovered_devices(output, 64) == 0); // No stale row.
    advertise(1, 0);
    assert(discovered_devices(output, 64) == 1); // Fresh packet, cached classification.
    advertise(1, 0, 1, 1); // Different address type must not inherit classification.
    assert(discovered_devices(output, 64) == 1);
    // No radio packet at all remains invisible, matching a stopped advertiser.
    cancel_scan(); assert(start_scan());
    assert(discovered_devices(output, 64) == 0);
    discovered_count = 0;
    for (int i = 1; i <= 64; ++i) advertise(i, 0);
    advertise(65, 1, 1); // Unrelated cache entries cannot hide a keyboard.
    assert(discovered_devices(output, 64) == 1);
    for (int i = 1; i <= 63; ++i) advertise(i, 2);
    assert(discovered_devices(output, 64) == 64);
    advertise(66, 1);
    assert(discovery_full && discovered_devices(output, 64) == 64);
    cancel_scan();
    state = State::Pairing; connection_handle = 42;
    fail_connection("AUTH REQUIREMENTS NOT MET (SMP 0x03)");
    assert(state == State::Disconnecting && connect_active() && busy_locked());
    int previous_scans = scans;
    assert(!start_scan() && scans == previous_scans);
    // Restore diagnostic after deliberately testing an invalid API call.
    set_error("AUTH REQUIREMENTS NOT MET (SMP 0x03)");
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0, 41);
    assert(state == State::Disconnecting); // Ignore another link.
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0, 42);
    assert(state == State::Idle && !connect_active() && !busy_locked());
    assert(std::strstr(error_text, "0x03"));
    assert(start_scan()); cancel_scan();
    state = State::Connecting;
    cancel_connect();
    assert(state == State::CancellingConnect && connect_active() && busy_locked());
    assert(!start_scan());
    event(HCI_EVENT_META_GAP, 0, 43); // Successful connect races our cancel.
    assert(state == State::Disconnecting && pairings == 0);
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0, 43);
    assert(state == State::Idle && !busy_locked());
    state = State::Connecting;
    cancel_connect();
    event(HCI_EVENT_META_GAP, 2, 0); // Controller acknowledges cancelled connect.
    assert(state == State::Idle && !busy_locked());
    state = State::Connecting;
    event(HCI_EVENT_META_GAP, 0x3e, 0); // Normal connect failure is already terminal.
    assert(state == State::Idle && !busy_locked());
    assert(std::strstr(error_text, "0x3E"));
    state = State::Ready; connection_handle = 54;
    event(HCI_EVENT_DISCONNECTION_COMPLETE, 0, 54);
    assert(state == State::Idle && !busy_locked());
    assert(std::strstr(error_text, "DISCONNECTED"));
    std::puts("BLE security/discovery/recovery regression tests: PASS");
}
"""

# Guard both call sites against a regression back to independent AuthReq edits.
assert "sm_init();\n    configure_security();" in function("stack_init_locked")
assert "configure_security();" in function("connect_keyboard_impl")
assert "if (fresh_pairing) forget_matching_bond" in function("connect_keyboard_impl")
assert "name, false" in function("connect_paired_keyboard")
assert "manager_allows_keyboard_request(fresh_pairing)" in function("connect_keyboard_impl")
assert "state != State::Pairing" in function("sm_packet_handler")

with tempfile.TemporaryDirectory(prefix="cpb-ble-test-") as tmp:
    source = Path(tmp) / "test.cpp"
    binary = Path(tmp) / "test"
    source.write_text("#include <initializer_list>\n" + PRELUDE + "\n" +
                      "\n".join(function(name) for name in FUNCTIONS) + "\n" + TEST)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
