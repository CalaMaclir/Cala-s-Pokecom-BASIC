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
volatile int scan_count = 0;

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

int scan_callback(void*, const cyw43_ev_scan_result_t* result) {
    if (!result || result->ssid_len == 0) return 0;

    char ssid[33] = {};
    std::size_t length = result->ssid_len;
    if (length > 32) length = 32;
    std::memcpy(ssid, result->ssid, length);
    ssid[length] = '\0';

    for (int i = 0; i < scan_count; ++i) {
        if (std::strcmp(scan_results[i].ssid, ssid) == 0) {
            if (result->rssi > scan_results[i].rssi) {
                scan_results[i].rssi = result->rssi;
            }
            return 0;
        }
    }

    if (scan_count >= kMaxScanResults) return 0;

    const int index = scan_count++;
    std::snprintf(
        scan_results[index].ssid,
        sizeof(scan_results[index].ssid),
        "%s",
        ssid
    );
    scan_results[index].rssi = result->rssi;
    scan_results[index].secure = result->auth_mode != CYW43_AUTH_OPEN;
    return 0;
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
    void*
) {
    if (!address) {
        ntp.complete = true;
        ntp.success = false;
        return;
    }

    ip_addr_copy(ntp.server_addr, *address);
    ntp_send_locked();
}

void ntp_receive_callback(
    void*,
    struct udp_pcb*,
    struct pbuf* packet,
    const ip_addr_t* address,
    u16_t port
) {
    if (!packet) return;

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

    ntp.complete = true;
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

} // namespace

bool init() {
    if (wifi_initialized) return true;

    if (!wireless::init()) {
        set_error(wireless::last_error());
        return false;
    }

    cyw43_arch_enable_sta_mode();
    wifi_initialized = true;
    set_error("READY");
    return true;
}

bool initialized() {
    return wifi_initialized;
}

int scan(AccessPoint* results, int max_results) {
    if (!results || max_results <= 0) {
        set_error("BAD SCAN BUFFER");
        return -1;
    }
    if (!init()) return -1;

    scan_count = 0;
    for (auto& result : scan_results) {
        result = AccessPoint{};
    }

    cyw43_wifi_scan_options_t options = {};
    const int rc = cyw43_wifi_scan(
        &cyw43_state,
        &options,
        nullptr,
        scan_callback
    );
    if (rc != 0) {
        set_error("WIFI SCAN START FAILED");
        return -1;
    }

    const absolute_time_t deadline = make_timeout_time_ms(10000);
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (time_reached(deadline)) {
            set_error("WIFI SCAN TIMEOUT");
            break;
        }
        sleep_ms(20);
    }

    const int found = scan_count;
    std::sort(
        scan_results,
        scan_results + found,
        [](const AccessPoint& a, const AccessPoint& b) {
            return a.rssi > b.rssi;
        }
    );

    const int count = found < max_results ? found : max_results;
    for (int i = 0; i < count; ++i) {
        results[i] = scan_results[i];
    }

    if (count == 0) {
        set_error("NO NETWORKS FOUND");
    } else {
        set_error("SCAN COMPLETE");
    }
    return count;
}

