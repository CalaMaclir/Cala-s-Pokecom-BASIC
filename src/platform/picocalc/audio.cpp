#include "audio_engine.hpp"
#include "audio_buffer_policy.hpp"
#include "platform.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace rmb::platform {
namespace {

constexpr uint kLeftPin = 26;
constexpr uint kRightPin = 27;
constexpr std::size_t kFramesPerBuffer = 1024;
// Six DMA buffers provide about 139 ms of audio headroom at 44.1 kHz.
// Full-screen LCD SPI operations can occupy the foreground for roughly
// 100 ms, so the former two-buffer (~46 ms) queue could underrun even when
// the VM serviced audio regularly.
constexpr int kBufferCount = 6;

enum class Source : std::uint8_t { None, Synth, Wav };
enum class BufferState : std::uint8_t { Free, Ready, Playing };

audio::Engine synth;
audio::KeyClick key_click;
Source source = Source::None;
bool wav_paused = false;
int global_volume = 70;
bool initialized = false;
bool output_enabled = false;
int dma_channel = -1;
uint pwm_slice = 0;
volatile BufferState buffer_state[kBufferCount] = {
    BufferState::Free, BufferState::Free
};
volatile int playing_buffer = -1;
std::uint32_t pwm_buffers[kBufferCount][kFramesPerBuffer] = {};
std::int16_t pcm_buffer[kFramesPerBuffer * 2] = {};

FILE* wav_file = nullptr;
bool wav_storage_locked = false;
audio::WavInfo wav_info;
std::uint32_t wav_remaining = 0;
std::uint32_t wav_phase = 0;
std::int16_t wav_left = 0;
std::int16_t wav_right = 0;
bool wav_have_frame = false;
char audio_error[48] = "OK";

void set_error(const char* text) {
    std::snprintf(audio_error, sizeof(audio_error), "%s", text ? text : "AUDIO ERROR");
}

void close_wav() {
    if (wav_file) {
        std::fclose(wav_file);
        wav_file = nullptr;
    }
    if (wav_storage_locked) {
        storage::unlock();
        wav_storage_locked = false;
    }
    wav_remaining = 0;
    wav_have_frame = false;
}

void center_output() {
    if (initialized && output_enabled)
        pwm_set_both_levels(pwm_slice, 128, 128);
}

void enable_output() {
    if (!initialized || output_enabled) return;
    gpio_set_function(kLeftPin, GPIO_FUNC_PWM);
    gpio_set_function(kRightPin, GPIO_FUNC_PWM);
    pwm_set_counter(pwm_slice, 0);
    pwm_set_both_levels(pwm_slice, 128, 128);
    pwm_set_enabled(pwm_slice, true);
    output_enabled = true;
}

void start_dma_buffer(int index) {
    enable_output();
    buffer_state[index] = BufferState::Playing;
    playing_buffer = index;
    dma_channel_set_read_addr(dma_channel, pwm_buffers[index], false);
    dma_channel_set_trans_count(dma_channel, kFramesPerBuffer, true);
}

void dma_handler() {
    if (dma_channel < 0 ||
        (dma_hw->ints1 & (1u << static_cast<unsigned>(dma_channel))) == 0) {
        return;
    }
    dma_hw->ints1 = 1u << static_cast<unsigned>(dma_channel);
    const int completed = playing_buffer;
    if (completed >= 0) buffer_state[completed] = BufferState::Free;
    playing_buffer = -1;
    for (int i = 0; i < kBufferCount; ++i) {
        if (buffer_state[i] == BufferState::Ready) {
            start_dma_buffer(i);
            return;
        }
    }
    center_output();
}

void start_ready_dma() {
    const std::uint32_t irq_state = save_and_disable_interrupts();
    if (playing_buffer < 0) {
        for (int i = 0; i < kBufferCount; ++i) {
            if (buffer_state[i] == BufferState::Ready) {
                start_dma_buffer(i);
                break;
            }
        }
    }
    restore_interrupts(irq_state);
}

void reset_output() {
    if (!initialized) return;
    const std::uint32_t irq_state = save_and_disable_interrupts();
    if (dma_channel >= 0) {
        dma_channel_set_irq1_enabled(dma_channel, false);
        dma_channel_abort(dma_channel);
        dma_hw->ints1 = 1u << static_cast<unsigned>(dma_channel);
    }
    playing_buffer = -1;
    for (auto& state : buffer_state) state = BufferState::Free;

    // A centred PWM duty cycle is logically silent, but its carrier becomes
    // audible when STANDBY repeatedly gates and restores peripheral clocks.
    // Disable the slice and drive both audio pins low so sleep is electrically
    // quiet. enable_output() restores PWM lazily for the next BEEP/PLAY/WAV.
    pwm_set_enabled(pwm_slice, false);
    pwm_set_both_levels(pwm_slice, 0, 0);
    gpio_set_function(kLeftPin, GPIO_FUNC_SIO);
    gpio_set_function(kRightPin, GPIO_FUNC_SIO);
    gpio_set_dir(kLeftPin, GPIO_OUT);
    gpio_set_dir(kRightPin, GPIO_OUT);
    gpio_put(kLeftPin, 0);
    gpio_put(kRightPin, 0);
    output_enabled = false;
    if (dma_channel >= 0) dma_channel_set_irq1_enabled(dma_channel, true);
    restore_interrupts(irq_state);
}

bool file_read_at(
    void* context,
    std::uint32_t offset,
    void* data,
    std::size_t size
) {
    FILE* file = static_cast<FILE*>(context);
    if (!file || std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
        return false;
    return std::fread(data, 1, size, file) == size;
}

bool read_wav_frame() {
    if (!wav_file || wav_remaining == 0) return false;
    const std::uint32_t bytes =
        wav_info.channels * (wav_info.bits_per_sample / 8u);
    if (wav_remaining < bytes) return false;
    std::uint8_t data[4] = {};
    if (std::fread(data, 1, bytes, wav_file) != bytes) return false;
    wav_remaining -= bytes;

    auto sample = [&](int channel) -> std::int16_t {
        const int offset = channel * (wav_info.bits_per_sample / 8u);
        if (wav_info.bits_per_sample == 8) {
            return static_cast<std::int16_t>(
                (static_cast<int>(data[offset]) - 128) << 8
            );
        }
        return static_cast<std::int16_t>(
            static_cast<std::uint16_t>(data[offset]) |
            (static_cast<std::uint16_t>(data[offset + 1]) << 8)
        );
    };

    wav_left = sample(0);
    wav_right = wav_info.channels == 2 ? sample(1) : wav_left;
    wav_have_frame = true;
    return true;
}

bool render_wav(std::int16_t* output, std::size_t frames) {
    bool produced = false;
    for (std::size_t i = 0; i < frames; ++i) {
        if (!wav_paused && !wav_have_frame) {
            if (!read_wav_frame()) {
                output[i * 2] = 0;
                output[i * 2 + 1] = 0;
                continue;
            }
        }
        if (wav_paused) {
            output[i * 2] = 0;
            output[i * 2 + 1] = 0;
            continue;
        }
        // Ease the last 5 ms of source frames to zero. A non-zero final
        // sample followed by PWM center used to make a short audible pop.
        const unsigned frame_bytes = wav_info.channels *
            (wav_info.bits_per_sample / 8u);
        const unsigned frames_after_current = wav_remaining / frame_bytes;
        const unsigned gain = audio::wav_tail_gain(
            frames_after_current, wav_info.sample_rate);
        output[i * 2] = static_cast<std::int16_t>(
            static_cast<int>(wav_left) * global_volume * gain / 25600
        );
        output[i * 2 + 1] = static_cast<std::int16_t>(
            static_cast<int>(wav_right) * global_volume * gain / 25600
        );
        produced = true;

        wav_phase += wav_info.sample_rate;
        while (wav_phase >= audio::sample_rate) {
            wav_phase -= audio::sample_rate;
            wav_have_frame = false;
            if (!read_wav_frame()) break;
        }
    }
    return produced || wav_have_frame || wav_remaining > 0;
}

void fill_buffer(int index) {
    bool active = false;
    if (source == Source::Synth) {
        synth.render(pcm_buffer, kFramesPerBuffer);
        active = synth.active();
    } else if (source == Source::Wav) {
        active = render_wav(pcm_buffer, kFramesPerBuffer);
    } else {
        std::memset(pcm_buffer, 0, sizeof(pcm_buffer));
    }

    if (key_click.active()) key_click.mix(pcm_buffer, kFramesPerBuffer);
    // A completed WAV may have filled the final buffer only partway. Never
    // enqueue a subsequent empty buffer from reused PCM storage.
    if (!active && source != Source::None) {
        source = Source::None;
        close_wav();
        if (!key_click.active()) {
            // The final buffer is only needed if it actually contains audio.
            if (!audio::has_pcm_samples(pcm_buffer,
                                         kFramesPerBuffer * 2)) return;
        }
    }
    for (std::size_t i = 0; i < kFramesPerBuffer; ++i) {
        const int left = std::max(
            0, std::min(255, 128 + (static_cast<int>(pcm_buffer[i * 2]) >> 8))
        );
        const int right = std::max(
            0, std::min(255, 128 + (static_cast<int>(pcm_buffer[i * 2 + 1]) >> 8))
        );
        pwm_buffers[index][i] =
            static_cast<std::uint32_t>(left) |
            (static_cast<std::uint32_t>(right) << 16);
    }
    buffer_state[index] = BufferState::Ready;

}

const char* synth_error(audio::Result result) {
    switch (result) {
    case audio::Result::BadFrequency: return "BAD BEEP FREQUENCY";
    case audio::Result::BadDuration: return "BAD BEEP DURATION";
    case audio::Result::BadMml: return "BAD MML";
    case audio::Result::TooManyVoices: return "TOO MANY VOICES";
    default: return "OK";
    }
}

} // namespace

void audio_init() {
    if (initialized) return;
    synth.init();
    synth.set_volume(global_volume);

    gpio_set_function(kLeftPin, GPIO_FUNC_PWM);
    gpio_set_function(kRightPin, GPIO_FUNC_PWM);
    pwm_slice = pwm_gpio_to_slice_num(kLeftPin);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 255);
    const float divider =
        static_cast<float>(clock_get_hz(clk_sys)) /
        static_cast<float>(audio::sample_rate * 256u);
    pwm_config_set_clkdiv(&config, divider);
    pwm_init(pwm_slice, &config, false);
    pwm_set_both_levels(pwm_slice, 0, 0);

    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_config =
        dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(
        &dma_config,
        static_cast<uint>(DREQ_PWM_WRAP0 + pwm_slice)
    );
    dma_channel_configure(
        dma_channel,
        &dma_config,
        &pwm_hw->slice[pwm_slice].cc,
        nullptr,
        0,
        false
    );
    dma_channel_set_irq1_enabled(dma_channel, true);
    irq_add_shared_handler(
        DMA_IRQ_1,
        dma_handler,
        PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY
    );
    irq_set_enabled(DMA_IRQ_1, true);
    initialized = true;
    reset_output();
}

