#include "network.hpp"
#include "file_server.hpp"
#include "wireless.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

namespace rmb::network {

namespace {

constexpr int kMaxScanResults = 24;
constexpr uint16_t kNtpPort = 123;
constexpr int kNtpPacketSize = 48;
constexpr uint32_t kNtpEpochDelta = 2208988800u;

bool wifi_initialized = false;
char wifi_error[96] = "NOT INITIALIZED";
char wifi_ssid[33] = {};

AccessPoint scan_results[kMaxScanResults];
int scan_count = 0;
OperationState state = OperationState::Idle;
enum class OperationKind { None, Scan, Connect, Ntp };
OperationKind kind = OperationKind::None;
std::uint32_t generation = 0;
std::uint32_t scan_generation = 0;
std::uint32_t ntp_generation = 0;
absolute_time_t deadline;
OperationState after_settle = OperationState::Idle;
const char* after_settle_message = "DISCONNECTED";
bool radio_blocked = false;
bool leave_failed = false;
// Only a deferred replacement connection owns these bounded credentials.
char pending_ssid[33] = {};
char pending_password[64] = {};

void clear_pending_association() {
    pending_ssid[0] = '\0';
    std::memset(pending_password, 0, sizeof(pending_password));
}
int ntp_timezone = 540;
NetworkDateTime ntp_value;
struct LwipGuard {
    bool locked = wireless::initialized();
    LwipGuard() { if (locked) cyw43_arch_lwip_begin(); }
    ~LwipGuard() { if (locked) cyw43_arch_lwip_end(); }
};
std::uint32_t next_generation() {
    if (++generation == 0) ++generation;
    return generation;
}
void* cookie(std::uint32_t value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}
bool matches(void* arg, std::uint32_t value) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arg)) == value;
}

struct NtpState {
    struct udp_pcb* pcb = nullptr;
    ip_addr_t server_addr = {};
    volatile bool complete = false;
    volatile bool success = false;
    volatile uint32_t seconds_1900 = 0;
};

NtpState ntp;

void set_error(const char* text) {
    std::snprintf(
        wifi_error,
        sizeof(wifi_error),
        "%s",
        text ? text : "ERROR"
    );
}

int scan_callback(void* arg, const cyw43_ev_scan_result_t* result) {
    if (state != OperationState::Scanning || !matches(arg, scan_generation)) return 0;
    if (!result || result->ssid_len == 0) return 0;

    char ssid[33] = {};
    std::size_t length = result->ssid_len;
    if (length > 32) length = 32;
    std::memcpy(ssid, result->ssid, length);
    ssid[length] = '\0';

    const bool secure = result->auth_mode != CYW43_AUTH_OPEN;
    for (int i = 0; i < scan_count; ++i) {
        if (std::strcmp(scan_results[i].ssid, ssid) == 0) {
            scan_results[i].rssi = std::max(
                scan_results[i].rssi,
                static_cast<int>(result->rssi)
            );
            // If any observation reports security, never present the SSID as open.
            scan_results[i].secure = scan_results[i].secure || secure;
            return 0;
        }
    }

    int index = scan_count;
    if (scan_count < kMaxScanResults) {
        ++scan_count;
    } else {
        index = 0;
        for (int i = 1; i < kMaxScanResults; ++i)
            if (scan_results[i].rssi < scan_results[index].rssi) index = i;
        if (result->rssi <= scan_results[index].rssi) return 0;
    }
    std::snprintf(
        scan_results[index].ssid,
        sizeof(scan_results[index].ssid),
        "%s",
        ssid
    );
    scan_results[index].rssi = result->rssi;
    scan_results[index].secure = secure;
    return 0;
}

bool start_scan_cycle_locked() {
    cyw43_wifi_scan_options_t options = {};
    const int rc = cyw43_wifi_scan(
        &cyw43_state,
        &options,
        cookie(scan_generation),
        scan_callback
    );
    if (rc == 0) return true;

    scan_generation = 0;
    state = OperationState::Failed;
    set_error("WIFI SCAN START FAILED");
    return false;
}

void ntp_send_locked() {
    if (!ntp.pcb || ip_addr_isany(&ntp.server_addr)) return;

    struct pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, kNtpPacketSize, PBUF_RAM);
    if (!packet) {
        ntp.complete = true;
        ntp.success = false;
        return;
    }

    std::memset(packet->payload, 0, kNtpPacketSize);
    static_cast<uint8_t*>(packet->payload)[0] = 0x1b;
    udp_sendto(ntp.pcb, packet, &ntp.server_addr, kNtpPort);
    pbuf_free(packet);
}

