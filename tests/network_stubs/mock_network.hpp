#pragma once
// Public SDK/lwIP surface only. Tests execute production network.cpp unchanged.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
using absolute_time_t = std::uint64_t;
using u16_t = std::uint16_t;
using err_t = int;
constexpr int ERR_OK = 0, ERR_INPROGRESS = -5;
constexpr int IPADDR_TYPE_ANY = 0, PBUF_TRANSPORT = 0, PBUF_RAM = 0;
constexpr int CYW43_ITF_STA = 0;
constexpr int CYW43_LINK_DOWN = 0, CYW43_LINK_JOIN = 1, CYW43_LINK_NOIP = 2, CYW43_LINK_UP = 3;
constexpr int CYW43_LINK_FAIL = -1, CYW43_LINK_NONET = -2, CYW43_LINK_BADAUTH = -3;
constexpr int CYW43_AUTH_OPEN = 0, CYW43_AUTH_WPA2_AES_PSK = 1;
struct ip_addr_t { std::uint32_t addr = 0; };
using ip4_addr_t = ip_addr_t;
struct netif { ip4_addr_t ip; };
struct cyw43_t { struct netif netif[1]; };
inline cyw43_t cyw43_state;
struct cyw43_wifi_scan_options_t {};
struct cyw43_ev_scan_result_t { unsigned ssid_len; char ssid[32]; int rssi; int auth_mode; };
struct pbuf { void* payload; unsigned tot_len; };
using udp_callback = void(*)(void*, struct udp_pcb*, pbuf*, const ip_addr_t*, u16_t);
struct udp_pcb { udp_callback callback = nullptr; void* arg = nullptr; };
using dns_callback = void(*)(const char*, const ip_addr_t*, void*);
namespace mock_network {
inline std::uint64_t now = 0;
inline bool sta = false, scanning = false;
inline int link = 0, joins = 0, leaves = 0, starts = 0, pcb_live = 0, sent = 0, lock_depth = 0;
inline int join_rc = 0, scan_rc = 0, dns_rc = ERR_INPROGRESS;
inline bool auto_scan = false, auto_connect = false;
inline unsigned auto_scan_duration_ms = 0;
inline std::uint64_t scan_complete_at = 0;
inline bool disassociation_pending = false, leave_stuck = false;
inline unsigned leave_delay_ms = 0;
inline int leave_rc = 0, radio_overlap = 0;
inline std::uint64_t leave_complete_at = 0;
inline bool dhcp_running = false;
inline int dhcp_starts = 0, dhcp_stops = 0, dhcp_rc = ERR_OK;
inline std::function<int(const char*, const char*)> join_link;
inline void complete_disassociation() { disassociation_pending = false; link = CYW43_LINK_DOWN; }
inline void service_radio() {
    if (auto_scan && scanning && now >= scan_complete_at) scanning = false;
    if (disassociation_pending && !leave_stuck && now >= leave_complete_at)
        complete_disassociation();
}
inline std::vector<std::string> attempts, events;
inline std::string last_password;
inline std::uint32_t last_auth_mode = CYW43_AUTH_OPEN;
inline int (*scan_callback)(void*, const cyw43_ev_scan_result_t*) = nullptr;
inline void* scan_arg = nullptr;
struct DnsRequest { dns_callback callback; void* arg; };
inline std::vector<DnsRequest> dns_requests;
inline udp_pcb* last_pcb = nullptr;
inline netif& interface = cyw43_state.netif[0];
inline void emit(const char* ssid, int rssi, bool secure = true) {
    cyw43_ev_scan_result_t result{}; result.ssid_len = std::strlen(ssid);
    assert(result.ssid_len <= 32); std::memcpy(result.ssid, ssid, result.ssid_len);
    result.rssi = rssi; result.auth_mode = secure ? 1 : 0;
    if (scan_callback) scan_callback(scan_arg, &result);
}
}
inline netif* netif_default = &mock_network::interface;
inline absolute_time_t make_timeout_time_ms(unsigned ms) { return mock_network::now + ms; }
inline bool time_reached(absolute_time_t t) { return mock_network::now >= t; }
inline void cyw43_arch_lwip_begin() { ++mock_network::lock_depth; }
inline void cyw43_arch_lwip_end() { assert(mock_network::lock_depth > 0); --mock_network::lock_depth; }
inline void cyw43_arch_enable_sta_mode() {
    if (!mock_network::sta) {
        mock_network::interface.ip.addr = 0;
        mock_network::dhcp_running = true; ++mock_network::dhcp_starts;
        mock_network::sta = true;
    }
}
// SDK STA teardown removes the netif/DHCP client. It does not acknowledge
// DISASSOC, stop a physical scan, or clear the driver's join state.
inline void cyw43_wifi_set_up(cyw43_t*, int, bool up, std::uint32_t) {
    assert(!up);
    mock_network::sta = false; mock_network::dhcp_running = false;
    mock_network::interface.ip.addr = 0; ++mock_network::dhcp_stops;
}
inline bool cyw43_wifi_scan_active(cyw43_t*) { mock_network::service_radio(); return mock_network::scanning; }
inline int cyw43_wifi_scan(cyw43_t*, cyw43_wifi_scan_options_t*, void* arg,
    int (*callback)(void*, const cyw43_ev_scan_result_t*)) {
    mock_network::service_radio();
    if (mock_network::disassociation_pending) { ++mock_network::radio_overlap; return -1; }
    if (mock_network::scan_rc) return mock_network::scan_rc;
    ++mock_network::starts; mock_network::scanning = true;
    mock_network::scan_arg = arg; mock_network::scan_callback = callback;
    if (mock_network::auto_scan) {
        mock_network::scan_complete_at = mock_network::now + mock_network::auto_scan_duration_ms;
        mock_network::emit("HOME", -65); mock_network::emit("OFFICE", -40);
        mock_network::emit("MOBILE", -30);
        mock_network::scanning = mock_network::auto_scan_duration_ms != 0;
    }
    return 0;
}
inline int cyw43_arch_wifi_connect_async(const char* ssid, const char* password, std::uint32_t auth_mode) {
    mock_network::service_radio();
    if (mock_network::disassociation_pending) { ++mock_network::radio_overlap; return -1; }
    ++mock_network::joins; mock_network::attempts.emplace_back(ssid);
    mock_network::last_password = password ? password : "";
    mock_network::last_auth_mode = auth_mode;
    mock_network::events.push_back("join");
    if (mock_network::join_link) mock_network::link = mock_network::join_link(ssid, password);
    else mock_network::link = mock_network::auto_connect ?
        (std::strcmp(ssid,"OFFICE") == 0 ? -3 : CYW43_LINK_UP) : CYW43_LINK_JOIN;
    return mock_network::join_rc;
}
inline int cyw43_wifi_leave(cyw43_t*, int) {
    ++mock_network::leaves; mock_network::events.push_back("leave");
    if (mock_network::leave_rc) return mock_network::leave_rc;
    mock_network::disassociation_pending = true;
    mock_network::leave_complete_at = mock_network::now + mock_network::leave_delay_ms;
    mock_network::service_radio();
    return 0;
}
inline void cyw43_arch_disable_sta_mode() {
    cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, false, 0);
    if (mock_network::link != CYW43_LINK_DOWN) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}
