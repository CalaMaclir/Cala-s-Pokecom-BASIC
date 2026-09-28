#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rmb::audio {
inline unsigned wav_tail_gain(std::uint32_t remaining_frames,
                               std::uint32_t sample_rate) {
    const std::uint32_t tail = std::max<std::uint32_t>(1u, sample_rate / 200u);
    return remaining_frames >= tail ? 256u :
        static_cast<unsigned>(remaining_frames * 256u / tail);
}
inline bool has_pcm_samples(const std::int16_t* pcm, std::size_t count) {
    for (std::size_t i=0; i<count; ++i) if (pcm[i] != 0) return true;
    return false;
}

inline bool consume_wav_frame(
    std::uint32_t& remaining,
    std::uint32_t frame_bytes,
    std::size_t bytes_read
) {
    if (frame_bytes == 0 || remaining < frame_bytes ||
        bytes_read != frame_bytes) {
        remaining = 0;
        return false;
    }
    remaining -= frame_bytes;
    return true;
}

inline std::int16_t decode_pcm_sample(
    const std::uint8_t* data,
    std::uint16_t bits_per_sample
) {
    if (bits_per_sample == 8) {
        // Multiplication is defined for negative values; left-shifting a
        // negative signed value is undefined in C++.
        return static_cast<std::int16_t>(
            (static_cast<int>(data[0]) - 128) * 256
        );
    }
    const std::uint16_t raw =
        static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8);
    return static_cast<std::int16_t>(
        raw >= 0x8000u ? static_cast<int>(raw) - 0x10000
                       : static_cast<int>(raw)
    );
}

class WavResamplePhase {
public:
    void reset() { phase_ = 0; }

    unsigned advance(
        std::uint32_t input_rate,
        std::uint32_t output_rate
    ) {
        phase_ += input_rate;
        unsigned input_frames = 0;
        while (phase_ >= output_rate) {
            phase_ -= output_rate;
            ++input_frames;
        }
        return input_frames;
    }

    std::uint32_t phase() const { return phase_; }

private:
    std::uint32_t phase_ = 0;
};

constexpr std::uint32_t pwm_carrier_rate = 44100u;
constexpr std::uint32_t pwm_repeats_per_sample = 2u;

struct PwmTiming {
    std::uint16_t wrap = 0;
    std::uint32_t actual_carrier_hz = 0;
};

inline PwmTiming pwm_timing(
    std::uint32_t clock_hz,
    std::uint32_t carrier_hz = pwm_carrier_rate
) {
    PwmTiming timing;
    if (clock_hz == 0 || carrier_hz == 0) return timing;

    // Use an integer divider of one and the largest complete PWM period that
    // does not exceed the requested carrier period. This keeps the wrap
    // calculation deterministic across all supported CPU clocks.
    std::uint64_t period =
        static_cast<std::uint64_t>(clock_hz) / carrier_hz;
    period = std::max<std::uint64_t>(
        2u, std::min<std::uint64_t>(65536u, period));
    timing.wrap = static_cast<std::uint16_t>(period - 1u);
    timing.actual_carrier_hz =
        static_cast<std::uint32_t>(clock_hz / period);
    return timing;
}

constexpr std::uint32_t pwm_code_scale = 4096u;

// PicoCalc-specific output ceilings. BEEP/PLAY use 25% at full source level.
// WAV/MP3 use 50% because their independently adjustable source volume and
// master volume provide the user-facing safety controls.
constexpr std::uint32_t picocalc_non_wav_output_gain_per_mille = 250u;
constexpr std::uint32_t picocalc_wav_output_gain_per_mille = 500u;

inline std::int16_t apply_audio_volume(
    std::int16_t sample,
    std::uint32_t volume_percent
) {
    const std::uint32_t volume =
        std::min<std::uint32_t>(100u, volume_percent);
    return static_cast<std::int16_t>(
        static_cast<std::int32_t>(sample) *
        static_cast<std::int32_t>(volume) / 100);
}

inline std::int16_t apply_picocalc_output_gain(
    std::int16_t sample,
    std::uint32_t gain_per_mille
) {
    const std::uint32_t gain =
        std::min<std::uint32_t>(1000u, gain_per_mille);
    return static_cast<std::int16_t>(
        static_cast<std::int32_t>(sample) *
        static_cast<std::int32_t>(gain) / 1000);
}

inline std::uint16_t pcm_to_pwm_code(std::int16_t sample) {
    const std::uint32_t biased =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(sample) + 32768);
    return static_cast<std::uint16_t>(biased >> 4u);
}

inline std::uint16_t pwm_level(
    std::int16_t sample,
    std::uint16_t wrap
) {
    const std::uint32_t code = pcm_to_pwm_code(sample);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint64_t>(code) * wrap) / pwm_code_scale);
}