void ntp_dns_callback(
    const char*,
    const ip_addr_t* address,
    void* arg
) {
    if (state != OperationState::SyncingTime || !matches(arg, ntp_generation)) return;
    if (!address) {
        ntp.complete = true;
        ntp.success = false;
        return;
    }

    ip_addr_copy(ntp.server_addr, *address);
    ntp_send_locked();
}

void ntp_receive_callback(
    void* arg,
    struct udp_pcb*,
    struct pbuf* packet,
    const ip_addr_t* address,
    u16_t port
) {
    if (!packet) return;
    if (state != OperationState::SyncingTime || !matches(arg, ntp_generation)) {
        pbuf_free(packet);
        return;
    }

    bool valid =
        port == kNtpPort &&
        packet->tot_len >= kNtpPacketSize &&
        ip_addr_cmp(address, &ntp.server_addr);

    if (valid) {
        const uint8_t mode = pbuf_get_at(packet, 0) & 0x07u;
        const uint8_t stratum = pbuf_get_at(packet, 1);
        valid = mode == 4 && stratum != 0;
    }

    if (valid) {
        uint8_t bytes[4] = {};
        pbuf_copy_partial(packet, bytes, sizeof(bytes), 40);
        ntp.seconds_1900 =
            (static_cast<uint32_t>(bytes[0]) << 24) |
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) |
            static_cast<uint32_t>(bytes[3]);
        ntp.success = ntp.seconds_1900 > kNtpEpochDelta;
    }

    if (valid) ntp.complete = true;
    pbuf_free(packet);
}

