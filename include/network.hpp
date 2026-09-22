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

bool init();
bool initialized();

int scan(AccessPoint* results, int max_results);

bool connect(const char* ssid, const char* password);
void disconnect();
bool connected();

bool get_ip(char* output, std::size_t capacity);
const char* current_ssid();

bool ntp_time(
    NetworkDateTime& value,
    int timezone_minutes = 540,
    const char* server = "pool.ntp.org"
);
const char* last_error();

} // namespace rmb::network
