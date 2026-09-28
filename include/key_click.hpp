#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rmb::audio {
enum class KeyClickMode : std::uint8_t { Off, Low, Mid, High };
inline const char* key_click_name(KeyClickMode mode) {
    switch (mode) {
    case KeyClickMode::Off: return "OFF";
    case KeyClickMode::Mid: return "MID";
    case KeyClickMode::High: return "HIGH";
    default: return "LOW";
    }
}
inline bool key_click_setting_is(const char* value, const char* expected) {
    if (!value || !expected) return false;
    while (*value && *expected) {
        char a=*value++, b=*expected++;
        if (a>='a'&&a<='z') a=static_cast<char>(a-'a'+'A');
        if (b>='a'&&b<='z') b=static_cast<char>(b-'a'+'A');
        if (a!=b) return false;
    }
    return *value=='\0' && *expected=='\0';
}
inline KeyClickMode key_click_from_setting(const char* value) {
    if (!value) return KeyClickMode::Low;
    if (key_click_setting_is(value,"OFF")) return KeyClickMode::Off;
    if (key_click_setting_is(value,"MID")) return KeyClickMode::Mid;
    if (key_click_setting_is(value,"HIGH")) return KeyClickMode::High;
    if (key_click_setting_is(value,"LOW")) return KeyClickMode::Low;
    // Version 0.85 used timbre names. Preserve audible level on upgrade:
    // SOFT / CLASSIC / SHARP all migrate to the new LOW level.
    if (key_click_setting_is(value,"SOFT") ||
        key_click_setting_is(value,"CLASSIC") ||
        key_click_setting_is(value,"SHARP")) return KeyClickMode::Low;
    return KeyClickMode::Low;
}
class KeyClick {
public:
    void set_mode(KeyClickMode mode) {
        mode_=mode;
        if (mode==KeyClickMode::Off) remaining_=0;
    }
    KeyClickMode mode() const { return mode_; }
    bool active() const { return remaining_ != 0; }
    void cancel() { remaining_=0; }
    bool trigger(bool local_key, bool program_running) {
        if (!local_key || program_running || mode_==KeyClickMode::Off) return false;
        total_=132;
        period_=32;
        switch (mode_) {
        case KeyClickMode::Mid: amplitude_=8500; break;
        case KeyClickMode::High: amplitude_=16000; break;
        default: amplitude_=2800; break; // v0.85 current click = LOW
        }
        remaining_=total_;
        return true;
    }
    void mix(
        std::int16_t* stereo,
        std::size_t frames,
        unsigned output_percent = 100
    ) {
        if (!remaining_) return;
        const int gain = static_cast<int>(std::min(100u, output_percent));
        for (std::size_t i=0; i<frames && remaining_; ++i) {
            const unsigned phase=total_-remaining_;
            const int sign=phase%period_<period_/2 ? 1 : -1;
            const int sample=sign*amplitude_*static_cast<int>(remaining_-1)/
                static_cast<int>(total_) * gain / 100;
            for (int channel=0; channel<2; ++channel) {
                const int value=static_cast<int>(stereo[i*2+channel])+sample;
                stereo[i*2+channel]=static_cast<std::int16_t>(
                    std::max(-32768,std::min(32767,value)));
            }
            --remaining_;
        }
    }
private:
    KeyClickMode mode_=KeyClickMode::Low;
    unsigned total_=0,remaining_=0,period_=1;
    int amplitude_=0;
};
} // namespace rmb::audio