void audio_reconfigure_clock() {
    if (!initialized) return;
    const float divider =
        static_cast<float>(clock_get_hz(clk_sys)) /
        static_cast<float>(audio::sample_rate * 256u);
    pwm_set_clkdiv(pwm_slice, divider);
}

void audio_service() {
    if (!initialized) audio_init();
    if (source == Source::Wav &&
        (!storage::firmware_owns_card() || !storage::card_present())) {
        audio_stop();
        set_error("SD CARD REMOVED");
        return;
    }
    if (source == Source::None && !key_click.active()) {
        start_ready_dma();
        return;
    }
    for (int i = 0; i < kBufferCount; ++i) {
        if (source == Source::None && !key_click.active()) break;
        if (buffer_state[i] == BufferState::Free) fill_buffer(i);
    }
    start_ready_dma();
}

void audio_stop() {
    key_click.cancel();
    synth.stop();
    close_wav();
    source = Source::None;
    wav_paused = false;
    reset_output();
}

void audio_set_key_click(audio::KeyClickMode mode) {
    key_click.set_mode(mode);
}

void audio_key_click() {
    if (!key_click.trigger(true, false)) return;
    audio_service(); // Mix into the next free DMA buffer; never stop PLAY/WAV.
}

void audio_pause() {
    if (source == Source::Synth) synth.pause();
    if (source == Source::Wav) wav_paused = true;
}