void unix_to_datetime(
    int64_t unix_seconds,
    NetworkDateTime& value
) {
    int64_t days = unix_seconds / 86400;
    int seconds_of_day = static_cast<int>(unix_seconds % 86400);
    if (seconds_of_day < 0) {
        seconds_of_day += 86400;
        --days;
    }

    value.hour = seconds_of_day / 3600;
    value.minute = (seconds_of_day % 3600) / 60;
    value.second = seconds_of_day % 60;

    int64_t z = days + 719468;
    const int64_t era =
        (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe =
        static_cast<unsigned>(z - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy =
        doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;

    value.year = year;
    value.month = static_cast<int>(month);
    value.day = static_cast<int>(day);
}

// A new connection must not inherit an old lease, IP or DHCP retry timer.
// These are public lwIP operations on the station netif; the CYW43 driver
// (also shared with Bluetooth/LED) is never deinitialized or patched.
void clear_station_ip_locked() {
    auto* interface = &cyw43_state.netif[CYW43_ITF_STA];
    dhcp_stop(interface);
    const ip4_addr_t zero = {};
    netif_set_addr(interface, &zero, &zero, &zero);
}

} // namespace

bool link_down_locked();
void begin_settle_locked(OperationState result, const char* message);

bool init() {
    if (wifi_initialized) return true;

    if (!wireless::init()) {
        set_error(wireless::last_error());
        return false;
    }

    LwipGuard guard;
    cyw43_arch_enable_sta_mode();
    clear_station_ip_locked();
    wifi_initialized = true;
    state = OperationState::Idle;
    kind = OperationKind::None;
    clear_pending_association();
    // STA teardown does not acknowledge the asynchronous leave. Preserve its
    // ownership across OFF/ON so Enable Wi-Fi cannot race the old DISASSOC.
    if (radio_blocked && !leave_failed && !link_down_locked()) {
        after_settle = OperationState::Idle;
        after_settle_message = "READY";
        state = OperationState::Disassociating;
        deadline = make_timeout_time_ms(kRadioSettleTimeoutMs);
        set_error("WAITING FOR WIFI RADIO");
        return true;
    }
    if (leave_failed || !link_down_locked()) {
        begin_settle_locked(OperationState::Idle, "READY");
        return true;
    }
    radio_blocked = false;
    set_error("READY");
    return true;
}

void ntp_cleanup_locked() {
    if (ntp.pcb) { udp_recv(ntp.pcb, nullptr, nullptr); udp_remove(ntp.pcb); ntp.pcb = nullptr; }
    ntp_generation = 0; // Outstanding DNS callbacks retain the old integer cookie.
}

// DOWN is reported after CYW43_EV_DISASSOC clears the join state and
// lowers the lwIP link. A negative auth status is NOT proof of disassociation.
bool link_down_locked() {
    return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_DOWN &&
           cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_DOWN;
}

void begin_settle_locked(OperationState result, const char* message) {
    clear_station_ip_locked();
    after_settle = result;
    after_settle_message = message;
    wifi_ssid[0] = '\0';
    radio_blocked = true;
    state = OperationState::Disassociating;
    deadline = make_timeout_time_ms(kRadioSettleTimeoutMs);
    leave_failed = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0;
    if (leave_failed) {
        clear_pending_association();
        state = OperationState::Failed;
        set_error("WIFI RADIO NOT READY");
    } else {
        set_error("WAITING FOR WIFI RADIO");
    }
}

bool radio_ready() {
    LwipGuard guard;
    return wifi_initialized && !radio_blocked && state != OperationState::Disassociating &&
        (wifi_ssid[0] || link_down_locked());
}

bool operation_active() {
    LwipGuard guard;
    return state == OperationState::Scanning ||
           state == OperationState::DrainingScan ||
           state == OperationState::Disassociating ||
           state == OperationState::Associating || state == OperationState::WaitingForIp ||
           state == OperationState::SyncingTime;
}
OperationState operation_state() { LwipGuard guard; return state; }

void operation_cancel() {
    const auto current = operation_state();
    if (current == OperationState::Associating ||
        current == OperationState::WaitingForIp) {
        // file_server_stop() takes the lwIP lock itself.
        file_server_stop();
    }
    LwipGuard guard;
    if (state == OperationState::Scanning || state == OperationState::DrainingScan) {
        state = OperationState::Cancelled;
        scan_generation = 0;
        scan_count = 0;
        set_error("SCAN CANCELLED");
        // No supported scan-abort API: callbacks are ignored until physical drain.
    } else if (state == OperationState::Associating || state == OperationState::WaitingForIp) {
        const bool dhcp = state == OperationState::WaitingForIp;
        clear_pending_association();
        begin_settle_locked(OperationState::Cancelled,
            dhcp ? "DHCP CANCELLED" : "CONNECT CANCELLED");
    } else if (state == OperationState::Disassociating) {
        // Keep the original deadline and single leave request. Cleanup continues
        // in the background even when ESC returns the user to the menu.
        after_settle = OperationState::Cancelled;
        after_settle_message = "WIFI OPERATION CANCELLED";
        clear_pending_association();
    } else if (state == OperationState::SyncingTime) {
        ntp_cleanup_locked();
        state = OperationState::Cancelled;
        set_error("NTP CANCELLED");
    }
}

void shutdown() {
    if (!wifi_initialized) { set_error("OFF"); return; }
    file_server_stop();
    {
        LwipGuard guard;
        ntp_cleanup_locked();
        scan_generation = 0;
        scan_count = 0;
        // Request leave once, then remove only the STA netif. The arch disable
        // wrapper would send another leave during Disassociating and neither
        // API waits for its completion. Keep the readiness latch for init().
        if (state != OperationState::Disassociating || leave_failed) {
            if (!link_down_locked() || leave_failed)
                begin_settle_locked(OperationState::Idle, "READY");
        }
        cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, false, 0);
        wifi_ssid[0] = '\0';
        clear_pending_association();
        state = OperationState::Idle;
        kind = OperationKind::None;
        wifi_initialized = false;
        set_error("OFF");
    }
}
bool initialized() { return wifi_initialized; }

bool can_start() {
    if (!radio_ready() && wifi_initialized) { set_error("WIFI RADIO BUSY"); return false; }
    if (operation_active()) { set_error("NETWORK BUSY"); return false; }
    if (!wifi_initialized) { set_error("WIFI DISABLED"); return false; }
    LwipGuard guard;
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        set_error("SCAN BUSY - TRY AGAIN"); return false;
    }
    return true;
}

bool scan_start() {
    if (!can_start()) return false;
    LwipGuard guard;
    scan_count = 0;
    scan_generation = next_generation();
    kind = OperationKind::Scan;
    state = OperationState::Scanning;
    deadline = make_timeout_time_ms(kScanObservationMs);
    if (!start_scan_cycle_locked()) return false;
    set_error("SCANNING");
    return true;
}
int scan_result(AccessPoint* results, int max_results) {
    LwipGuard guard;
    if (kind != OperationKind::Scan || state != OperationState::Succeeded ||
        !results || max_results <= 0) return -1;
    const int n = std::min(scan_count, max_results);
    std::copy_n(scan_results, n, results);
    return n;
}
int scan_observed_count() {
    LwipGuard guard;
    return kind == OperationKind::Scan ? scan_count : 0;
}

