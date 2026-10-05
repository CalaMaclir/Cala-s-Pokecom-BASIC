#include "network.hpp"
#include "wireless.hpp"
#include "mock_network.hpp"
#include <cassert>
#include <cstdio>
namespace rmb::network { void file_server_stop() { mock_network::events.push_back("stop"); } }
using namespace rmb::network;
int main() {
    assert(!scan_start()); assert(init());
    assert(scan_start()); assert(operation_active());
    const auto old_scan = mock_network::scan_callback; const auto old_arg = mock_network::scan_arg;
    mock_network::emit("OLD",-40); operation_cancel();
    assert(operation_state()==OperationState::Cancelled && initialized());
    assert(!scan_start()); assert(std::strstr(last_error(),"SCAN BUSY"));
    mock_network::emit("LATE",-10);mock_network::scanning=false;
    assert(scan_start()); const int first_cycle = mock_network::starts;
    cyw43_ev_scan_result_t stale{};stale.ssid_len=3;std::memcpy(stale.ssid,"OLD",3);stale.rssi=-1;
    old_scan(old_arg,&stale);
    // Cycle A (0.0-2.1 s), B (2.1-4.2 s), and C (4.2-5.0+ s)
    // share one logical observation and never overlap physically.
    mock_network::emit("HOME",-70,false);mock_network::emit("OFFICE",-60);
    mock_network::now=2100;mock_network::scanning=false;operation_poll();operation_poll();
    assert(mock_network::starts==first_cycle+1 && operation_active());
    mock_network::emit("HOME",-50,true);mock_network::emit("MOBILE",-80);
    mock_network::now=4200;mock_network::scanning=false;operation_poll();operation_poll();
    assert(mock_network::starts==first_cycle+2 && operation_active());
    mock_network::emit("LATE",-90);
    mock_network::now=5000;operation_poll();operation_poll();
    assert(operation_state()==OperationState::DrainingScan && operation_active());
    AccessPoint aps[24];assert(scan_result(aps,24)<0);
    const int waiting_joins=mock_network::joins, waiting_starts=mock_network::starts;
    assert(!connect_start("HOME","pw") && !scan_start());
    assert(mock_network::joins==waiting_joins && mock_network::starts==waiting_starts);
    const int completed_count=scan_observed_count();
    mock_network::now=5500;mock_network::emit("OVERRUN",-10);
    assert(scan_observed_count()==completed_count && scan_result(aps,24)<0);
    mock_network::scanning=false;operation_poll();
    assert(operation_state()==OperationState::Succeeded && !operation_active());
    assert(scan_result(aps,24)==4);
    assert(std::strcmp(aps[0].ssid,"HOME")==0 && aps[0].rssi==-50 && aps[0].secure);
    assert(std::strcmp(aps[1].ssid,"OFFICE")==0 && aps[1].rssi==-60);
    // Bounded storage retains 24 entries and replaces only the weakest entry.
    assert(scan_start());
    for(int i=0;i<24;++i) {
        char name[16];std::snprintf(name,sizeof(name),"AP%02d",i);
        mock_network::emit(name,-100+i);
    }
    mock_network::emit("STRONG",-5);mock_network::now=10500;operation_poll();operation_poll();
    assert(operation_state()==OperationState::DrainingScan);
    mock_network::scanning=false;operation_poll();
    assert(scan_result(aps,24)==24 && std::strcmp(aps[0].ssid,"STRONG")==0);
    // A stuck physical scan is bounded and never permits a connection.
    assert(scan_start());mock_network::emit("HOME",-20);
    mock_network::now+=kScanObservationMs;operation_poll();
    mock_network::now+=kScanDrainTimeoutMs;operation_poll();
    assert(operation_state()==OperationState::Failed && !operation_active());
    assert(std::strcmp(last_error(),"WIFI SCAN FINISH TIMEOUT")==0 && scan_result(aps,24)<0);
    assert(!connect_start("HOME","pw") && !scan_start());
    mock_network::scanning=false;
    assert(scan_start());mock_network::now+=kScanObservationMs;operation_poll();
    assert(operation_state()==OperationState::DrainingScan);
    operation_cancel();assert(operation_state()==OperationState::Cancelled && !operation_active());
    assert(scan_result(aps,24)<0 && !scan_start());
    mock_network::scanning=false;
    // NOIP is intermediate: DHCP must finish before success, and a successful
    // join stays connected without any credential-test leave/rejoin sequence.
    const int initial_leaves=mock_network::leaves;
    assert(connect_start("SECURE","secret"));
    assert(operation_state()==OperationState::Associating);
    assert(mock_network::dhcp_running);
    assert(mock_network::last_password=="secret" &&
        mock_network::last_auth_mode==CYW43_AUTH_WPA2_AES_PSK);
    mock_network::link=CYW43_LINK_NOIP;operation_poll();operation_poll();
    assert(operation_state()==OperationState::WaitingForIp && !connected());
    assert(mock_network::leaves==initial_leaves);
    mock_network::link=CYW43_LINK_UP;operation_poll();operation_poll();
    assert(operation_state()==OperationState::Succeeded && connected());
    assert(std::strcmp(current_ssid(),"SECURE")==0 && mock_network::leaves==initial_leaves);
    disconnect();operation_poll();
    assert(connect_start("BAD","wrong"));mock_network::link=-6;operation_poll();operation_poll();
    assert(operation_state()==OperationState::Failed && !connected());
    assert(std::strcmp(last_error(),"WIFI CONNECT FAILED")==0);
    assert(connect_start("SLOW","secret"));mock_network::now+=20000;operation_poll();operation_poll();
    assert(operation_state()==OperationState::Failed && !connected());
    assert(std::strcmp(last_error(),"WIFI CONNECT TIMEOUT")==0);
    assert(connect_start("CANCEL","secret"));operation_cancel();operation_poll();
    assert(operation_state()==OperationState::Cancelled && !connected());
    assert(std::strcmp(last_error(),"CONNECT CANCELLED")==0);
    assert(connect_start("HOME","pw"));assert(!scan_start());assert(!connect_start("OFFICE","pw"));
    operation_cancel();operation_poll();assert(initialized() && !connected() && !*current_ssid());
    assert(std::strcmp(last_error(),"CONNECT CANCELLED")==0);
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_NOIP;operation_poll();operation_poll();
    assert(operation_state()==OperationState::WaitingForIp);
    operation_cancel();operation_poll();assert(!connected() && std::strcmp(last_error(),"DHCP CANCELLED")==0);
    assert(connect_start("HOME","pw"));mock_network::link=-3;operation_poll();operation_poll();
    assert(operation_state()==OperationState::Failed && !connected());
    assert(connect_start("HOME","pw"));mock_network::now+=20000;operation_poll();operation_poll();
    assert(operation_state()==OperationState::Failed && !connected());
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_NOIP;operation_poll();operation_poll();
    mock_network::now+=10000;operation_poll();operation_poll();assert(std::strcmp(last_error(),"DHCP/IP TIMEOUT")==0);
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_UP;operation_poll();operation_poll();assert(connected());
    assert(scan_result(aps,24)<0);
    assert(ntp_start());assert(mock_network::pcb_live==1);
    const auto dns_a=mock_network::dns_requests.back();operation_cancel();
    assert(mock_network::pcb_live==0 && connected() && initialized());
    assert(std::strcmp(last_error(),"NTP CANCELLED")==0);
    assert(ntp_start());const auto dns_b=mock_network::dns_requests.back();
    ip_addr_t addr{1};const auto sends=mock_network::sent;
    dns_a.callback("old",&addr,dns_a.arg);assert(mock_network::sent==sends);
    dns_b.callback("new",&addr,dns_b.arg);assert(mock_network::sent==sends+1);
    auto* pcb=mock_network::last_pcb;auto* packet=pbuf_alloc(0,48,0);
    std::memset(packet->payload,0,48);auto* bytes=static_cast<unsigned char*>(packet->payload);
    bytes[0]=0x24;bytes[1]=1;const std::uint32_t seconds=2208988800u+1704067200u;
    for(int i=0;i<4;++i)bytes[40+i]=seconds>>(24-i*8);
    pcb->callback(pcb->arg,pcb,packet,&addr,123);operation_poll();operation_poll();
    NetworkDateTime time;assert(ntp_result(time));assert(time.year==2024 && time.hour==9);
    assert(mock_network::pcb_live==0 && connected());
    assert(ntp_start()); mock_network::now+=10000;operation_poll();operation_poll();assert(connected());
    assert(operation_state()==OperationState::Failed && mock_network::pcb_live==0);
    assert(ntp_start());shutdown();assert(!initialized() && !connected() && mock_network::pcb_live==0);
    const auto last=mock_network::dns_requests.back();last.callback("late",&addr,last.arg);
    assert(init());mock_network::now=0;assert(scan_start());
    mock_network::now=3000;operation_cancel();
    assert(operation_state()==OperationState::Cancelled && !scan_start());
    mock_network::emit("CANCELLED",-1);mock_network::scanning=false;
    assert(scan_start());operation_cancel();shutdown();
    mock_network::scanning=false; // STA OFF does not abort a physical scan.
    assert(mock_network::lock_depth==0);
    // Delayed driver DISASSOC: no scan/connect while leave is pending.
    assert(init());
    mock_network::leave_delay_ms=150;mock_network::leave_stuck=true;
    for (int cycle=0;cycle<6;++cycle) {
        for (int attempt=0;attempt<2;++attempt) {
            assert(connect_start("HOME","wrong"));
            mock_network::link=CYW43_LINK_BADAUTH;
            operation_poll();
            assert(operation_state()==OperationState::Disassociating && operation_active());
            assert(!radio_ready() && !connected() && !*current_ssid());
            const int scans=mock_network::starts,joins=mock_network::joins,leaves=mock_network::leaves;
            assert(!scan_start() && !connect_start("HOME","pw"));
            assert(mock_network::starts==scans && mock_network::joins==joins && mock_network::leaves==leaves);
            mock_network::complete_disassociation();operation_poll();
            assert(radio_ready() && !operation_active());
            assert(operation_state()==OperationState::Failed);
            assert(std::strcmp(last_error(),"WIFI PASSWORD INCORRECT")==0);
        }
        assert(scan_start());mock_network::emit("HOME",-41);
        mock_network::now+=kScanObservationMs;operation_poll();mock_network::scanning=false;operation_poll();
        assert(scan_result(aps,24)==1 && std::strcmp(aps[0].ssid,"HOME")==0);
        assert(connect_start("HOME","correct"));mock_network::link=CYW43_LINK_UP;operation_poll();
        assert(connected());
        // Replacing a connected profile defers the join until old link leaves.
        const int joins=mock_network::joins;
        assert(connect_start("OTHER","new"));assert(mock_network::joins==joins);
        assert(operation_state()==OperationState::Disassociating);
        mock_network::complete_disassociation();operation_poll();
        assert(mock_network::joins==joins+1 && mock_network::last_password=="new");
        operation_cancel();const int leaves=mock_network::leaves;
        operation_cancel();operation_cancel();assert(mock_network::leaves==leaves);
        mock_network::complete_disassociation();operation_poll();
        assert(operation_state()==OperationState::Cancelled && radio_ready());
    }
    // Cancel a queued replacement without starting it or extending its deadline.
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_UP;operation_poll();
    const int queued_joins=mock_network::joins;
    assert(connect_start("OFFICE","pw"));operation_cancel();
    mock_network::complete_disassociation();operation_poll();
    assert(mock_network::joins==queued_joins && radio_ready());
    // Bounded timeout remains blocked; even Disconnect cannot bypass it.
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_NOIP;operation_poll();
    operation_cancel();mock_network::now+=kRadioSettleTimeoutMs;operation_poll();
    assert(operation_state()==OperationState::Failed && !operation_active() && !radio_ready());
    assert(std::strcmp(last_error(),"WIFI RADIO NOT READY")==0);
    disconnect();assert(!scan_start() && !connect_start("HOME","pw"));
    mock_network::complete_disassociation();operation_poll();assert(radio_ready());
    // Failed leave is not an incorrect password; OFF/ON recovers the latch.
    mock_network::leave_rc=-1;
    assert(connect_start("HOME","pw"));mock_network::link=CYW43_LINK_BADAUTH;operation_poll();
    assert(operation_state()==OperationState::Failed && !radio_ready());
    assert(std::strcmp(last_error(),"WIFI RADIO NOT READY")==0);
    shutdown();assert(!initialized() && !connected() && !operation_active());
    mock_network::leave_rc=0;mock_network::leave_stuck=false;
    assert(init());assert(!radio_ready());
    mock_network::now+=150;operation_poll();assert(radio_ready());
    assert(connect_start("HOME","pw"));operation_cancel();shutdown();
    assert(!initialized() && !connected() && !operation_active());
    // Immediate OFF/ON while connection cleanup is pending must wait for the same
    // leave, not clear the latch or issue another request ahead of scan/join.
    const int off_leaves=mock_network::leaves, off_joins=mock_network::joins;
    assert(init());assert(operation_state()==OperationState::Disassociating);
    assert(!scan_start() && !connect_start("HOME","pw"));
    assert(mock_network::leaves==off_leaves && mock_network::joins==off_joins);
    mock_network::now+=150;operation_poll();assert(radio_ready());
    // A new connection discards old addressing but retains the new lease
    // until the user explicitly disconnects.
    mock_network::interface.ip.addr=123;
    assert(connect_start("HOME","pw"));
    assert(mock_network::dhcp_running && mock_network::interface.ip.addr==0);
    mock_network::link=CYW43_LINK_UP;mock_network::interface.ip.addr=456;
    operation_poll();assert(mock_network::interface.ip.addr==456 && connected());
    disconnect();assert(mock_network::interface.ip.addr==0);
    mock_network::now+=150;operation_poll();assert(radio_ready() && !connected());
    // A late firmware/driver rejoin after disconnect cannot own a new operation.
    mock_network::link=CYW43_LINK_JOIN;assert(!radio_ready() && !scan_start());
    operation_poll();assert(operation_state()==OperationState::Disassociating);
    mock_network::now+=150;operation_poll();assert(radio_ready() && !connected());
    const int dhcp_starts=mock_network::dhcp_starts;
    assert(connect_start("HOME","pw"));
    assert(mock_network::dhcp_running && mock_network::dhcp_starts==dhcp_starts+1);
    mock_network::link=CYW43_LINK_NOIP;operation_poll();
    assert(operation_state()==OperationState::WaitingForIp && !connected());
    mock_network::link=CYW43_LINK_UP;operation_poll();assert(connected());
    disconnect();mock_network::now+=150;operation_poll();
    mock_network::dhcp_rc=-1;
    const int failed_ip_joins=mock_network::joins;
    assert(!connect_start("HOME","pw") && mock_network::joins==failed_ip_joins);
    assert(std::strcmp(last_error(),"WIFI IP START FAILED")==0);
    assert(radio_ready() && !operation_active());mock_network::dhcp_rc=ERR_OK;
    shutdown();
    assert(mock_network::radio_overlap==0 && mock_network::lock_depth==0);
    std::puts("Production network: cancellation, busy ownership, stale scan/DNS, DHCP/NTP and cleanup: PASS");
}