void audio_resume() {
    if (source == Source::Synth) synth.resume();
    if (source == Source::Wav) wav_paused = false;
}

bool audio_playing() {
    if (source != Source::None) return true;
    if (playing_buffer >= 0) return true;
    for (const auto state : buffer_state)
        if (state == BufferState::Ready) return true;
    return false;
}

bool audio_beep(int frequency_hz, int duration_ms) {
    audio_stop();
    const audio::Result result = synth.start_beep(frequency_hz, duration_ms);
    if (result != audio::Result::Ok) {
        set_error(synth_error(result));
        return false;
    }
    source = Source::Synth;
    set_error("OK");
    audio_service();
    return true;
}

bool audio_play_mml(const char* const* voices, int count) {
    audio_stop();
    const audio::Result result = synth.start_mml(voices, count);
    if (result != audio::Result::Ok) {
        set_error(synth_error(result));
        return false;
    }
    source = Source::Synth;
    set_error("OK");
    audio_service();
    return true;
}

bool audio_wavplay(const char* filename) {
    if (!filename || !*filename) {
        set_error("BAD WAV FILENAME");
        return false;
    }
    audio_stop();
    if (!storage::firmware_owns_card() || !storage::available() ||
        !storage::card_present()) {
        set_error("SD CARD NOT AVAILABLE");
        return false;
    }
    if (!storage::try_lock()) {
        set_error(storage::last_error());
        return false;
    }
    wav_storage_locked = true;

    char path[96] = {};
    if (filename[0] == '/') {
        std::snprintf(path, sizeof(path), "%s", filename);
    } else {
        std::snprintf(path, sizeof(path), "/%s", filename);
    }
    wav_file = std::fopen(path, "rb");
    if (!wav_file) {
        close_wav();
        set_error("WAV FILE NOT FOUND");
        return false;
    }
    if (std::fseek(wav_file, 0, SEEK_END) != 0) {
        close_wav();
        set_error("WAV READ ERROR");
        return false;
    }
    const long file_size = std::ftell(wav_file);
    if (file_size < 0 ||
        static_cast<unsigned long>(file_size) > 0xfffffffful) {
        close_wav();
        set_error("WAV TOO LARGE");
        return false;
    }

    const audio::WavError error = audio::parse_wav(
        file_read_at,
        wav_file,
        static_cast<std::uint32_t>(file_size),
        wav_info
    );
    if (error != audio::WavError::Ok) {
        close_wav();
        set_error(
            error == audio::WavError::UnsupportedFormat
                ? "UNSUPPORTED WAV" :
            error == audio::WavError::Truncated
                ? "TRUNCATED WAV" : "INVALID WAV"
        );
        return false;
    }
    if (std::fseek(
            wav_file,
            static_cast<long>(wav_info.data_offset),
            SEEK_SET
        ) != 0) {
        close_wav();
        set_error("WAV READ ERROR");
        return false;
    }

    wav_remaining = wav_info.data_size;
    wav_phase = 0;
    wav_have_frame = false;
    wav_paused = false;
    source = Source::Wav;
    set_error("OK");
    audio_service();
    return true;
}

void audio_set_volume(int percent) {
    global_volume = std::max(0, std::min(100, percent));
    synth.set_volume(global_volume);
}

int audio_volume() { return global_volume; }
const char* audio_last_error() { return audio_error; }

} // namespace rmb::platform
