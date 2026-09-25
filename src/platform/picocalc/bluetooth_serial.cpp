#include "bluetooth_serial.hpp"
#include "bluetooth_serial_core.hpp"
#include "wireless.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
namespace rmb::bluetooth_serial {
namespace {
constexpr char kDeviceName[]="CPB-PicoCalc";
constexpr std::uint8_t kChannel=1;
constexpr std::uint32_t kClassOfDevice=0x001f00;
constexpr std::size_t kSendChunk=256;
Core core;
bool protocol_initialized=false;
TxRequestState tx_request;
std::uint16_t rfcomm_cid=0,rfcomm_mtu=127;
std::uint8_t service_buffer[150]={};
btstack_packet_callback_registration_t hci_events={};
char error_text[96]="OFF";
char error_snapshot[96]="OFF";

// After a binary transfer ends the peer may still be retransmitting a frame.
// Never let those bytes fall through into the interactive BASIC console.
// Keep discarding until the RFCOMM link has been quiet for this interval.
constexpr std::uint32_t kPostTransferQuietMs = 350;
bool rx_quarantine = false;
std::uint32_t rx_quarantine_until_ms = 0;

void set_error_text(const char* text){std::snprintf(error_text,sizeof(error_text),"%s",text?text:"ERROR");}

void clear_rx_quarantine_locked() {
    rx_quarantine = false;
    rx_quarantine_until_ms = 0;
}

void start_rx_quarantine_locked() {
    rx_quarantine = true;
    rx_quarantine_until_ms =
        to_ms_since_boot(get_absolute_time()) + kPostTransferQuietMs;
}

bool rx_quarantine_active_locked() {
    if (!rx_quarantine) return false;
    const auto now = to_ms_since_boot(get_absolute_time());
    if (static_cast<std::int32_t>(
            now - rx_quarantine_until_ms) >= 0) {
        clear_rx_quarantine_locked();
        return false;
    }
    return true;
}

void extend_rx_quarantine_locked() {
    rx_quarantine_until_ms =
        to_ms_since_boot(get_absolute_time()) + kPostTransferQuietMs;
}
void request_send_locked(){
    const bool ready=core.enabled()&&core.connected()&&rfcomm_cid;
    if(!tx_request.begin(ready,core.tx_size()))return;
    const std::uint8_t rc=rfcomm_request_can_send_now_event(rfcomm_cid);
    if(rc!=ERROR_CODE_SUCCESS){
        tx_request.on_request_failed();
        set_error_text("RFCOMM SEND REQUEST FAILED");
    }
}
void packet_handler(std::uint8_t type,std::uint16_t channel,std::uint8_t* packet,std::uint16_t size){
    bd_addr_t address;
    if(type==RFCOMM_DATA_PACKET){
        if(core.enabled()&&core.connected()&&channel==rfcomm_cid){
            if(rx_quarantine_active_locked()){
                // Intentionally discard late YMODEM/XMODEM retransmissions.
                // Every packet extends the quiet window so the console is
                // reopened only after the peer has actually stopped sending.
                extend_rx_quarantine_locked();
                return;
            }
            const auto n=core.enqueue_rx(packet,size);
            if(n!=size)set_error_text("RX BUFFER OVERFLOW");
        }
        return;
    }
    if(type!=HCI_EVENT_PACKET)return;
    switch(hci_event_packet_get_type(packet)){
    case BTSTACK_EVENT_STATE:
        if(core.enabled()&&btstack_event_state_get_state(packet)==HCI_STATE_WORKING){gap_connectable_control(1);gap_discoverable_control(1);core.set_discoverable();set_error_text("DISCOVERABLE");}
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet,address);gap_pin_code_negative(address);break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet,address);gap_ssp_confirmation_response(address);break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        if(hci_event_simple_pairing_complete_get_status(packet)!=ERROR_CODE_SUCCESS)set_error_text("PAIRING FAILED");break;
    case RFCOMM_EVENT_INCOMING_CONNECTION:{
        const auto incoming=rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
        if(!core.enabled()||rfcomm_cid){rfcomm_decline_connection(incoming);break;}
        rfcomm_cid=incoming;rfcomm_accept_connection(rfcomm_cid);set_error_text("CONNECTING");break;}
    case RFCOMM_EVENT_CHANNEL_OPENED:{
        const auto rc=rfcomm_event_channel_opened_get_status(packet);
        if(rc!=ERROR_CODE_SUCCESS||!core.enabled()){rfcomm_cid=0;core.set_error();set_error_text("RFCOMM OPEN FAILED");break;}
        rfcomm_cid=rfcomm_event_channel_opened_get_rfcomm_cid(packet);
        rfcomm_mtu=rfcomm_event_channel_opened_get_max_frame_size(packet);if(!rfcomm_mtu)rfcomm_mtu=127;
        tx_request.reset();clear_rx_quarantine_locked();
        core.set_connected(true);set_error_text("CONNECTED");break;}
    case RFCOMM_EVENT_CAN_SEND_NOW:{
        tx_request.on_can_send_now();
        if(!core.enabled()||!core.connected()||!rfcomm_cid||!core.tx_size())break;
        std::uint8_t chunk[kSendChunk]={};
        const auto limit=std::min<std::size_t>(rfcomm_mtu,sizeof(chunk));
        const auto count=core.peek_tx(chunk,limit);if(!count)break;
        const auto rc=rfcomm_send(rfcomm_cid,chunk,static_cast<std::uint16_t>(count));
        if(rc==ERROR_CODE_SUCCESS){core.complete_tx_send(count);request_send_locked();}else set_error_text("RFCOMM SEND FAILED");
        break;}
    case RFCOMM_EVENT_CHANNEL_CLOSED:
        rfcomm_cid=0;rfcomm_mtu=127;tx_request.reset();
        clear_rx_quarantine_locked();
        if(core.enabled()){core.set_connected(false);gap_connectable_control(1);gap_discoverable_control(1);set_error_text("DISCOVERABLE");}
        break;
    default:break;
    }
}
void setup_locked(){
    if(protocol_initialized)return;
    l2cap_init();rfcomm_init();rfcomm_register_service(packet_handler,kChannel,0xffff);
    sdp_init();std::memset(service_buffer,0,sizeof(service_buffer));
    spp_create_sdp_record(service_buffer,sdp_create_service_record_handle(),kChannel,"CPB-PicoCalc Serial Port");
    sdp_register_service(service_buffer);
    hci_events.callback=&packet_handler;hci_add_event_handler(&hci_events);
    gap_set_class_of_device(kClassOfDevice);gap_set_local_name(kDeviceName);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    protocol_initialized=true;
}
} // namespace
bool init(){if(!core.enabled())set_error_text("OFF");return true;}
bool enable(){if(core.enabled())return true;if(!wireless::init()){set_error_text(wireless::last_error());return false;}cyw43_arch_lwip_begin();core.enable();clear_rx_quarantine_locked();setup_locked();rfcomm_cid=0;rfcomm_mtu=127;tx_request.reset();gap_connectable_control(1);gap_discoverable_control(1);hci_power_control(HCI_POWER_ON);set_error_text("READY");cyw43_arch_lwip_end();return true;}
void disable(){if(!core.enabled())return;if(!wireless::initialized()){core.disable();clear_rx_quarantine_locked();set_error_text("OFF");return;}cyw43_arch_lwip_begin();gap_discoverable_control(0);gap_connectable_control(0);if(rfcomm_cid)rfcomm_disconnect(rfcomm_cid);rfcomm_cid=0;rfcomm_mtu=127;tx_request.reset();clear_rx_quarantine_locked();hci_power_control(HCI_POWER_OFF);core.disable();set_error_text("OFF");cyw43_arch_lwip_end();}
bool enabled(){return core.enabled();}
bool connected(){if(!wireless::initialized())return false;cyw43_arch_lwip_begin();const bool v=core.connected();cyw43_arch_lwip_end();return v;}
const char* status(){if(!wireless::initialized())return "OFF";cyw43_arch_lwip_begin();const State v=core.state();cyw43_arch_lwip_end();switch(v){case State::Off:return "OFF";case State::Ready:return "READY";case State::Discoverable:return "DISCOVERABLE";case State::Connected:return "CONNECTED";case State::Error:return "ERROR";}return "ERROR";}
const char* device_name(){return kDeviceName;}
const char* last_error(){if(!wireless::initialized())return "OFF";cyw43_arch_lwip_begin();std::snprintf(error_snapshot,sizeof(error_snapshot),"%s",error_text);cyw43_arch_lwip_end();return error_snapshot;}
bool set_console_enabled(bool value){if(!wireless::initialized())return !value;cyw43_arch_lwip_begin();const bool ok=core.set_console_enabled(value);cyw43_arch_lwip_end();return ok;}
bool console_enabled(){if(!wireless::initialized())return false;cyw43_arch_lwip_begin();const bool v=core.console_enabled();cyw43_arch_lwip_end();return v;}
void set_test_terminal_active(bool value){if(!wireless::initialized())return;cyw43_arch_lwip_begin();core.set_test_terminal_active(value);cyw43_arch_lwip_end();}
bool test_terminal_active(){if(!wireless::initialized())return false;cyw43_arch_lwip_begin();const bool v=core.test_terminal_active();cyw43_arch_lwip_end();return v;}
bool begin_transfer(){
 if(!wireless::initialized())return false;
 const auto deadline=make_timeout_time_ms(1000);
 while(true){
  cyw43_arch_lwip_begin();
  if(!core.enabled()||!core.connected()||core.test_terminal_active()){
   set_error_text("BLUETOOTH TRANSFER NOT AVAILABLE");cyw43_arch_lwip_end();return false;
  }
  if(core.tx_size()==0){
   clear_rx_quarantine_locked();
   const bool ok=core.begin_transfer();if(ok)set_error_text("TRANSFER ACTIVE");
   cyw43_arch_lwip_end();return ok;
  }
  request_send_locked();cyw43_arch_lwip_end();
  if(time_reached(deadline)){cyw43_arch_lwip_begin();set_error_text("BLUETOOTH TX BUSY");cyw43_arch_lwip_end();return false;}
  sleep_ms(1);
 }
}
void end_transfer(){if(!wireless::initialized())return;cyw43_arch_lwip_begin();core.end_transfer();start_rx_quarantine_locked();if(core.connected())set_error_text("CONNECTED");cyw43_arch_lwip_end();}
bool transfer_active(){if(!wireless::initialized())return false;cyw43_arch_lwip_begin();const bool v=core.transfer_active();cyw43_arch_lwip_end();return v;}
int read_console(){if(!wireless::initialized())return -1;cyw43_arch_lwip_begin();const int v=rx_quarantine_active_locked()?-1:core.read_console_rx();cyw43_arch_lwip_end();return v;}
int read_test(){if(!wireless::initialized())return -1;cyw43_arch_lwip_begin();const int v=rx_quarantine_active_locked()?-1:core.read_test_rx();cyw43_arch_lwip_end();return v;}
std::size_t read_transfer(std::uint8_t* destination,std::size_t capacity){
 if(!destination||!capacity||!wireless::initialized())return 0;
 cyw43_arch_lwip_begin();
 const auto count=core.read_transfer_rx(destination,capacity);
 cyw43_arch_lwip_end();
 return count;
}
int read_transfer(){
 std::uint8_t value=0;
 return read_transfer(&value,1)==1?static_cast<int>(value):-1;
}
bool write_transfer(const std::uint8_t* data,std::size_t length){
 if(!length)return true;
 if(!data||!wireless::initialized()||length>Core::kTxCapacity)return false;
 const auto deadline=make_timeout_time_ms(1000);
 while(true){
  cyw43_arch_lwip_begin();
  if(core.rx_owner()!=RxOwner::Transfer){set_error_text("BLUETOOTH DISCONNECTED");cyw43_arch_lwip_end();return false;}
  if(core.tx_free()>=length){
   const bool ok=core.enqueue_tx_exact(data,length);if(ok)request_send_locked();
   cyw43_arch_lwip_end();
   if(!ok)return false;

   // YMODEM/XMODEM receiver control bytes (ACK/NAK/C/CAN and the small
   // packet prefix/suffix writes) must actually leave RFCOMM before the
   // protocol waits for the peer. Merely queueing a one-byte ACK can race
   // Tera Term's response timer over SPP. Large payload writes remain
   // asynchronous to preserve sender throughput.
   if(length>3)return true;
   while(!time_reached(deadline)){
    cyw43_arch_lwip_begin();
    if(core.rx_owner()!=RxOwner::Transfer){
     set_error_text("BLUETOOTH DISCONNECTED");
     cyw43_arch_lwip_end();return false;
    }
    request_send_locked();
    const bool drained=core.tx_size()==0;
    cyw43_arch_lwip_end();
    if(drained)return true;
    sleep_ms(1);
   }
   cyw43_arch_lwip_begin();
   set_error_text("TRANSFER TX TIMEOUT");
   cyw43_arch_lwip_end();
   return false;
  }
  request_send_locked();cyw43_arch_lwip_end();
  if(time_reached(deadline))break;
  sleep_ms(1);
 }
 cyw43_arch_lwip_begin();core.record_tx_drop(length);set_error_text("TRANSFER TX BACKPRESSURE");cyw43_arch_lwip_end();
 return false;
}
bool write(const std::uint8_t* data,std::size_t length){if(!data||!length||!wireless::initialized())return false;cyw43_arch_lwip_begin();const auto n=core.enqueue_tx(data,length);if(n!=length)set_error_text("TX BUFFER OVERFLOW");request_send_locked();cyw43_arch_lwip_end();return n==length;}
bool write_text(const char* text){return text&&write(reinterpret_cast<const std::uint8_t*>(text),std::strlen(text));}
bool write_console(const std::uint8_t* data,std::size_t length){
 if(!data||!length||!wireless::initialized())return false;
 std::size_t done=0;const auto deadline=make_timeout_time_ms(25);
 while(done<length){
  cyw43_arch_lwip_begin();
  if(core.rx_owner()!=RxOwner::Console){cyw43_arch_lwip_end();return false;}
  const auto amount=std::min(core.tx_free(),length-done);
  if(amount){done+=core.enqueue_tx(data+done,amount);request_send_locked();}
  cyw43_arch_lwip_end();
  if(done==length)return true;
  if(time_reached(deadline))break;
  sleep_ms(1);
 }
 cyw43_arch_lwip_begin();core.record_tx_drop(length-done);set_error_text("CONSOLE TX BACKPRESSURE");cyw43_arch_lwip_end();
 return false;
}
bool write_console_text(const char* text){return text&&write_console(reinterpret_cast<const std::uint8_t*>(text),std::strlen(text));}
void write_console_char(char value){const auto byte=static_cast<std::uint8_t>(value);write_console(&byte,1);}
void service(){if(!wireless::initialized()||!core.enabled())return;cyw43_arch_lwip_begin();rx_quarantine_active_locked();request_send_locked();cyw43_arch_lwip_end();}
std::uint32_t rx_overflow_count(){if(!wireless::initialized())return 0;cyw43_arch_lwip_begin();const auto v=core.rx_overflow_count();cyw43_arch_lwip_end();return v;}
std::uint32_t tx_overflow_count(){if(!wireless::initialized())return 0;cyw43_arch_lwip_begin();const auto v=core.tx_overflow_count();cyw43_arch_lwip_end();return v;}
std::uint32_t tx_queued_bytes(){if(!wireless::initialized())return 0;cyw43_arch_lwip_begin();const auto v=core.tx_queued_bytes();cyw43_arch_lwip_end();return v;}
std::uint32_t tx_sent_bytes(){if(!wireless::initialized())return 0;cyw43_arch_lwip_begin();const auto v=core.tx_sent_bytes();cyw43_arch_lwip_end();return v;}
} // namespace rmb::bluetooth_serial
