#include "key_click.hpp"
#include "audio_buffer_policy.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>

int main() {
    using namespace rmb::audio;
    KeyClick click;

    assert(key_click_from_setting(nullptr)==KeyClickMode::Low);
    assert(key_click_from_setting("CLASSIC")==KeyClickMode::Low);
    assert(key_click_from_setting("SOFT")==KeyClickMode::Low);
    assert(key_click_from_setting("SHARP")==KeyClickMode::Low);

    int first_sample[3] = {};
    int level_index = 0;
    for (const auto mode : {KeyClickMode::Off, KeyClickMode::Low,
                            KeyClickMode::Mid, KeyClickMode::High}) {
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

        if (generated) {
            first_sample[level_index++] = std::abs(static_cast<int>(pcm[0]));
            // A foreground source stays intact; mixing never calls stop.
            std::int16_t bgm[512];
            for (auto& sample:bgm) sample=500;
            bool source_active=true;
            assert(click.trigger(true,false));
            click.mix(bgm,256);
            assert(source_active && bgm[0]!=500);
        }
    }

    assert(first_sample[0] > 0);
    assert(first_sample[1] > first_sample[0] * 2);
    assert(first_sample[2] > first_sample[1] + first_sample[0]);

    // WAV buffers use a 25% output ceiling, so a simultaneous key click is
    // halved before mixing and retains its previous 12.5%-ceiling loudness.
    click.set_mode(KeyClickMode::Low);
    std::int16_t full_click[512] = {};
    std::int16_t half_click[512] = {};
    assert(click.trigger(true,false));
    click.mix(full_click,256);
    assert(click.trigger(true,false));
    click.mix(half_click,256,50);
    assert(half_click[0] == full_click[0] / 2);

    // Click mixing is saturating and cannot wrap a loud foreground source.
    click.set_mode(KeyClickMode::High);
    std::int16_t loud_bgm[512];
    for (auto& sample:loud_bgm) sample=30000;
    assert(click.trigger(true,false));
    click.mix(loud_bgm,256);
    assert(loud_bgm[0]==32767);

    for (auto& sample:loud_bgm) sample=-30000;
    assert(click.trigger(true,false));
    click.mix(loud_bgm,256);
    assert(loud_bgm[32]==-32768); // first negative half-wave, left channel

    assert(wav_tail_gain(0,22050)==0);
    assert(wav_tail_gain(110,22050)==256);
    assert(wav_tail_gain(0xffffffffu,22050)==256);
    assert(wav_tail_gain(55,22050)>0 && wav_tail_gain(55,22050)<256);
    std::int16_t stale[8]={1,0,0,0,0,0,0,0};
    assert(has_pcm_samples(stale,8));
    for(auto& sample:stale) sample=0;
    assert(!has_pcm_samples(stale,8)); // No stale buffer after WAV end.
}