// Called with the lwIP guard held, only after radio readiness is established.
static bool join_locked(const char* ssid, const char* password) {
    clear_station_ip_locked();
    // Start a fresh negotiation so a lease from another AP cannot masquerade
    // as CONNECTED. Every join, including Add, completes the normal IP flow.
    if (dhcp_start(&cyw43_state.netif[CYW43_ITF_STA]) != ERR_OK) {
        state = OperationState::Failed;
        set_error("WIFI IP START FAILED");
        return false;
    }
    const int rc = cyw43_arch_wifi_connect_async(ssid, password,
        *password ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    if (rc != 0) {
        begin_settle_locked(OperationState::Failed, "WIFI CONNECT START FAILED");
        return false;
    }
    std::snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    state = OperationState::Associating;
    deadline = make_timeout_time_ms(20000);
    set_error("CONNECTING");
    return true;
}

bool connect_start(const char* ssid, const char* password) {
    if (!can_start()) return false;
    if (!ssid || !*ssid || std::strlen(ssid) > 32 ||
        (password && std::strlen(password) > 63)) {
        set_error("INVALID WIFI PROFILE"); return false;
    }
    file_server_stop(); // Complete Stage 4 rollback before altering the STA link.
    LwipGuard guard;
    kind = OperationKind::Connect;
    const char* psk = password ? password : "";
    if (!link_down_locked()) {
        std::snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
        std::snprintf(pending_password, sizeof(pending_password), "%s", psk);
        begin_settle_locked(OperationState::Associating, "");
        return state == OperationState::Disassociating;
    }
    return join_locked(ssid, psk);
}

void disconnect() {
    file_server_stop();
    if (!wifi_initialized) return;
    LwipGuard guard;
    ntp_cleanup_locked();
    scan_generation = 0;
    scan_count = 0;
    clear_pending_association();
    kind = OperationKind::None;
    if (state == OperationState::Disassociating) {
        after_settle = OperationState::Idle;
        after_settle_message = "DISCONNECTED";
    } else if (radio_blocked) {
        // A failed leave/timeout must not be bypassed by Disconnect.
        set_error("WIFI RADIO NOT READY");
    } else if (link_down_locked()) {
        wifi_ssid[0] = '\0';
        state = OperationState::Idle;
        set_error("DISCONNECTED");
    } else {
        begin_settle_locked(OperationState::Idle, "DISCONNECTED");
    }
}

bool connected() {
    if (!wifi_initialized) return false;
    LwipGuard guard;
    return wifi_ssid[0] && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

void operation_poll() {
    if (!wifi_initialized) return;
    LwipGuard guard;
    // A late DISASSOC after timeout can restore readiness, but a failed ioctl
    // requires Wi-Fi OFF/ON: an old DOWN alone does not acknowledge that request.
    if (radio_blocked && !leave_failed && state != OperationState::Disassociating &&
        link_down_locked()) radio_blocked = false;
    // Driver retry events can arrive after a cancelled/failed connection.
    // An unowned association must be left again before the next operation;
    // never treat it as a user connection or overwrite private retry flags.
    if (!radio_blocked && !wifi_ssid[0] && !link_down_locked() &&
        (state == OperationState::Idle || state == OperationState::Succeeded ||
         state == OperationState::Failed || state == OperationState::Cancelled))
        begin_settle_locked(OperationState::Idle, "READY");
    switch (state) {
    case OperationState::Disassociating:
        if (link_down_locked()) {
            radio_blocked = false;
            if (pending_ssid[0]) {
                (void)join_locked(pending_ssid, pending_password);
                clear_pending_association();
            } else {
                state = after_settle;
                set_error(after_settle_message);
            }
        } else if (time_reached(deadline)) {
            clear_pending_association();
            state = OperationState::Failed;
            set_error("WIFI RADIO NOT READY");
        }
        break;
    case OperationState::Scanning:
        if (time_reached(deadline)) {
            scan_generation = 0;
            std::sort(scan_results, scan_results + scan_count,
                [](const AccessPoint& a, const AccessPoint& b) { return a.rssi > b.rssi; });
            // Closing observation does not abort the current firmware scan.
            // Invalidate callbacks now, but retain ownership until it completes
            // so Auto Connect cannot skip every candidate as SCAN BUSY.
            if (cyw43_wifi_scan_active(&cyw43_state)) {
                state = OperationState::DrainingScan;
                deadline = make_timeout_time_ms(kScanDrainTimeoutMs);
                set_error("FINISHING WIFI SCAN");
            } else {
                state = OperationState::Succeeded;
                set_error(scan_count ? "SCAN COMPLETE" : "NO NETWORKS FOUND");
            }
        } else if (!cyw43_wifi_scan_active(&cyw43_state)) {
            // Continue observing for the complete five-second window. A new
            // physical scan starts only after the driver reports the previous
            // cycle complete; every cycle shares this observation generation.
            (void)start_scan_cycle_locked();
        }
        break;
    case OperationState::DrainingScan:
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            state = OperationState::Succeeded;
            set_error(scan_count ? "SCAN COMPLETE" : "NO NETWORKS FOUND");
        } else if (time_reached(deadline)) {
            state = OperationState::Failed;
            set_error("WIFI SCAN FINISH TIMEOUT");
        }
        break;
    case OperationState::Associating:
    case OperationState::WaitingForIp: {
        const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            state = OperationState::Succeeded; set_error("CONNECTED");
        } else if (link < 0 || time_reached(deadline)) {
            const bool dhcp = state == OperationState::WaitingForIp;
            begin_settle_locked(OperationState::Failed,
                link == CYW43_LINK_BADAUTH ? "WIFI PASSWORD INCORRECT" :
                link < 0 ? "WIFI CONNECT FAILED" : dhcp ? "DHCP/IP TIMEOUT" : "WIFI CONNECT TIMEOUT");
        } else if (link == CYW43_LINK_NOIP && state == OperationState::Associating) {
            state = OperationState::WaitingForIp;
            deadline = make_timeout_time_ms(10000); set_error("WAITING FOR IP");
        }
        break;
    }
    case OperationState::SyncingTime:
        if (ntp.complete || time_reached(deadline)) {
            const bool ok = ntp.complete && ntp.success;
            if (ok) unix_to_datetime(static_cast<int64_t>(ntp.seconds_1900) -
                kNtpEpochDelta + static_cast<int64_t>(ntp_timezone) * 60, ntp_value);
            ntp_cleanup_locked();
            state = ok ? OperationState::Succeeded : OperationState::Failed;
            set_error(ok ? "NTP OK" : ntp.complete ? "NTP RESPONSE FAILED" : "NTP TIMEOUT");
        }
        break;
    default: break;
    }
}

