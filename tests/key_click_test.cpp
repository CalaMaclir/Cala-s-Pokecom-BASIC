#include "key_click.hpp"
#include "audio_buffer_policy.hpp"
#include <cassert>
#include <cstdint>

int main() {
    using namespace rmb::audio;
    KeyClick click;
    assert(key_click_from_setting(nullptr)==KeyClickMode::Classic);
    for (const auto mode : {KeyClickMode::Off, KeyClickMode::Soft,
                            KeyClickMode::Classic, KeyClickMode::Sharp}) {
        const auto saved=key_click_name(mode);
        assert(key_click_from_setting(saved)==mode);
        click.set_mode(mode);
        std::int16_t pcm[512] = {};
        assert(!click.trigger(true,true));  // RUN: INKEY/PAUSE/BREAK
        click.mix(pcm,256);
        assert(!has_pcm_samples(pcm,512));
        assert(!click.trigger(false,false)); // USB and Bluetooth input
        const bool generated=click.trigger(true,false); // REPL/menu
        assert(generated==(mode!=KeyClickMode::Off));
        click.mix(pcm,256);
        assert(has_pcm_samples(pcm,512)==generated);
        assert(!click.active());
        // A foreground source stays intact; mixing never calls stop.
        if (generated) {
            std::int16_t bgm[512];
            for (auto& sample:bgm) sample=500;
            bool source_active=true;
            assert(click.trigger(true,false));
            click.mix(bgm,256);
            assert(source_active && bgm[0]!=500);
        }
    }
    assert(wav_tail_gain(0,22050)==0);
    assert(wav_tail_gain(110,22050)==256);
    assert(wav_tail_gain(0xffffffffu,22050)==256);
    assert(wav_tail_gain(55,22050)>0 && wav_tail_gain(55,22050)<256);
    std::int16_t stale[8]={1,0,0,0,0,0,0,0};
    assert(has_pcm_samples(stale,8));
    for(auto& sample:stale) sample=0;
    assert(!has_pcm_samples(stale,8)); // No stale buffer after WAV end.
}
