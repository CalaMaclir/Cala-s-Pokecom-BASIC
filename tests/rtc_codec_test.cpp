#include "rtc_codec.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using namespace rmb;
    using platform::DateTime;
    using platform::RtcDevice;

    {
        const std::uint8_t pcf[7] = {
            0x56, 0x34, 0x12, 0x25, 0x05, 0x09, 0x26
        };
        DateTime value;
        assert(rtc::decode(RtcDevice::Pcf8563, pcf, 7, value));
        assert(value.year == 2026 && value.month == 9 && value.day == 25);
        assert(value.hour == 12 && value.minute == 34 && value.second == 56);

        std::uint8_t encoded[7] = {};
        assert(rtc::encode(RtcDevice::Pcf8563, value, encoded, 7));
        assert(encoded[0] == 0x56 && encoded[1] == 0x34);
        assert(encoded[2] == 0x12 && encoded[3] == 0x25);
        assert(encoded[5] == 0x09 && encoded[6] == 0x26);
    }

    {
        const std::uint8_t ds[7] = {
            0x56, 0x34, 0x12, 0x05, 0x25, 0x09, 0x26
        };
        DateTime value;
        assert(rtc::decode(RtcDevice::Ds3231, ds, 7, value));
        assert(value.year == 2026 && value.month == 9 && value.day == 25);
        assert(value.hour == 12 && value.minute == 34 && value.second == 56);

        std::uint8_t encoded[7] = {};
        assert(rtc::encode(RtcDevice::Ds3231, value, encoded, 7));
        assert(encoded[0] == 0x56 && encoded[1] == 0x34);
        assert(encoded[2] == 0x12 && encoded[4] == 0x25);
        assert(encoded[5] == 0x09 && encoded[6] == 0x26);
    }

    {
        const std::uint8_t ds12pm[7] = {
            0x00, 0x00, 0x69, 0x01, 0x01, 0x01, 0x26
        };
        DateTime value;
        assert(rtc::decode(RtcDevice::Ds3231, ds12pm, 7, value));
        assert(value.hour == 21);
    }

    {
        DateTime value{2101, 2, 3, 4, 5, 6};
        std::uint8_t encoded[7] = {};
        assert(rtc::encode(RtcDevice::Ds3231, value, encoded, 7));
        assert((encoded[5] & 0x80u) != 0);
        DateTime decoded;
        assert(rtc::decode(RtcDevice::Ds3231, encoded, 7, decoded));
        assert(decoded.year == 2101 && decoded.month == 2 && decoded.day == 3);
        assert(!rtc::encode(RtcDevice::Pcf8563, value, encoded, 7));
    }

    assert(rtc::default_address(RtcDevice::Pcf8563) == 0x51);
    assert(rtc::default_address(RtcDevice::Ds3231) == 0x68);
    assert(rtc::first_register(RtcDevice::Pcf8563) == 0x02);
    assert(rtc::first_register(RtcDevice::Ds3231) == 0x00);
}
