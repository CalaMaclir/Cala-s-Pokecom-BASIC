#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rmb::audio {
enum class KeyClickMode : std::uint8_t { Off, Soft, Classic, Sharp };
inline const char* key_click_name(KeyClickMode mode) {
    switch (mode) {
    case KeyClickMode::Off: return "OFF";
    case KeyClickMode::Soft: return "SOFT";
    case KeyClickMode::Sharp: return "SHARP";
    default: return "CLASSIC";
    }
}
inline KeyClickMode key_click_from_setting(const char* value) {
    if (!value) return KeyClickMode::Classic;
    if (value[0]=='O' && value[1]=='F' && value[2]=='F' && !value[3])
        return KeyClickMode::Off;
    if (value[0]=='S' && value[1]=='O' && value[2]=='F' && value[3]=='T' && !value[4])
        return KeyClickMode::Soft;
    if (value[0]=='S' && value[1]=='H' && value[2]=='A' && value[3]=='R' &&
        value[4]=='P' && !value[5]) return KeyClickMode::Sharp;
    return KeyClickMode::Classic; // Missing or older configuration.
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
        switch (mode_) {
        case KeyClickMode::Soft: total_=176; period_=44; amplitude_=1700; break;
        case KeyClickMode::Sharp: total_=88; period_=18; amplitude_=4000; break;
        default: total_=132; period_=32; amplitude_=2800; break;
        }
        remaining_=total_;
        return true;
    }
    void mix(std::int16_t* stereo, std::size_t frames) {
        if (!remaining_) return;
        for (std::size_t i=0; i<frames && remaining_; ++i) {
            const unsigned phase=total_-remaining_;
            const int sign=phase%period_<period_/2 ? 1 : -1;
            const int sample=sign*amplitude_*static_cast<int>(remaining_-1)/
                static_cast<int>(total_);
            for (int channel=0; channel<2; ++channel) {
                const int value=static_cast<int>(stereo[i*2+channel])+sample;
                stereo[i*2+channel]=static_cast<std::int16_t>(
                    std::max(-32768,std::min(32767,value)));
            }
            --remaining_;
        }
    }
private:
    KeyClickMode mode_=KeyClickMode::Classic;
    unsigned total_=0,remaining_=0,period_=1;
    int amplitude_=0;
};
} // namespace rmb::audio