bool get_ip(char* output, std::size_t capacity) {
    if (!output || capacity < 16 || !connected()) {
        return false;
    }

    cyw43_arch_lwip_begin();
    struct netif* interface = netif_default;
    if (!interface ||
        ip4_addr_isany_val(*netif_ip4_addr(interface))) {
        cyw43_arch_lwip_end();
        return false;
    }

    std::snprintf(
        output,
        capacity,
        "%s",
        ip4addr_ntoa(netif_ip4_addr(interface))
    );
    cyw43_arch_lwip_end();
    return true;
}

const char* current_ssid() {
    return connected() ? wifi_ssid : "";
}

bool ntp_start(int timezone_minutes, const char* server) {
    if (!can_start()) return false;
    if (!connected()) { set_error("WIFI NOT CONNECTED"); return false; }
    if (!server || !*server) { set_error("NTP SERVER NOT SET"); return false; }
    LwipGuard guard;
    ntp.complete = false; ntp.success = false; ntp.seconds_1900 = 0;
    ip_addr_set_zero(&ntp.server_addr);
    ntp.pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!ntp.pcb) { state = OperationState::Failed; set_error("NTP UDP ALLOC FAILED"); return false; }
    ntp_generation = next_generation();
    ntp_timezone = timezone_minutes;
    kind = OperationKind::Ntp;
    state = OperationState::SyncingTime;
    udp_recv(ntp.pcb, ntp_receive_callback, cookie(ntp_generation));
    const err_t rc = dns_gethostbyname(server, &ntp.server_addr, ntp_dns_callback, cookie(ntp_generation));
    if (rc == ERR_OK) ntp_send_locked();
    else if (rc != ERR_INPROGRESS) {
        ntp_cleanup_locked(); state = OperationState::Failed;
        set_error("NTP DNS FAILED"); return false;
    }
    deadline = make_timeout_time_ms(10000);
    set_error("SYNCING TIME");
    return true;
}
bool ntp_result(NetworkDateTime& value) {
    LwipGuard guard;
    if (kind != OperationKind::Ntp || state != OperationState::Succeeded ||
        !ntp.success) return false;
    value = ntp_value;
    return true;
}

const char* last_error() {
    return wifi_error;
}

} // namespace rmb::network