inline void dhcp_stop(netif*) { mock_network::dhcp_running = false; ++mock_network::dhcp_stops; }
inline err_t dhcp_start(netif*) {
    ++mock_network::dhcp_starts;
    mock_network::dhcp_running = mock_network::dhcp_rc == ERR_OK;
    return mock_network::dhcp_rc;
}
inline void netif_set_addr(netif* n, const ip4_addr_t* ip, const ip4_addr_t*, const ip4_addr_t*) { n->ip = *ip; }
inline int cyw43_wifi_link_status(cyw43_t*, int) {
    mock_network::service_radio();
    const int link = mock_network::link;
    return link == CYW43_LINK_UP || link == CYW43_LINK_NOIP ? CYW43_LINK_JOIN : link;
}
inline int cyw43_tcpip_link_status(cyw43_t*, int) { mock_network::service_radio(); return mock_network::link; }
inline bool ip_addr_isany(const ip_addr_t* a) { return a->addr == 0; }
inline bool ip_addr_cmp(const ip_addr_t* a, const ip_addr_t* b) { return a->addr == b->addr; }
#define ip_addr_copy(a,b) ((a) = (b))
inline void ip_addr_set_zero(ip_addr_t* a) { a->addr = 0; }
inline const ip4_addr_t* netif_ip4_addr(const netif* n) { return &n->ip; }
inline const char* ip4addr_ntoa(const ip4_addr_t*) { return "192.0.2.1"; }
#define ip4_addr_isany_val(a) ((a).addr == 0)
inline udp_pcb* udp_new_ip_type(int) {
    ++mock_network::pcb_live; return mock_network::last_pcb = new udp_pcb;
}
inline void udp_recv(udp_pcb* pcb, udp_callback cb, void* arg) { pcb->callback = cb; pcb->arg = arg; }
inline void udp_remove(udp_pcb* pcb) {
    --mock_network::pcb_live;
    if (pcb == mock_network::last_pcb) mock_network::last_pcb = nullptr;
    delete pcb;
}
inline pbuf* pbuf_alloc(int, unsigned n, int) { return new pbuf{new unsigned char[n], n}; }
inline void pbuf_free(pbuf* p) { delete[] static_cast<unsigned char*>(p->payload); delete p; }
inline int udp_sendto(udp_pcb*, pbuf*, const ip_addr_t*, unsigned) { ++mock_network::sent; return 0; }
inline unsigned char pbuf_get_at(const pbuf* p, unsigned at) { return static_cast<unsigned char*>(p->payload)[at]; }
inline unsigned pbuf_copy_partial(const pbuf* p, void* out, unsigned size, unsigned at) {
    std::memcpy(out, static_cast<unsigned char*>(p->payload) + at, size); return size;
}
inline int dns_gethostbyname(const char*, ip_addr_t* out, dns_callback cb, void* arg) {
    if (mock_network::dns_rc == ERR_OK) out->addr = 1;
    else if (mock_network::dns_rc == ERR_INPROGRESS) mock_network::dns_requests.push_back({cb,arg});
    return mock_network::dns_rc;
}
