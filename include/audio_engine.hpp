#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::audio {

constexpr std::uint32_t sample_rate = 22050;
constexpr int max_voices = 3;
constexpr std::size_t max_mml_length = 384;

enum class Result : std::uint8_t {
    Ok,
    BadFrequency,
    BadDuration,
    BadMml,
    TooManyVoices
};

enum class WavError : std::uint8_t {
    Ok,
    InvalidRiff,
    UnsupportedFormat,
    Truncated
};

struct WavInfo {
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t data_size = 0;
};

using ReadAt = bool (*)(void* context, std::uint32_t offset, void* data, std::size_t size);

WavError parse_wav(
    ReadAt read_at,
    void* context,
    std::uint32_t size,
    WavInfo& info
);

class Engine {
public:
    void init();
    void stop();
    void pause();
    void resume();
    bool active() const;
    bool paused() const;

    void set_volume(int percent);
    int volume() const;

    Result start_beep(int frequency_hz, int duration_ms);
    Result start_mml(const char* const* voices, int count);

    void render(std::int16_t* stereo, std::size_t frames);

private:
    enum class Mode : std::uint8_t { Idle, Beep, Mml };

    struct Voice {
        char text[max_mml_length + 1] = {};
        std::size_t position = 0;
        int octave = 4;
        int default_length = 4;
        int tempo = 120;
        int level = 15;
        std::uint32_t phase = 0;
        std::uint32_t phase_step = 0;
        std::uint32_t remaining = 0;
        bool sounding = false;
        bool finished = false;
    };

    Mode mode_ = Mode::Idle;
    bool paused_ = false;
    int volume_ = 70;
    int voice_count_ = 0;
    Voice voices_[max_voices] = {};
    std::uint32_t beep_phase_ = 0;
    std::uint32_t beep_step_ = 0;
    std::uint32_t beep_remaining_ = 0;

    static bool next_event(Voice& voice);
    static bool validate_mml(const char* text);
};

} // namespace rmb::audio
