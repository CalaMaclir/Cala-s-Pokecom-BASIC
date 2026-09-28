#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.hpp"

namespace rmb::rtc {

using platform::DateTime;
using platform::RtcDevice;

inline std::uint8_t bcd_to_bin(std::uint8_t value) {
    return static_cast<std::uint8_t>((value >> 4) * 10u + (value & 0x0fu));
}

inline std::uint8_t bin_to_bcd(int value) {
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

inline const char* device_name(RtcDevice device) {
    switch (device) {
    case RtcDevice::Pcf8563: return "PCF8563";
    case RtcDevice::Ds3231: return "DS3231";
    case RtcDevice::None: break;
    }
    return "AUTO";
}

inline std::uint8_t default_address(RtcDevice device) {
    return device == RtcDevice::Ds3231 ? 0x68u : 0x51u;
}

inline std::uint8_t first_register(RtcDevice device) {
    return device == RtcDevice::Ds3231 ? 0x00u : 0x02u;
}

inline bool valid_datetime(const DateTime& value) {
    return value.year >= 2000 && value.year <= 2199 &&
           value.month >= 1 && value.month <= 12 &&
           value.day >= 1 && value.day <= 31 &&
           value.hour >= 0 && value.hour <= 23 &&
           value.minute >= 0 && value.minute <= 59 &&
           value.second >= 0 && value.second <= 59;
}

inline bool decode(RtcDevice device, const std::uint8_t* data,
                   std::size_t size, DateTime& out) {
    if (!data || size < 7 || device == RtcDevice::None) return false;

    DateTime value;
    if (device == RtcDevice::Pcf8563) {
        value.second = bcd_to_bin(data[0] & 0x7fu);
        value.minute = bcd_to_bin(data[1] & 0x7fu);
        value.hour = bcd_to_bin(data[2] & 0x3fu);
        value.day = bcd_to_bin(data[3] & 0x3fu);
        value.month = bcd_to_bin(data[5] & 0x1fu);
        value.year = 2000 + bcd_to_bin(data[6]);
    } else {
        value.second = bcd_to_bin(data[0] & 0x7fu);
        value.minute = bcd_to_bin(data[1] & 0x7fu);
        if ((data[2] & 0x40u) != 0) {
            int hour = bcd_to_bin(data[2] & 0x1fu);
            if (hour == 12) hour = 0;
            if ((data[2] & 0x20u) != 0) hour += 12;
            value.hour = hour;
        } else {
            value.hour = bcd_to_bin(data[2] & 0x3fu);
        }
        value.day = bcd_to_bin(data[4] & 0x3fu);
        value.month = bcd_to_bin(data[5] & 0x1fu);
        value.year = 2000 + bcd_to_bin(data[6]);
        if ((data[5] & 0x80u) != 0) value.year += 100;
    }

    if (!valid_datetime(value)) return false;
    out = value;
    return true;
}

inline bool encode(RtcDevice device, const DateTime& value,
                   std::uint8_t* data, std::size_t size) {
    if (!data || size < 7 || device == RtcDevice::None ||
        !valid_datetime(value)) return false;

    if (device == RtcDevice::Pcf8563) {
        if (value.year > 2099) return false;
        data[0] = bin_to_bcd(value.second);
        data[1] = bin_to_bcd(value.minute);
        data[2] = bin_to_bcd(value.hour);
        data[3] = bin_to_bcd(value.day);
        data[4] = 0x01u;
        data[5] = bin_to_bcd(value.month);
        data[6] = bin_to_bcd(value.year - 2000);
    } else {
        data[0] = bin_to_bcd(value.second);
        data[1] = bin_to_bcd(value.minute);
        data[2] = bin_to_bcd(value.hour); // force 24-hour mode
        data[3] = 0x01u;                  // day-of-week is not used by CPB
        data[4] = bin_to_bcd(value.day);
        data[5] = bin_to_bcd(value.month);
        if (value.year >= 2100) data[5] |= 0x80u;
        data[6] = bin_to_bcd(value.year % 100);
    }
    return true;
}

} // namespace rmb::rtc
