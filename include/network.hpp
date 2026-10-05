#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::network {

struct AccessPoint {
    char ssid[33] = {};
    int rssi = -127;
    bool secure = true;
};

struct NetworkDateTime {
    int year = 2000;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

constexpr std::uint32_t kScanObservationMs = 5000;
// Firmware owns scan duration; bound the completion wait after observation.
constexpr std::uint32_t kScanDrainTimeoutMs = 10000;

bool init();
void shutdown();
bool initialized();

// Only one foreground operation owns the network. Starts never wait for completion.
enum class OperationState { Idle, Scanning, DrainingScan, Disassociating, Associating, WaitingForIp,
                            SyncingTime, Succeeded, Failed, Cancelled };
// Bounded, cooperative wait for the driver DISASSOC event (not an arbitrary delay).
constexpr std::uint32_t kRadioSettleTimeoutMs = 2000;
bool radio_ready();
bool operation_active();
OperationState operation_state();
void operation_poll();
void operation_cancel();
bool scan_start();
int scan_result(AccessPoint* results, int max_results);
int scan_observed_count();
bool connect_start(const char* ssid, const char* password);
bool ntp_start(int timezone_minutes = 540, const char* server = "pool.ntp.org");
bool ntp_result(NetworkDateTime& value);

void disconnect();
bool connected();

bool get_ip(char* output, std::size_t capacity);
const char* current_ssid();

const char* last_error();

} // namespace rmb::network
