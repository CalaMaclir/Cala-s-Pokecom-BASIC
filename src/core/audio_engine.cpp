#include "audio_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rmb::audio {
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool fourcc(const std::uint8_t* p, const char* text) {
    return std::memcmp(p, text, 4) == 0;
}

int upper(char c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

void skip_spaces(const char* text, std::size_t& p) {
    while (text[p] == ' ' || text[p] == '\t' ||
           text[p] == '\r' || text[p] == '\n') ++p;
}

bool number(const char* text, std::size_t& p, int& value) {
    skip_spaces(text, p);
    if (text[p] < '0' || text[p] > '9') return false;
    value = 0;
    while (text[p] >= '0' && text[p] <= '9') {
        value = value * 10 + (text[p] - '0');
        ++p;
        if (value > 10000) return false;
    }
    return true;
}

bool valid_length(int value) {
    return value == 1 || value == 2 || value == 4 ||
           value == 8 || value == 16 || value == 32;
}

std::uint32_t phase_step(float frequency) {
    const double scale = 4294967296.0 / static_cast<double>(sample_rate);
    return static_cast<std::uint32_t>(frequency * scale);
}

} // namespace

WavError parse_wav(
    ReadAt read_at,
    void* context,
    std::uint32_t size,
    WavInfo& info
) {
    info = {};
    if (!read_at || size < 12) return WavError::Truncated;

    std::uint8_t header[12] = {};
    if (!read_at(context, 0, header, sizeof(header)))
        return WavError::Truncated;
    if (!fourcc(header, "RIFF") || !fourcc(header + 8, "WAVE"))
        return WavError::InvalidRiff;

    const std::uint32_t riff_size = le32(header + 4);
    if (riff_size < 4 || static_cast<std::uint64_t>(riff_size) + 8u > size)
        return WavError::Truncated;

    bool have_fmt = false;
    bool have_data = false;
    std::uint32_t offset = 12;
    const std::uint32_t limit = std::min<std::uint32_t>(size, riff_size + 8u);

    while (offset + 8u <= limit) {
        std::uint8_t chunk[8] = {};
        if (!read_at(context, offset, chunk, sizeof(chunk)))
            return WavError::Truncated;
        const std::uint32_t chunk_size = le32(chunk + 4);
        const std::uint64_t data_end =
            static_cast<std::uint64_t>(offset) + 8u + chunk_size;
        if (data_end > limit) return WavError::Truncated;

        if (fourcc(chunk, "fmt ")) {
            if (chunk_size < 16) return WavError::UnsupportedFormat;
            std::uint8_t fmt[16] = {};
            if (!read_at(context, offset + 8u, fmt, sizeof(fmt)))
                return WavError::Truncated;
            const std::uint16_t format = le16(fmt);
            const std::uint16_t channels = le16(fmt + 2);
            const std::uint32_t rate = le32(fmt + 4);
            const std::uint16_t block_align = le16(fmt + 12);
            const std::uint16_t bits = le16(fmt + 14);
            const std::uint16_t expected_align =
                static_cast<std::uint16_t>(channels * (bits / 8u));
            if (format != 1 || (channels != 1 && channels != 2) ||
                (bits != 8 && bits != 16) ||
                (rate != 11025 && rate != 22050 && rate != 44100) ||
                block_align != expected_align) {
                return WavError::UnsupportedFormat;
            }
            info.channels = channels;
            info.bits_per_sample = bits;
            info.sample_rate = rate;
            have_fmt = true;
        } else if (fourcc(chunk, "data")) {
            info.data_offset = offset + 8u;
            info.data_size = chunk_size;
            have_data = true;
        }

        const std::uint64_t next =
            static_cast<std::uint64_t>(offset) + 8u +
            ((static_cast<std::uint64_t>(chunk_size) + 1u) & ~1u);
        if (next > limit) return WavError::Truncated;
        offset = static_cast<std::uint32_t>(next);
    }

    if (!have_fmt || !have_data) return WavError::InvalidRiff;
    const std::uint32_t frame_size =
        info.channels * (info.bits_per_sample / 8u);
    if (frame_size == 0 || info.data_size % frame_size != 0)
        return WavError::Truncated;
    return WavError::Ok;
}

void Engine::init() {
    stop();
    volume_ = 70;
}

void Engine::stop() {
    mode_ = Mode::Idle;
    paused_ = false;
    voice_count_ = 0;
    beep_remaining_ = 0;
    for (auto& voice : voices_) voice = Voice{};
}

void Engine::pause() {
    if (mode_ != Mode::Idle) paused_ = true;
}

void Engine::resume() {
    if (mode_ != Mode::Idle) paused_ = false;
}

bool Engine::active() const { return mode_ != Mode::Idle; }
bool Engine::paused() const { return paused_; }

void Engine::set_volume(int percent) {
    volume_ = std::max(0, std::min(100, percent));
}

int Engine::volume() const { return volume_; }

Result Engine::start_beep(int frequency_hz, int duration_ms) {
    if (frequency_hz < 20 || frequency_hz > 20000)
        return Result::BadFrequency;
    if (duration_ms < 1 || duration_ms > 60000)
        return Result::BadDuration;

    stop();
    mode_ = Mode::Beep;
    beep_step_ = phase_step(static_cast<float>(frequency_hz));
    beep_remaining_ = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(sample_rate) *
         static_cast<std::uint32_t>(duration_ms)) / 1000u
    );
    if (beep_remaining_ == 0) beep_remaining_ = 1;
    return Result::Ok;
}

