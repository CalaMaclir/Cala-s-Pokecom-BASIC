// Inert hardware/menu dependencies for production workflow integration tests.
#include "platform.hpp"
#include "serial_transfer.hpp"
#include "storage.hpp"
#include "network.hpp"
#include "file_server.hpp"
#include "bluetooth_hid_keyboard.hpp"
#include "bluetooth_hid_ble_keyboard.hpp"
#include "bluetooth_manager.hpp"
#include "usb_device.hpp"
#include "usb_msc.hpp"
#include <vector>
#include <algorithm>
namespace rmb::platform { RtcSource rtc_source() { return {}; } }
namespace rmb::platform { bool probe_rtc() { return {}; } }
namespace rmb::platform { const char* rtc_location() { return "HOST STUB"; } }
namespace rmb::platform { const char* rtc_device_name() { return "HOST STUB"; } }
namespace rmb::platform { std::uint8_t rtc_active_address() { return {}; } }
namespace rmb::platform { void clear_screen() {} }
namespace rmb::platform { void enter_sleep_mode(bool refresh_status) {} }
namespace rmb::platform { void enter_bootsel() {} }
namespace rmb::platform { void reboot_system() {} }
namespace rmb::platform { bool set_datetime(const DateTime& value) { return {}; } }
namespace rmb::platform { const char* datetime_last_error() { return "HOST STUB"; } }
namespace rmb::platform { bool get_lcd_backlight(std::uint8_t& value) { return {}; } }
namespace rmb::platform { std::uint32_t full_cpu_clock_hz() { return {}; } }
namespace rmb::platform { const char* serial_transfer_route_name(SerialTransferRoute route) { return "HOST STUB"; } }
namespace rmb::platform { const char* serial_transfer_error() { return "HOST STUB"; } }
namespace rmb::platform { bool begin_serial_transfer(SerialTransferRoute route) { return {}; } }
namespace rmb::platform { void end_serial_transfer() {} }
namespace rmb::platform { int serial_transfer_read(unsigned timeout_ms) { return {}; } }
namespace rmb::platform { int serial_transfer_read_exact(std::uint8_t* data,
    std::size_t size,
    unsigned timeout_ms) { return {}; } }
namespace rmb::platform { bool serial_transfer_write(const std::uint8_t* data, std::size_t size) { return {}; } }
namespace rmb::platform {
static bool diagnostic_lease = false;
bool usb_cdc_ready() { return true; }
bool serial_transfer_active() { return diagnostic_lease; }
DiagnosticSerialResult begin_usb_diagnostic() { if(diagnostic_lease) return DiagnosticSerialResult::Busy; diagnostic_lease=true;return DiagnosticSerialResult::Ready; }
bool write_usb_diagnostic(const char* text) { if(!diagnostic_lease)return false;put_string(text);return true; }
void end_usb_diagnostic() { diagnostic_lease=false; }
}
namespace rmb::platform { const SerialTransferPerformance& serial_transfer_performance() { static const SerialTransferPerformance value{}; return value; } }
namespace rmb::platform { void set_ymodem_rx_bulk(bool enabled) {} }
namespace rmb::platform { void set_tx_packet_coalesce(bool enabled) {} }
namespace rmb::platform { void set_usb_cdc_bulk(bool enabled) {} }
namespace rmb::platform { bool set_uart_transfer_baud(std::uint32_t baud) { return {}; } }
namespace rmb::platform { void set_uart_rx_mode(UartRxMode mode) {} }
namespace rmb::platform { const char* uart_rx_mode_name(UartRxMode mode) { return "HOST STUB"; } }
namespace rmb::storage { bool busy() { return false; } }
namespace rmb::storage { const char* owner_name() { return "HOST STUB"; } }
namespace rmb::storage { bool firmware_owns_card() { return true; } }
namespace rmb::storage { bool begin_usb_host_ownership() { return {}; } }
namespace rmb::storage { void poll() {} }
namespace rmb::storage { UsbEvent take_usb_event() { return {}; } }
namespace rmb::storage { bool recover_firmware_ownership() { return {}; } }
namespace rmb::storage { bool request_usb_safe_return(bool host_ejected) { return {}; } }
namespace rmb::storage { bool request_usb_force_return() { return {}; } }
namespace rmb::storage { bool remount() { return true; } }
namespace rmb::storage { bool list_directory(const char* directory, bool programs_only) { return {}; } }
namespace rmb::storage { std::vector<DirectoryEntry> host_program_listing; }
namespace rmb::storage { std::size_t collect_directory_entries(const char* directory, bool programs_only,
    DirectoryEntry* output, std::size_t max_entries, std::size_t skip,
    bool* more) {
    const auto count = skip < host_program_listing.size()
        ? std::min(max_entries, host_program_listing.size() - skip) : 0u;
    if (count) std::copy_n(host_program_listing.begin() + skip, count, output);
    if (more) *more = skip + count < host_program_listing.size();
    return count;
} }
namespace rmb::storage { bool create_directory(const char* path) { return {}; } }
namespace rmb::storage { bool delete_directory(const char* path, const char* current_file) { return {}; } }
namespace rmb::storage { bool rename_directory(const char* old_path, const char* new_path, const char* current_file) { return {}; } }
namespace rmb::storage { bool root_entry_info(const char* name, DirectoryEntry& output) { return {}; } }
namespace rmb::storage { bool rename_root_file(const char* old_name,
    const char* new_name,
    const char* current_file,
    bool recovery) { return {}; } }
