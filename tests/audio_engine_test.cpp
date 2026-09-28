#include "audio_engine.hpp"
#include "audio_buffer_policy.hpp"
#include "audio_file_policy.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#include <string>

namespace {

void put16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void fourcc(std::vector<std::uint8_t>& data, const char* text) {
    data.insert(data.end(), text, text + 4);
}

std::vector<std::uint8_t> wav(
    int bits,
    int channels,
    int rate,
    bool unknown_chunk = false,
    int format = 1
) {
    std::vector<std::uint8_t> body;
    fourcc(body, "WAVE");
    if (unknown_chunk) {
        fourcc(body, "JUNK");
        put32(body, 3);
        body.push_back(1);
        body.push_back(2);
        body.push_back(3);
        body.push_back(0);
    }
    fourcc(body, "fmt ");
    put32(body, 16);
    put16(body, static_cast<std::uint16_t>(format));
    put16(body, static_cast<std::uint16_t>(channels));
    put32(body, static_cast<std::uint32_t>(rate));
    const int align = channels * bits / 8;
    put32(body, static_cast<std::uint32_t>(rate * align));
    put16(body, static_cast<std::uint16_t>(align));
    put16(body, static_cast<std::uint16_t>(bits));
    fourcc(body, "data");
    put32(body, static_cast<std::uint32_t>(align * 2));
    for (int i = 0; i < align * 2; ++i)
        body.push_back(static_cast<std::uint8_t>(i * 17));

    std::vector<std::uint8_t> result;
    fourcc(result, "RIFF");
    put32(result, static_cast<std::uint32_t>(body.size()));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

bool read_at(void* context, std::uint32_t offset, void* output, std::size_t size) {
    auto& data = *static_cast<std::vector<std::uint8_t>*>(context);
    if (static_cast<std::uint64_t>(offset) + size > data.size())
        return false;
    std::memcpy(output, data.data() + offset, size);
    return true;
}

std::string mml_of_length(std::size_t length, char note = 'C') {
    const std::string prefix = "T400O4L32";
    assert(length >= prefix.size());
    return prefix + std::string(length - prefix.size(), note);
}

struct SequentialSource {
    const std::vector<std::uint8_t>* data = nullptr;
    std::size_t position = 0;
    std::size_t calls = 0;
};

std::size_t sequential_read(
    void* context,
    void* output,
    std::size_t size
) {
    auto& source = *static_cast<SequentialSource*>(context);
    ++source.calls;
    const std::size_t available =
        source.data->size() - source.position;
    const std::size_t take = std::min(size, available);
    std::memcpy(output, source.data->data() + source.position, take);
    source.position += take;
    return take;
}

} // namespace

int main() {
    using namespace rmb::audio;

    {
        BufferRingOrder<6> order;
        for (std::size_t i = 0; i < 6; ++i) {
            assert(order.producer_index() == i);
            order.advance_producer();
        }
        assert(order.producer_index() == 0);

        for (std::size_t i = 0; i < 6; ++i) {
            assert(order.consumer_index() == i);
            order.advance_consumer();
        }
        assert(order.consumer_index() == 0);

        // Regression for the six-buffer WAV failure: after buffer 0 has
        // completed and is refilled, the next playback slot is still 1,
        // never the newly refilled 0.
        order.reset();
        for (int i = 0; i < 6; ++i) order.advance_producer();
        assert(order.producer_index() == 0);
        assert(order.consumer_index() == 0);
        order.advance_consumer(); // start buffer 0
        assert(order.consumer_index() == 1);
        order.advance_producer(); // refill completed buffer 0
        assert(order.producer_index() == 1);
        assert(order.consumer_index() == 1);

        // Repeated wraps keep producer and consumer FIFO positions
        // independent and stable.
        for (int wrap = 0; wrap < 20; ++wrap) {
            const std::size_t producer = order.producer_index();
            order.advance_producer();
            assert(order.producer_index() == (producer + 1u) % 6u);
            const std::size_t consumer = order.consumer_index();
            order.advance_consumer();
            assert(order.consumer_index() == (consumer + 1u) % 6u);
        }
    }

    {
        std::uint32_t remaining = 8;
        assert(consume_wav_frame(remaining, 4, 4));
        assert(remaining == 4);
        assert(!consume_wav_frame(remaining, 4, 2));
        assert(remaining == 0);
        remaining = 3;
        assert(!consume_wav_frame(remaining, 4, 0));
        assert(remaining == 0);

        const std::uint8_t pcm8_min[] = {0};
        const std::uint8_t pcm8_zero[] = {128};
        const std::uint8_t pcm8_max[] = {255};
        assert(decode_pcm_sample(pcm8_min, 8) == -32768);
        assert(decode_pcm_sample(pcm8_zero, 8) == 0);
        assert(decode_pcm_sample(pcm8_max, 8) == 32512);

        const std::uint8_t pcm16_zero[] = {0x00, 0x00};
        const std::uint8_t pcm16_max[] = {0xff, 0x7f};
        const std::uint8_t pcm16_min[] = {0x00, 0x80};
        const std::uint8_t pcm16_minus_one[] = {0xff, 0xff};
        assert(decode_pcm_sample(pcm16_zero, 16) == 0);
        assert(decode_pcm_sample(pcm16_max, 16) == 32767);
        assert(decode_pcm_sample(pcm16_min, 16) == -32768);
        assert(decode_pcm_sample(pcm16_minus_one, 16) == -1);

        WavResamplePhase phase;
        assert(phase.advance(11025, sample_rate) == 0);
        assert(phase.advance(11025, sample_rate) == 1);
        assert(phase.phase() == 0);
        assert(phase.advance(22050, sample_rate) == 1);
        assert(phase.phase() == 0);
        assert(phase.advance(44100, sample_rate) == 2);
        assert(phase.phase() == 0);
        phase.reset();
        assert(phase.phase() == 0);
    }

    {
        assert(pwm_carrier_rate == 44100u);
        assert(pwm_repeats_per_sample == 2u);

        struct TimingCase {
            std::uint32_t clock_hz;
            std::uint16_t wrap;
            std::uint32_t actual_hz;
        };
        for (const TimingCase item : {
                 TimingCase{75000000u, 1699u, 44117u},
                 TimingCase{100000000u, 2266u, 44111u},
                 TimingCase{150000000u, 3400u, 44104u},
                 TimingCase{200000000u, 4534u, 44101u}}) {
            const PwmTiming timing = pwm_timing(item.clock_hz);
            assert(timing.wrap == item.wrap);
            assert(timing.actual_carrier_hz == item.actual_hz);
            const std::uint64_t error =
                timing.actual_carrier_hz >= pwm_carrier_rate
                    ? timing.actual_carrier_hz - pwm_carrier_rate
                    : pwm_carrier_rate - timing.actual_carrier_hz;
            assert(error * 1000u < pwm_carrier_rate);
        }

        const PwmTiming timing = pwm_timing(150000000u);
        assert(picocalc_non_wav_output_gain_per_mille == 250u);
        assert(picocalc_wav_output_gain_per_mille == 500u);
        assert(apply_audio_volume(-32768, 10) == -3276);
        assert(apply_audio_volume(32767, 10) == 3276);
        assert(apply_audio_volume(-32768, 50) == -16384);
        assert(apply_audio_volume(32767, 50) == 16383);
        assert(apply_audio_volume(-32768, 100) == -32768);
        assert(apply_audio_volume(32767, 100) == 32767);
        assert(apply_audio_volume(-32768, 110) == -32768);
        assert(apply_audio_volume(32767, 110) == 32767);
        assert(apply_picocalc_output_gain(-32768, 125) == -4096);
        assert(apply_picocalc_output_gain(-1, 125) == 0);
        assert(apply_picocalc_output_gain(0, 125) == 0);
        assert(apply_picocalc_output_gain(1, 125) == 0);
        assert(apply_picocalc_output_gain(32767, 125) == 4095);
        assert(apply_picocalc_output_gain(-32768, 250) == -8192);
        assert(apply_picocalc_output_gain(32767, 250) == 8191);
        assert(pcm_to_pwm_code(-32768) == 0u);
        assert(pcm_to_pwm_code(0) == 2048u);
        assert(pcm_to_pwm_code(32767) == 4095u);
        assert(pwm_level(-32768, timing.wrap) == 0u);
        assert(pwm_level(32767, timing.wrap) == 3399u);
        assert(pwm_level(0, timing.wrap) == 1700u);
        assert(pwm_level(-4096, timing.wrap) == 1487u);
        assert(pwm_level(4095, timing.wrap) == 1911u);
        assert(pack_pwm_frame(-32768, 32767, timing.wrap) ==
               (static_cast<std::uint32_t>(1275u) |
                (static_cast<std::uint32_t>(2124u) << 16u)));
        assert(pack_pwm_frame(
                   -32768, 32767, timing.wrap,
                   picocalc_wav_output_gain_per_mille) ==
               (static_cast<std::uint32_t>(850u) |
                (static_cast<std::uint32_t>(2549u) << 16u)));

        // WAV/MP3 50% x the new 50% ceiling equals PLAY at its full
        // 25% ceiling. Master volume is layered later by the backend.
        const std::int16_t wav_left =
            apply_audio_volume(-32768, 50);
        const std::int16_t wav_right =
            apply_audio_volume(32767, 50);
        assert(pack_pwm_frame(
                   wav_left, wav_right, timing.wrap,
                   picocalc_wav_output_gain_per_mille) ==
               pack_pwm_frame(-32768, 32767, timing.wrap));
        assert(apply_audio_volume(
                   apply_audio_volume(20000, 50), 70) == 7000);
        assert(playable_audio_filename("TRACK.WAV"));
        assert(playable_audio_filename("track.mp3"));
        assert(!playable_audio_filename("track.bas"));
        const unsigned char riff[] = {
            'R','I','F','F',0,0,0,0,'W','A','V','E'
        };
        assert(wav_file_header(riff, sizeof(riff)));

        // A 22.05 kHz PCM frame is held for two 44.1 kHz PWM periods.
        const std::uint32_t packed =
            pack_pwm_frame(-1234, 5678, timing.wrap);
        const std::uint32_t repeated[pwm_repeats_per_sample] = {
            packed, packed
        };
        assert(repeated[0] == repeated[1]);

        // The direct diagnostic sequence is independent of WAV parsing:
        // low/medium/high stereo, then left-only and right-only.
        {
            const std::uint32_t block =
                diagnostic_sample_rate + diagnostic_gap_frames;
            DiagnosticToneStep step = diagnostic_tone_step(0);
            assert(step.active && step.left && step.right);
            assert(step.amplitude == 4096);

            step = diagnostic_tone_step(diagnostic_sample_rate);
            assert(step.active && !step.left && !step.right);
            assert(step.amplitude == 0);

            step = diagnostic_tone_step(block);
            assert(step.active && step.left && step.right);
            assert(step.amplitude == 8192);

            step = diagnostic_tone_step(block * 2u);
            assert(step.active && step.left && step.right);
            assert(step.amplitude == 16384);

            step = diagnostic_tone_step(block * 3u);
            assert(step.active && step.left && !step.right);
            assert(step.amplitude == 8192);

            step = diagnostic_tone_step(block * 4u);
            assert(step.active && !step.left && step.right);
            assert(step.amplitude == 8192);

            step = diagnostic_tone_step(diagnostic_total_frames);
            assert(!step.active);
        }

        // Prefix of the real 22050 Hz / 16-bit stereo AUTORUN.WAV used in
        // hardware diagnosis. This locks byte order and L/R frame stepping.
        const std::uint8_t autorun_prefix[] = {
            0xf1, 0xff, 0xdd, 0xff,
            0x3f, 0x00, 0x03, 0x01,
            0x18, 0x01, 0xc9, 0x02
        };
        assert(decode_pcm_sample(autorun_prefix + 0, 16) == -15);
        assert(decode_pcm_sample(autorun_prefix + 2, 16) == -35);
        assert(decode_pcm_sample(autorun_prefix + 4, 16) == 63);
        assert(decode_pcm_sample(autorun_prefix + 6, 16) == 259);
        assert(decode_pcm_sample(autorun_prefix + 8, 16) == 280);
        assert(decode_pcm_sample(autorun_prefix + 10, 16) == 713);

        assert(record_audio_underrun(0, false) == 0);
        assert(record_audio_underrun(0, true) == 1);
        assert(record_audio_underrun(9, true) == 10);
        assert(record_audio_underrun(0xffffffffu, true) == 0xffffffffu);

        // A 16-bit stereo second used to make 22050 fread() calls. The
        // 4 KiB read-ahead path reduces that to 22 sequential reads.
        std::vector<std::uint8_t> pcm(22050u * 4u);
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::uint8_t>(i);
        SequentialSource source{&pcm};
        WavReadAheadBuffer<4096> read_ahead;
        read_ahead.reset(
            sequential_read, &source,
            static_cast<std::uint32_t>(pcm.size()));
        std::uint8_t frame[4] = {};
        for (std::size_t i = 0; i < 22050u; ++i) {
            assert(read_ahead.read_exact(frame, sizeof(frame)));
            const std::size_t offset = i * sizeof(frame);
            for (std::size_t byte = 0; byte < sizeof(frame); ++byte)
                assert(frame[byte] == pcm[offset + byte]);
        }
        assert(read_ahead.remaining() == 0);
        assert(!read_ahead.failed());
        assert(source.calls == 22);

        std::vector<std::uint8_t> short_pcm(60, 0x5a);
        SequentialSource short_source{&short_pcm};
        WavReadAheadBuffer<32> short_read_ahead;
        short_read_ahead.reset(sequential_read, &short_source, 80);
        std::uint8_t block[20] = {};
        assert(short_read_ahead.read_exact(block, sizeof(block)));
        assert(short_read_ahead.read_exact(block, sizeof(block)));
        assert(short_read_ahead.read_exact(block, sizeof(block)));
        assert(!short_read_ahead.read_exact(block, sizeof(block)));
        assert(short_read_ahead.failed());
        assert(short_read_ahead.remaining() == 0);
    }

    Engine engine;
    engine.init();
    assert(engine.volume() == 70);
    engine.set_volume(130);
    assert(engine.volume() == 100);
    engine.set_volume(-1);
    assert(engine.volume() == 0);
    engine.set_volume(70);

    assert(engine.start_beep(10, 100) == Result::BadFrequency);
    assert(engine.start_beep(880, 0) == Result::BadDuration);
    assert(engine.start_beep(880, 1) == Result::Ok);
    assert(engine.active());
    std::int16_t samples[256 * 2] = {};
    engine.render(samples, 256);
    assert(!engine.active());
    assert(engine.start_beep(440, 100) == Result::Ok);
    engine.stop();
    assert(!engine.active());
    assert(engine.start_beep(660, 1) == Result::Ok);
    engine.render(samples, 256);
    assert(!engine.active());

    const char* one[] = {"T120 O4 L8 C D# E- F G A B > C. R"};
    assert(engine.start_mml(one, 1) == Result::Ok);
    engine.pause();
    assert(engine.active() && engine.paused());
    std::memset(samples, 1, sizeof(samples));
    engine.render(samples, 32);
    for (int i = 0; i < 64; ++i) assert(samples[i] == 0);
    engine.resume();
    assert(!engine.paused());
    engine.stop();
    assert(!engine.active());

    const char* three[] = {
        "T160O5L8 CDEFGAB>C",
        "T160O4L8 EFGAB>CD",
        "T160O3L4 C G C G V8 R."
    };
    assert(engine.start_mml(three, 3) == Result::Ok);
    engine.render(samples, 64);
    assert(engine.active());
    engine.stop();

    // Version 0.86: one MML voice may use the SD long-line capacity instead
    // of the former 384-character engine limit. The engine must also remain
    // active until the longest voice has actually sounded its final note.
    std::string long_voice = "T400O4L32 ";
    for (int i = 0; i < 700; ++i) long_voice += "C";
    std::string mid_voice = "T400O4L32 ";
    for (int i = 0; i < 350; ++i) mid_voice += "E";
    std::string short_voice = "T400O4L32 ";
    for (int i = 0; i < 175; ++i) short_voice += "G";
    const char* long_chord[] = {
        long_voice.c_str(), mid_voice.c_str(), short_voice.c_str()
    };
    assert(long_voice.size() > 384);
    assert(engine.start_mml(long_chord, 3) == Result::Ok);
    std::uint64_t rendered_frames = 0;
    while (engine.active()) {
        engine.render(samples, 256);
        rendered_frames += 256;
        assert(rendered_frames < 1000000);
    }
    // 700 x L32 at T400 is about 289k frames. A premature end caused by the
    // old MML limit would fall far below this threshold.
    assert(rendered_frames > 280000);

    // Accept the complete documented MML length range and reject the first
    // byte beyond it. Three near-limit voices must remain independently
    // valid at the same time.
    for (const std::size_t length : {500u, 1000u, 2000u, 2047u}) {
        std::string boundary = mml_of_length(length);
        const char* voice[] = {boundary.c_str()};
        assert(engine.start_mml(voice, 1) == Result::Ok);
        engine.stop();
    }
    std::string too_long = mml_of_length(2048);
    const char* oversized[] = {too_long.c_str()};
    assert(engine.start_mml(oversized, 1) == Result::BadMml);

    std::string near_limit_c = mml_of_length(2000, 'C');
    std::string near_limit_e = mml_of_length(2000, 'E');
    std::string near_limit_g = mml_of_length(2000, 'G');
    const char* near_limit_chord[] = {
        near_limit_c.c_str(), near_limit_e.c_str(), near_limit_g.c_str()
    };
    assert(engine.start_mml(near_limit_chord, 3) == Result::Ok);
    engine.stop();

    const char* parser_boundaries[] = {
        "T32 O0 L1 V0 C# R. T400 O8 L32 V15 B C+ D- O1 < C > C"
    };
    assert(engine.start_mml(parser_boundaries, 1) == Result::Ok);
    engine.stop();

    // At T400/L32 each note is exactly floor(22050*60*4/(400*32))
    // samples. The longest voice owns completion and its final sample must
    // be emitted before the one terminal idle frame.
    const char* exact_voices[] = {
        "T400O4L32C", "T400O4L32CC", "T400O4L32CCC"
    };
    engine.set_volume(100);
    assert(engine.start_mml(exact_voices, 3) == Result::Ok);
    constexpr std::uint64_t note_frames =
        (static_cast<std::uint64_t>(sample_rate) * 60u * 4u) / (400u * 32u);
    constexpr std::uint64_t expected_frames = note_frames * 3u;
    std::uint64_t render_calls = 0;
    std::uint64_t last_audible = 0;
    while (engine.active()) {
        std::int16_t frame[2] = {};
        engine.render(frame, 1);
        if (frame[0] != 0 || frame[1] != 0) last_audible = render_calls;
        ++render_calls;
        assert(render_calls < 10000);
    }
    assert(render_calls == expected_frames + 1u);
    assert(last_audible == expected_frames - 1u);

    // The Stage 5 mixer shares about 26k of PCM headroom across active
    // voices, so one voice is louder without clipping a three-voice chord.
    const char* solo_loud[] = {"T400O4L32V15C"};
    assert(engine.start_mml(solo_loud, 1) == Result::Ok);
    std::int16_t solo_frame[2] = {};
    engine.render(solo_frame, 1);
    assert(solo_frame[0] == -25995);
    assert(solo_frame[1] == -25995);
    engine.stop();

    // Three maximum-level square waves use the same 25,995 peak.
    const char* loud[] = {
        "T400O4L32V15C", "T400O4L32V15C", "T400O4L32V15C"
    };
    assert(engine.start_mml(loud, 3) == Result::Ok);
    std::int16_t loud_frame[2] = {};
    engine.render(loud_frame, 1);
    assert(loud_frame[0] == -25995);
    assert(loud_frame[1] == -25995);
    engine.stop();
    engine.set_volume(70);

    for (const char* invalid_mml :
         {"T31C", "T401C", "O9C", "L3C", "V16C", "X", "T", "O", "L", "V"}) {
        const char* invalid_voice[] = {invalid_mml};
        assert(engine.start_mml(invalid_voice, 1) == Result::BadMml);
    }

    const char* bad[] = {"T0 O9 L3 X"};
    assert(engine.start_mml(bad, 1) == Result::BadMml);
    const char* four[] = {"C", "E", "G", "B"};
    assert(engine.start_mml(four, 4) == Result::TooManyVoices);

    for (int rate : {11025, 22050, 44100}) {
        for (auto format : std::vector<std::pair<int, int>>{
                 {8, 1}, {8, 2}, {16, 1}, {16, 2}}) {
            auto data = wav(format.first, format.second, rate);
            WavInfo info;
            assert(parse_wav(
                read_at, &data, static_cast<std::uint32_t>(data.size()), info
            ) == WavError::Ok);
            assert(info.bits_per_sample == format.first);
            assert(info.channels == format.second);
            assert(info.sample_rate == static_cast<std::uint32_t>(rate));
            assert(info.data_size > 0);
        }
    }

    auto with_unknown = wav(16, 2, 22050, true);
    WavInfo info;
    assert(parse_wav(
        read_at, &with_unknown,
        static_cast<std::uint32_t>(with_unknown.size()), info
    ) == WavError::Ok);

    auto invalid = wav(16, 1, 22050);
    invalid[0] = 'N';
    assert(parse_wav(
        read_at, &invalid, static_cast<std::uint32_t>(invalid.size()), info
    ) == WavError::InvalidRiff);

    auto non_pcm = wav(16, 1, 22050, false, 3);
    assert(parse_wav(
        read_at, &non_pcm, static_cast<std::uint32_t>(non_pcm.size()), info
    ) == WavError::UnsupportedFormat);

    auto truncated = wav(16, 1, 22050);
    truncated.pop_back();
    assert(parse_wav(
        read_at, &truncated,
        static_cast<std::uint32_t>(truncated.size()), info
    ) == WavError::Truncated);
}