bool Engine::next_event(Voice& voice) {
    const char* text = voice.text;
    while (true) {
        skip_spaces(text, voice.position);
        const int command = upper(text[voice.position]);
        if (command == 0) {
            voice.finished = true;
            voice.sounding = false;
            voice.remaining = 0;
            return true;
        }

        if (command == 'T' || command == 'O' ||
            command == 'L' || command == 'V') {
            ++voice.position;
            int value = 0;
            if (!number(text, voice.position, value)) return false;
            if (command == 'T') {
                if (value < 32 || value > 400) return false;
                voice.tempo = value;
            } else if (command == 'O') {
                if (value < 0 || value > 8) return false;
                voice.octave = value;
            } else if (command == 'L') {
                if (!valid_length(value)) return false;
                voice.default_length = value;
            } else {
                if (value < 0 || value > 15) return false;
                voice.level = value;
            }
            continue;
        }

        if (command == '<' || command == '>') {
            ++voice.position;
            voice.octave += command == '>' ? 1 : -1;
            if (voice.octave < 0 || voice.octave > 8) return false;
            continue;
        }

        int semitone = -1;
        switch (command) {
        case 'C': semitone = 0; break;
        case 'D': semitone = 2; break;
        case 'E': semitone = 4; break;
        case 'F': semitone = 5; break;
        case 'G': semitone = 7; break;
        case 'A': semitone = 9; break;
        case 'B': semitone = 11; break;
        case 'R': semitone = -2; break;
        default: return false;
        }
        ++voice.position;

        if (semitone >= 0) {
            const char accidental = text[voice.position];
            if (accidental == '#' || accidental == '+') {
                ++semitone;
                ++voice.position;
            } else if (accidental == '-') {
                --semitone;
                ++voice.position;
            }
        }

        int length = voice.default_length;
        std::size_t before_length = voice.position;
        int explicit_length = 0;
        if (number(text, voice.position, explicit_length)) {
            if (!valid_length(explicit_length)) return false;
            length = explicit_length;
        } else {
            voice.position = before_length;
        }

        skip_spaces(text, voice.position);
        bool dotted = false;
        if (text[voice.position] == '.') {
            dotted = true;
            ++voice.position;
        }

        std::uint64_t samples =
            (static_cast<std::uint64_t>(sample_rate) * 60u * 4u) /
            (static_cast<std::uint32_t>(voice.tempo) *
             static_cast<std::uint32_t>(length));
        if (dotted) samples = samples * 3u / 2u;
        voice.remaining = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1u, samples)
        );
        voice.sounding = semitone >= 0 && voice.level > 0;
        if (semitone >= 0) {
            const int midi = (voice.octave + 1) * 12 + semitone;
            const float frequency =
                440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
            voice.phase_step = phase_step(frequency);
        } else {
            voice.phase_step = 0;
        }
        return true;
    }
}

bool Engine::validate_mml(const char* text) {
    if (!text || std::strlen(text) > max_mml_length) return false;
    Voice voice;
    std::snprintf(voice.text, sizeof(voice.text), "%s", text);
    while (!voice.finished) {
        if (!next_event(voice)) return false;
        voice.remaining = 0;
    }
    return true;
}

Result Engine::start_mml(const char* const* voices, int count) {
    if (!voices || count < 1 || count > max_voices)
        return count > max_voices ? Result::TooManyVoices : Result::BadMml;
    for (int i = 0; i < count; ++i) {
        if (!validate_mml(voices[i])) return Result::BadMml;
    }

    stop();
    mode_ = Mode::Mml;
    voice_count_ = count;
    for (int i = 0; i < count; ++i) {
        std::snprintf(
            voices_[i].text,
            sizeof(voices_[i].text),
            "%s",
            voices[i]
        );
        if (!next_event(voices_[i])) {
            stop();
            return Result::BadMml;
        }
    }
    return Result::Ok;
}

void Engine::render(std::int16_t* stereo, std::size_t frames) {
    if (!stereo) return;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int mixed = 0;
        if (mode_ != Mode::Idle && !paused_) {
            if (mode_ == Mode::Beep) {
                if (beep_remaining_ > 0) {
                    mixed = (beep_phase_ & 0x80000000u) ? 10000 : -10000;
                    beep_phase_ += beep_step_;
                    --beep_remaining_;
                }
                if (beep_remaining_ == 0) mode_ = Mode::Idle;
            } else {
                bool any = false;
                for (int i = 0; i < voice_count_; ++i) {
                    Voice& voice = voices_[i];
                    while (!voice.finished && voice.remaining == 0) {
                        if (!next_event(voice)) {
                            voice.finished = true;
                            break;
                        }
                    }
                    if (voice.finished) continue;
                    any = true;
                    if (voice.sounding) {
                        const int amplitude = voice.level * 650;
                        mixed += (voice.phase & 0x80000000u)
                            ? amplitude : -amplitude;
                        voice.phase += voice.phase_step;
                    }
                    if (voice.remaining > 0) --voice.remaining;
                }
                if (!any) mode_ = Mode::Idle;
            }
        }
        mixed = mixed * volume_ / 100;
        mixed = std::max(-32768, std::min(32767, mixed));
        stereo[frame * 2] = static_cast<std::int16_t>(mixed);
        stereo[frame * 2 + 1] = static_cast<std::int16_t>(mixed);
    }
}

} // namespace rmb::audio