inline std::uint32_t pack_pwm_frame(
    std::int16_t left,
    std::int16_t right,
    std::uint16_t wrap,
    std::uint32_t output_gain_per_mille =
        picocalc_non_wav_output_gain_per_mille
) {
    const std::int16_t calibrated_left =
        apply_picocalc_output_gain(left, output_gain_per_mille);
    const std::int16_t calibrated_right =
        apply_picocalc_output_gain(right, output_gain_per_mille);
    return static_cast<std::uint32_t>(
               pwm_level(calibrated_left, wrap)) |
        (static_cast<std::uint32_t>(
             pwm_level(calibrated_right, wrap)) << 16u);
}

constexpr std::uint32_t diagnostic_sample_rate = 22050u;
constexpr std::uint32_t diagnostic_tone_hz = 440u;
constexpr std::uint32_t diagnostic_tone_frames = 5u;
constexpr std::uint32_t diagnostic_gap_frames = diagnostic_sample_rate / 4u;
constexpr std::uint32_t diagnostic_total_frames =
    diagnostic_tone_frames *
    (diagnostic_sample_rate + diagnostic_gap_frames);

struct DiagnosticToneStep {
    std::int16_t amplitude = 0;
    bool left = false;
    bool right = false;
    bool active = false;
};

inline DiagnosticToneStep diagnostic_tone_step(std::uint32_t frame) {
    if (frame >= diagnostic_total_frames) return {};

    const std::uint32_t block =
        diagnostic_sample_rate + diagnostic_gap_frames;
    const std::uint32_t tone = frame / block;
    const bool sounding = (frame % block) < diagnostic_sample_rate;
    DiagnosticToneStep step;
    step.active = true;
    if (!sounding) return step;

    constexpr std::int16_t amplitudes[diagnostic_tone_frames] = {
        4096, 8192, 16384, 8192, 8192
    };
    step.amplitude = amplitudes[tone];
    step.left = tone != 4u;
    step.right = tone != 3u;
    return step;
}

inline std::uint32_t record_audio_underrun(
    std::uint32_t current,
    bool source_active
) {
    return source_active && current != 0xffffffffu ? current + 1u : current;
}

using SequentialRead =
    std::size_t (*)(void* context, void* data, std::size_t size);

template <std::size_t Capacity>
class WavReadAheadBuffer {
public:
    static_assert(Capacity > 0, "WAV read-ahead buffer must not be empty");

    void reset(
        SequentialRead reader = nullptr,
        void* context = nullptr,
        std::uint32_t remaining = 0
    ) {
        reader_ = reader;
        context_ = context;
        remaining_ = remaining;
        position_ = 0;
        valid_ = 0;
        failed_ = false;
    }

    bool read_exact(void* output, std::size_t size) {
        if (!output || size > remaining_) {
            if (size != 0) failed_ = true;
            remaining_ = 0;
            return false;
        }

        auto* destination = static_cast<std::uint8_t*>(output);
        std::size_t copied = 0;
        while (copied < size) {
            if (position_ == valid_) {
                const std::size_t request = std::min<std::size_t>(
                    Capacity, remaining_);
                valid_ = reader_ ? reader_(context_, buffer_, request) : 0;
                position_ = 0;
                if (valid_ == 0 || valid_ > request) {
                    failed_ = true;
                    remaining_ = 0;
                    valid_ = 0;
                    return false;
                }
            }

            const std::size_t available = valid_ - position_;
            const std::size_t wanted = size - copied;
            const std::size_t take = std::min(available, wanted);
            std::memcpy(destination + copied, buffer_ + position_, take);
            position_ += take;
            copied += take;
            remaining_ -= static_cast<std::uint32_t>(take);
        }
        return true;
    }

    std::uint32_t remaining() const { return remaining_; }
    bool failed() const { return failed_; }

private:
    SequentialRead reader_ = nullptr;
    void* context_ = nullptr;
    std::uint32_t remaining_ = 0;
    std::size_t position_ = 0;
    std::size_t valid_ = 0;
    bool failed_ = false;
    std::uint8_t buffer_[Capacity] = {};
};

template <std::size_t Count>
class BufferRingOrder {
public:
    static_assert(Count > 0, "audio buffer ring must not be empty");

    void reset() {
        producer_ = 0;
        consumer_ = 0;
    }

    std::size_t producer_index() const { return producer_; }
    std::size_t consumer_index() const { return consumer_; }

    void advance_producer() { producer_ = (producer_ + 1u) % Count; }
    void advance_consumer() { consumer_ = (consumer_ + 1u) % Count; }

private:
    std::size_t producer_ = 0;
    std::size_t consumer_ = 0;
};

} // namespace rmb::audio