bool connect(const char* ssid, const char* password) {
    if (!ssid || !*ssid) {
        set_error("SSID NOT SET");
        return false;
    }
    if (!init()) return false;

    if (connected() && std::strcmp(wifi_ssid, ssid) == 0) {
        return true;
    }

    // A server is bound to the current STA interface/address. Tear it down
    // before any reconnect so an in-flight SD transaction is aborted and
    // rolled back before the link changes.
    file_server_stop();

    if (connected()) {
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        sleep_ms(100);
    }

    const char* psk = password ? password : "";
    const uint32_t auth =
        *psk ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;

    const int rc = cyw43_arch_wifi_connect_timeout_ms(
        ssid,
        psk,
        auth,
        20000
    );

    if (rc != 0) {
        if (rc == PICO_ERROR_TIMEOUT) {
            set_error("WIFI CONNECT TIMEOUT");
        } else {
            set_error("WIFI CONNECT FAILED");
        }
        return false;
    }

    // Association can complete before DHCP has assigned an IPv4 address.
    // NTP/DNS started in that gap can fail even though the Wi-Fi link itself
    // is already up. Wait briefly for a usable address before reporting the
    // connection ready to higher layers.
    const absolute_time_t dhcp_deadline = make_timeout_time_ms(10000);
    bool have_ip = false;
    while (!time_reached(dhcp_deadline)) {
        cyw43_arch_lwip_begin();
        struct netif* interface = netif_default;
        have_ip =
            interface &&
            !ip4_addr_isany_val(*netif_ip4_addr(interface));
        cyw43_arch_lwip_end();

        if (have_ip) break;
        sleep_ms(50);
    }

    if (!have_ip) {
        set_error("DHCP/IP TIMEOUT");
        return false;
    }

    std::snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    set_error("CONNECTED");
    return true;
}

void disconnect() {
    file_server_stop();
    if (!wifi_initialized) return;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    wifi_ssid[0] = '\0';
    set_error("DISCONNECTED");
}

bool connected() {
    if (!wifi_initialized) return false;
    return
        cyw43_tcpip_link_status(
            &cyw43_state,
            CYW43_ITF_STA
        ) == CYW43_LINK_UP;
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
    return wifi_ssid;
}

bool ntp_time(
    NetworkDateTime& value,
    int timezone_minutes,
    const char* server
) {
    if (!connected()) {
        set_error("WIFI NOT CONNECTED");
        return false;
    }

    if (!server || !*server) {
        set_error("NTP SERVER NOT SET");
        return false;
    }

    ntp.complete = false;
    ntp.success = false;
    ntp.seconds_1900 = 0;
    ip_addr_set_zero(&ntp.server_addr);

    cyw43_arch_lwip_begin();
    ntp.pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!ntp.pcb) {
        cyw43_arch_lwip_end();
        set_error("NTP UDP ALLOC FAILED");
        return false;
    }

    udp_recv(ntp.pcb, ntp_receive_callback, nullptr);

    const err_t dns_result = dns_gethostbyname(
        server,
        &ntp.server_addr,
        ntp_dns_callback,
        nullptr
    );

    if (dns_result == ERR_OK) {
        ntp_send_locked();
    } else if (dns_result != ERR_INPROGRESS) {
        udp_remove(ntp.pcb);
        ntp.pcb = nullptr;
        cyw43_arch_lwip_end();
        std::snprintf(
            wifi_error,
            sizeof(wifi_error),
            "NTP DNS FAILED: %.64s",
            server
        );
        return false;
    }
    cyw43_arch_lwip_end();

    const absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!ntp.complete && !time_reached(deadline)) {
        sleep_ms(20);
    }

    cyw43_arch_lwip_begin();
    if (ntp.pcb) {
        udp_remove(ntp.pcb);
        ntp.pcb = nullptr;
    }
    cyw43_arch_lwip_end();

    if (!ntp.complete || !ntp.success) {
        std::snprintf(
            wifi_error,
            sizeof(wifi_error),
            ntp.complete
                ? "NTP RESPONSE FAILED: %.58s"
                : "NTP TIMEOUT: %.67s",
            server
        );
        return false;
    }

    const int64_t unix_seconds =
        static_cast<int64_t>(ntp.seconds_1900) -
        static_cast<int64_t>(kNtpEpochDelta) +
        static_cast<int64_t>(timezone_minutes) * 60;

    unix_to_datetime(unix_seconds, value);
    std::snprintf(
        wifi_error,
        sizeof(wifi_error),
        "NTP OK: %.72s",
        server
    );
    return true;
}

const char* last_error() {
    return wifi_error;
}

} // namespace rmb::network