namespace rmb::storage { bool delete_root_file(const char* name, const char* current_file) { return {}; } }
#ifndef CPB_PRODUCTION_NETWORK
namespace rmb::network { int scan(AccessPoint* results, int max_results) { return {}; } }
namespace rmb::network { void disconnect() {} }
namespace rmb::network { bool get_ip(char* output, std::size_t capacity) { return {}; } }
namespace rmb::network { const char* current_ssid() { return "HOST STUB"; } }
namespace rmb::network { bool ntp_time(NetworkDateTime& value,
    int timezone_minutes,
    const char* server) { return {}; } }
namespace rmb::network { const char* last_error() { return "HOST STUB"; } }
#endif
namespace rmb::network { void file_server_stop() {} }
namespace rmb::network { void file_server_poll() {} }
namespace rmb::network { const char* file_server_token() { return "HOST STUB"; } }
namespace rmb::network { const char* file_server_last_error() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid { bool start_scan() { return {}; } }
namespace rmb::bluetooth_hid { void cancel_scan() {} }
namespace rmb::bluetooth_hid { bool scanning() { return {}; } }
namespace rmb::bluetooth_hid { std::size_t discovered_devices(DiscoveredDevice* output,
    std::size_t capacity) { return {}; } }
namespace rmb::bluetooth_hid { bool connect_keyboard(const std::uint8_t address[6]) { return {}; } }
namespace rmb::bluetooth_hid { bool connect_paired_keyboard(const std::uint8_t address[6]) { return {}; } }
namespace rmb::bluetooth_hid { void cancel_connect() {} }
namespace rmb::bluetooth_hid { bool connect_active() { return {}; } }
namespace rmb::bluetooth_hid { bool pairing_code_available() { return {}; } }
namespace rmb::bluetooth_hid { std::uint32_t pairing_code() { return {}; } }
namespace rmb::bluetooth_hid { bool connected() { return {}; } }
namespace rmb::bluetooth_hid { bool disconnect() { return {}; } }
namespace rmb::bluetooth_hid { const char* connected_name() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid { const char* status() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid { const char* last_error() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid_ble { bool forget_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type) { return {}; } }
namespace rmb::bluetooth_hid_ble { bool connect_paired_keyboard(const std::uint8_t address[6], std::uint8_t address_type, const char* name) { return {}; } }
namespace rmb::bluetooth_hid_ble { bool start_scan() { return {}; } }
namespace rmb::bluetooth_hid_ble { void cancel_scan() {} }
namespace rmb::bluetooth_hid_ble { bool scanning() { return {}; } }
namespace rmb::bluetooth_hid_ble { std::size_t discovered_devices(DiscoveredDevice* output,
    std::size_t capacity) { return {}; } }
namespace rmb::bluetooth_hid_ble { bool discovery_capacity_reached() { return {}; } }
namespace rmb::bluetooth_hid_ble { bool connect_keyboard(const std::uint8_t address[6],
    std::uint8_t address_type,
    const char* name) { return {}; } }
namespace rmb::bluetooth_hid_ble { void cancel_connect() {} }
namespace rmb::bluetooth_hid_ble { bool connect_active() { return {}; } }
namespace rmb::bluetooth_hid_ble { bool pairing_code_available() { return {}; } }
namespace rmb::bluetooth_hid_ble { std::uint32_t pairing_code() { return {}; } }
namespace rmb::bluetooth_hid_ble { bool connected() { return {}; } }
namespace rmb::bluetooth_hid_ble { bool disconnect() { return {}; } }
namespace rmb::bluetooth_hid_ble { const char* connected_name() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid_ble { const char* status() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid_ble { const char* last_error() { return "HOST STUB"; } }
namespace rmb::bluetooth_hid_ble { void set_layout(bluetooth_hid::KeyboardLayout layout) {} }
namespace rmb::bluetooth_manager { bool enable() { return {}; } }
namespace rmb::bluetooth_manager { void disable() {} }
namespace rmb::bluetooth_manager { bool enabled() { return {}; } }
namespace rmb::bluetooth_manager { void service() {} }
namespace rmb::bluetooth_manager { std::size_t paired_devices(PairedDeviceInfo* output, std::size_t capacity) { return {}; } }
namespace rmb::bluetooth_manager { bool forget_paired_device(const std::uint8_t address[6]) { return {}; } }
namespace rmb::bluetooth_manager { bool forget_paired_devices() { return {}; } }
namespace rmb::usb_device { bool usb_connected() { return {}; } }
namespace rmb::usb_msc { bool active() { return {}; } }
namespace rmb::usb_msc { bool media_present() { return {}; } }
namespace rmb::usb_msc { bool set_media_present(bool present) { return {}; } }
namespace rmb::usb_msc { bool host_ejected() { return {}; } }
