// Production REPL/password editor/network state machine; hardware is mocked.
#define CPB_REAL_LINE_EDITOR 1
#define CPB_PRODUCTION_NETWORK 1
#include "benchmark_platform.hpp"
#include "wifi_profiles.hpp"
#include <cstdio>
namespace rmb::platform {
void begin_command_input() {} void end_command_input() {}
bool terminal_console_enabled() { return false; }
void screen_put_char(char c) { output += c; }
void serial_put_char_raw(char) {} void serial_put_string_raw(const char*) {}
}
namespace rmb::network {
bool file_server_start() { return false; }
bool file_server_running() { return false; }
}
using namespace rmb;
using namespace rmb::network;
auto snapshot(const wifi_profiles::Profiles& profiles) {
    std::array<wifi_profiles::WifiProfile,wifi_profiles::kCount> result;
    std::copy_n(profiles,wifi_profiles::kCount,result.begin());
    return result;
}
bool visible(const char* text) {
    for (const auto& row:ui_rows) if(row.text.find(text)!=std::string::npos)return true;
    return false;
}
enum class AddCase { Success, CancelMenu, AssocTimeout, CancelAssoc,
                     DhcpRetry, CancelDhcp, CancelDhcpMenu, IpStartRetry };
void run_add(Repl& repl, const std::vector<std::string>& passwords,
             AddCase scenario=AddCase::Success, bool secure=true) {
    editor_keys.clear();editor_key_index=0;output.clear();
    for(const auto& password:passwords) {
        for(char c:password)editor_keys.push_back(c);
        editor_keys.push_back('\r');
    }
    const auto profiles_before=snapshot(repl.settings_.wifi_profiles);
    const auto config_before=host_config;
    int retries=0,polls=0,last_join_leaves=-1;
    bool saw_ip_wait=false;
    const int initial_joins=mock_network::joins;
    const bool success=scenario!=AddCase::CancelMenu && scenario!=AddCase::CancelAssoc &&
        scenario!=AddCase::CancelDhcp && scenario!=AddCase::CancelDhcpMenu;
    if(scenario==AddCase::IpStartRetry)mock_network::dhcp_rc=-1;
    mock_network::join_link=[&](const char* ssid,const char* password) {
        assert(std::strcmp(ssid,"HOME")==0 && mock_network::dhcp_running);
        assert(mock_network::interface.ip.addr==0);
        last_join_leaves=mock_network::leaves;
        if(scenario==AddCase::CancelAssoc && mock_network::joins>=initial_joins+2)
            return CYW43_LINK_JOIN;
        if(scenario==AddCase::AssocTimeout && mock_network::last_password!="correct")
            return CYW43_LINK_JOIN;
        if(!secure) {
            assert(!*password && mock_network::last_auth_mode==CYW43_AUTH_OPEN);
            return CYW43_LINK_NOIP;
        }
        return std::strcmp(password,"correct")==0 ? CYW43_LINK_NOIP : CYW43_LINK_BADAUTH;
    };
    wifi_key=[&]() -> platform::RuntimeKeyResult {
        assert(++polls<20000);
        if(!operation_active() && visible("Retry")) {
            assert(++retries<6);
            if(scenario==AddCase::CancelMenu || scenario==AddCase::CancelDhcpMenu)
                return {platform::RuntimeKeyType::Break,0};
            if(scenario==AddCase::IpStartRetry)mock_network::dhcp_rc=ERR_OK;
            return {platform::RuntimeKeyType::Key,'\r'};
        }
        if(scenario==AddCase::CancelAssoc && mock_network::joins>=initial_joins+2 &&
           operation_state()==OperationState::Associating)
            return {platform::RuntimeKeyType::Break,0};
        if(operation_state()==OperationState::WaitingForIp) {
            saw_ip_wait=true;
            assert(!connected() && mock_network::dhcp_running);
            assert(std::memcmp(profiles_before.data(),repl.settings_.wifi_profiles,
                               sizeof(repl.settings_.wifi_profiles))==0);
            assert(host_config==config_before); // NOIP must never commit.
            assert(mock_network::leaves==last_join_leaves); // No early leave.
            if(scenario==AddCase::CancelDhcp)return {platform::RuntimeKeyType::Break,0};
            if(scenario==AddCase::CancelDhcpMenu ||
               (scenario==AddCase::DhcpRetry && retries==0))return {};
            mock_network::interface.ip.addr=789;mock_network::link=CYW43_LINK_UP;
        }
        if(operation_state()==OperationState::Succeeded && visible("K=known  *=secured")) {
            const bool known=wifi_profiles::find(repl.settings_.wifi_profiles,"HOME")>=0;
            assert(visible(secure ? (known ? "K* HOME" : " * HOME") :
                                   (known ? "K  HOME" : "   HOME")));
            return {platform::RuntimeKeyType::Key,'\r'};
        }
        return {};
    };
    mock_network::auto_scan=false;
    int scan_generation=-1;
    auto old=wifi_key;
    wifi_key=[&,old]() -> platform::RuntimeKeyResult {
        if(operation_state()==OperationState::Scanning && scan_generation!=mock_network::starts) {
            scan_generation=mock_network::starts;
            mock_network::emit("HOME",-42,secure);mock_network::scanning=false;
        }
        return old();
    };
    repl.add_wifi_network();wifi_key=nullptr;mock_network::dhcp_rc=ERR_OK;
    assert(mock_network::radio_overlap==0);
    if(success) {
        assert(saw_ip_wait && connected() && std::strcmp(current_ssid(),"HOME")==0);
        assert(mock_network::interface.ip.addr==789 && mock_network::dhcp_running);
        assert(mock_network::leaves==last_join_leaves);
        assert(visible("CONNECTED - PROFILE SAVED"));
    } else {
        assert(std::memcmp(profiles_before.data(),repl.settings_.wifi_profiles,
                           sizeof(repl.settings_.wifi_profiles))==0 && host_config==config_before);
    }
}
void connect_with_dhcp(Repl& repl, bool toggle_wifi, bool restore_active=false) {
    mock_network::auto_scan=true;
    // Firmware scan may extend beyond CPB's five-second observation window.
    mock_network::auto_scan_duration_ms=6000;
    mock_network::join_link=[](const char* ssid,const char*) {
        assert(std::strcmp(ssid,"HOME")==0);
        assert(!mock_network::scanning);
        assert(mock_network::dhcp_running);
        assert(mock_network::interface.ip.addr==0);
        return CYW43_LINK_NOIP;
    };
    int polls=0,phase=0;bool saw_drain=false;
    wifi_key=[&]() -> platform::RuntimeKeyResult {
        assert(++polls<20000);
        if(operation_state()==OperationState::DrainingScan)saw_drain=true;
        if(operation_state()==OperationState::WaitingForIp) {
            assert(mock_network::dhcp_running && !connected());
            mock_network::interface.ip.addr=789;mock_network::link=CYW43_LINK_UP;
        }
        if(toggle_wifi && !operation_active()) {
            if(phase==0) { assert(repl.settings_.wifi_enabled);++phase;return {platform::RuntimeKeyType::Key,'\r'}; }
            if(phase==1) { assert(!repl.settings_.wifi_enabled);++phase;return {platform::RuntimeKeyType::Key,'\r'}; }
            if(connected())return {platform::RuntimeKeyType::Key,0x1b};
        }
        return {};
    };
    const int scans=mock_network::starts;
    if(toggle_wifi)repl.menu_wifi();
    else if(restore_active)assert(repl.connect_wifi_profile(0,nullptr,true));
    else assert(repl.auto_connect_wifi());
    wifi_key=nullptr;
    assert(connected() && mock_network::radio_overlap==0);
    assert(restore_active ? mock_network::starts==scans : mock_network::starts>scans);
    assert(restore_active || saw_drain);
    mock_network::auto_scan_duration_ms=0;
}
void check_auto_scan_failures(Repl& repl) {
    const auto saved=snapshot(repl.settings_.wifi_profiles);const auto config=host_config;
    const int joins=mock_network::joins,leaves=mock_network::leaves;
    mock_network::auto_scan=true;mock_network::auto_scan_duration_ms=6000;
    wifi_key=[]() -> platform::RuntimeKeyResult {
        return operation_state()==OperationState::DrainingScan ?
            platform::RuntimeKeyResult{platform::RuntimeKeyType::Break,0} : platform::RuntimeKeyResult{};
    };
    assert(!repl.auto_connect_wifi() && operation_state()==OperationState::Cancelled);
    assert(connected() && mock_network::joins==joins && mock_network::leaves==leaves);
    assert(!scan_start()); // The cancelled physical scan still belongs to firmware.
    platform::sleep_millis(6000);
    mock_network::auto_scan_duration_ms=kScanObservationMs+kScanDrainTimeoutMs+1000;
    wifi_key=nullptr;
    assert(!repl.auto_connect_wifi());
    assert(visible("WIFI SCAN FINISH TIMEOUT"));
    assert(connected() && mock_network::joins==joins && mock_network::leaves==leaves);
    assert(std::memcmp(saved.data(),repl.settings_.wifi_profiles,sizeof(repl.settings_.wifi_profiles))==0);
    assert(host_config==config);
    platform::sleep_millis(2000);
    // After physical completion, RSSI-ranked fallback still works.
    wifi_profiles::upsert(repl.settings_.wifi_profiles,"OFFICE","wrong");
    mock_network::auto_scan_duration_ms=6000;
    mock_network::attempts.clear();
    mock_network::join_link=[](const char* ssid,const char*) {
        assert(!mock_network::scanning && mock_network::dhcp_running);
        return std::strcmp(ssid,"OFFICE")==0 ? CYW43_LINK_BADAUTH : CYW43_LINK_NOIP;
    };
    wifi_key=[]() -> platform::RuntimeKeyResult {
        if(operation_state()==OperationState::WaitingForIp) {
            mock_network::interface.ip.addr=789;mock_network::link=CYW43_LINK_UP;
        }
        return {};
    };
    assert(repl.auto_connect_wifi());wifi_key=nullptr;
    assert((mock_network::attempts==std::vector<std::string>{"OFFICE","HOME"}));
    assert(connected() && mock_network::radio_overlap==0);
    std::copy(saved.begin(),saved.end(),repl.settings_.wifi_profiles);
    mock_network::auto_scan_duration_ms=0;
}
void check_scan_markers(Repl& repl) {
    // Mark saved SSIDs regardless of Enabled or security; don't mark new SSIDs.
    const auto saved=snapshot(repl.settings_.wifi_profiles);
    wifi_profiles::upsert(repl.settings_.wifi_profiles,"OPEN","");
    repl.settings_.wifi_profiles[0].enabled=false;
    mock_network::auto_scan=false;
    int scans=-1;
    wifi_key=[&]() -> platform::RuntimeKeyResult {
        if(operation_state()==OperationState::Scanning && scans!=mock_network::starts) {
            scans=mock_network::starts;mock_network::emit("HOME",-20);
            mock_network::emit("OPEN",-30,false);mock_network::emit("NEW",-40);
            mock_network::scanning=false;
        }
        if(visible("K=known  *=secured")) {
            assert(visible("K* HOME") && visible("K  OPEN") && visible(" * NEW"));
            return {platform::RuntimeKeyType::Key,0x1b};
        }
        return {};
    };
    char ssid[33];bool secure;
    assert(!repl.pick_wifi_network(ssid,sizeof(ssid),secure));wifi_key=nullptr;
    std::copy(saved.begin(),saved.end(),repl.settings_.wifi_profiles);
}
void check_ntp_notifications(Repl& repl) {
    const auto saved=snapshot(repl.settings_.wifi_profiles);const auto config=host_config;
    const int leaves=mock_network::leaves;
    const auto ip=mock_network::interface.ip.addr;
    const std::string ssid=current_ssid();
    std::strcpy(repl.settings_.wifi_ntp_server,"different.example");
    int warnings=0;
    const char* expected="NTP DNS FAILED";
    ui_before_key=[&]() {
        ++warnings;
        assert(visible("NTP SYNC FAILED") && visible(expected));
        assert(visible("Server: different.example") && visible("WI-FI STILL CONNECTED"));
    };
    // Immediate start failure was silent during Auto Time.
    mock_network::dns_rc=-1;
    assert(!repl.sync_rtc_from_network(false) && warnings==1);
    assert(mock_network::pcb_live==0 && mock_network::leaves==leaves);
    // No response after successful DNS resolution: bounded NTP timeout.
    expected="NTP TIMEOUT";mock_network::dns_rc=ERR_OK;
    assert(!repl.sync_rtc_from_network(false) && warnings==2);
    assert(mock_network::pcb_live==0 && mock_network::leaves==leaves);
    // Async DNS failure also reaches the same visible warning screen.
    expected="NTP RESPONSE FAILED";mock_network::dns_rc=ERR_INPROGRESS;
    wifi_key=[]() -> platform::RuntimeKeyResult {
        if(operation_state()==OperationState::SyncingTime) {
            const auto request=mock_network::dns_requests.back();
            request.callback("different.example",nullptr,request.arg);
        }
        return {};
    };
    assert(!repl.sync_rtc_from_network(false) && warnings==3);
    // User cancellation returns directly without a failure warning.
    wifi_key=[]() -> platform::RuntimeKeyResult {
        return {platform::RuntimeKeyType::Break,0};
    };
    assert(!repl.sync_rtc_from_network(false) && warnings==3);
    assert(operation_state()==OperationState::Cancelled);
    // A valid reply succeeds without a warning and releases the UDP PCB.
    mock_network::dns_rc=ERR_OK;
    wifi_key=[]() -> platform::RuntimeKeyResult {
        if(operation_state()==OperationState::SyncingTime) {
            auto* pcb=mock_network::last_pcb;auto* packet=pbuf_alloc(0,48,0);
            std::memset(packet->payload,0,48);auto* bytes=static_cast<unsigned char*>(packet->payload);
            bytes[0]=0x24;bytes[1]=1;const std::uint32_t seconds=2208988800u+1704067200u;
            for(int i=0;i<4;++i)bytes[40+i]=seconds>>(24-i*8);
            const ip_addr_t address{1};pcb->callback(pcb->arg,pcb,packet,&address,123);
        }
        return {};
    };
    assert(repl.sync_rtc_from_network(false) && warnings==3 && mock_network::pcb_live==0);
    // Return to Cancelled to exercise a subsequent immediate start failure.
    mock_network::dns_rc=ERR_INPROGRESS;
    wifi_key=[]() -> platform::RuntimeKeyResult { return {platform::RuntimeKeyType::Break,0}; };
    assert(!repl.sync_rtc_from_network(false) && warnings==3);
    wifi_key=nullptr;
    // A start failure after Cancel must not inherit the old cancelled state.
    repl.settings_.wifi_ntp_server[0]='\0';expected="NTP SERVER NOT SET";
    ui_before_key=[&]() { ++warnings;assert(visible(expected) && visible("WI-FI STILL CONNECTED")); };
    assert(!repl.sync_rtc_from_network(false) && warnings==4);
    std::strcpy(repl.settings_.wifi_ntp_server,"different.example");
    expected="NTP DNS FAILED";mock_network::dns_rc=-1;
    ui_before_key=[&]() {
        ++warnings;assert(visible("NTP SYNC FAILED") && visible(expected));
        assert(visible("Server: different.example") && visible("WI-FI STILL CONNECTED"));
    };
    // Auto Connect succeeds as Wi-Fi even when its Auto Time fails.
    const int previous_joins=mock_network::joins;
    repl.settings_.wifi_auto_rtc=true;connect_with_dhcp(repl,false);
    assert(warnings==5 && mock_network::joins==previous_joins+1);
    // Known Networks Connect Now uses the same notifier once.
    int phase=0;
    wifi_key=[&]() -> platform::RuntimeKeyResult {
        if(operation_active()) {
            if(operation_state()==OperationState::WaitingForIp) {
                mock_network::interface.ip.addr=ip;mock_network::link=CYW43_LINK_UP;
            }
            return {};
        }
        if(phase++==0)return {platform::RuntimeKeyType::Key,'\r'};
        return {platform::RuntimeKeyType::Key,0x1b};
    };
    repl.menu_wifi_profile(0);assert(warnings==6);
    // Sync Time Now shows one warning; menu does not add a duplicate.
    int keys=0;
    wifi_key=[&]() -> platform::RuntimeKeyResult {
        if(keys++<6)return {platform::RuntimeKeyType::Key,0xb6};
        if(keys==7)return {platform::RuntimeKeyType::Key,'\r'};
        return {platform::RuntimeKeyType::Key,0x1b};
    };
    repl.menu_wifi();assert(warnings==7);
    wifi_key=nullptr;ui_before_key=nullptr;mock_network::dns_rc=ERR_INPROGRESS;
    assert(connected() && initialized() && ssid==current_ssid());
    assert(mock_network::interface.ip.addr==ip && mock_network::dhcp_running && mock_network::pcb_live==0);
    assert(std::memcmp(saved.data(),repl.settings_.wifi_profiles,sizeof(repl.settings_.wifi_profiles))==0);
    assert(host_config==config);
}
int main() {
    mock_network::leave_delay_ms=120;
    assert(init());
    Repl repl;repl.settings_.wifi_enabled=true;
    run_add(repl,{"wrong","wrong2","correct"});
    assert(wifi_profiles::count(repl.settings_.wifi_profiles)==1);
    assert(std::strcmp(repl.settings_.wifi_profiles[0].password,"correct")==0);
    assert(connected() && radio_ready());
    const int scans_before=mock_network::starts;
    connect_with_dhcp(repl,false);
    assert(mock_network::starts>scans_before && connected());
    connect_with_dhcp(repl,true);
    check_auto_scan_failures(repl);
    check_ntp_notifications(repl);
    check_scan_markers(repl);
    shutdown();assert(init());
    repl.settings_.wifi_profiles[0].enabled=false;
    std::strcpy(repl.settings_.wifi_profiles[0].password,"old");
    repl.save_settings();const auto before=snapshot(repl.settings_.wifi_profiles);const auto config=host_config;
    run_add(repl,{"wrong"},AddCase::CancelMenu);
    assert(std::memcmp(before.data(),repl.settings_.wifi_profiles,sizeof(repl.settings_.wifi_profiles))==0 && host_config==config);
    run_add(repl,{"wrong","correct"});
    assert(!repl.settings_.wifi_profiles[0].enabled && wifi_profiles::count(repl.settings_.wifi_profiles)==1);
    assert(std::strcmp(repl.settings_.wifi_profiles[0].password,"correct")==0);
    // Disabled remains excluded from new manual/auto selection, but an active
    // Add connection can be restored after a PLL/radio reinitialization.
    assert(connected() && !repl.connect_wifi_profile(0));
    shutdown();assert(init());connect_with_dhcp(repl,false,true);
    assert(!repl.settings_.wifi_profiles[0].enabled);
    run_add(repl,{"slow","correct"},AddCase::AssocTimeout);
    run_add(repl,{"correct"},AddCase::DhcpRetry);
    run_add(repl,{"correct"},AddCase::IpStartRetry);
    run_add(repl,{"correct"},AddCase::CancelDhcpMenu);
    run_add(repl,{"correct"},AddCase::CancelDhcp);
    assert(operation_state()==OperationState::Disassociating);
    platform::sleep_millis(120);operation_poll();assert(radio_ready());
    Repl empty;empty.settings_.wifi_enabled=true;
    const auto saved=host_config;
    run_add(empty,{"wrong"},AddCase::CancelMenu);
    assert(wifi_profiles::count(empty.settings_.wifi_profiles)==0 && saved==host_config);
    const auto existing=snapshot(repl.settings_.wifi_profiles);const auto config_before_cancel=host_config;
    run_add(repl,{"wrong","correct"},AddCase::CancelAssoc);
    assert(std::memcmp(existing.data(),repl.settings_.wifi_profiles,sizeof(repl.settings_.wifi_profiles))==0);
    assert(host_config==config_before_cancel);
    assert(operation_state()==OperationState::Disassociating);
    platform::sleep_millis(120);operation_poll();assert(radio_ready());
    for(int i=0;i<5;++i) {
        repl.settings_.wifi_profiles[0].enabled=true;
        run_add(repl,{"wrong","wrong2","correct"});
        assert(radio_ready());
        assert(mock_network::dhcp_running && connected() && *current_ssid());
        connect_with_dhcp(repl,false);
        connect_with_dhcp(repl,true);
    }
    // Open APs also complete DHCP before saving and stay connected.
    run_add(empty,{},AddCase::Success,false);
    assert(wifi_profiles::count(empty.settings_.wifi_profiles)==1);
    assert(!*empty.settings_.wifi_profiles[0].password && connected());
    shutdown();assert(mock_network::lock_depth==0);
    std::puts("Production REPL: known markers, Add -> DHCP/connect/save, retained link, retries/cancel, Auto Connect/Enable Wi-Fi: PASS");
}
