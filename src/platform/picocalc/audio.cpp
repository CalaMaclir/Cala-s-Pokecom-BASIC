#include "audio_engine.hpp"
#include "audio_buffer_policy.hpp"
#include "audio_file_policy.hpp"
#include "platform.hpp"
#include "file_path.hpp"
#include "storage.hpp"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MINIMP3_IO_SIZE (32 * 1024)
#include "minimp3_ex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"


namespace rmb::platform {
namespace {

constexpr uint kLeftPin = 26;
constexpr uint kRightPin = 27;
constexpr std::size_t kFramesPerBuffer = 1024;
constexpr std::size_t kPwmWordsPerBuffer =
    kFramesPerBuffer * audio::pwm_repeats_per_sample;
constexpr std::size_t kWavReadAheadBytes = 4096;
// Six PWM word buffers provide about 279 ms of headroom at CPB's 22.05 kHz
// output rate. Keep producer and consumer positions explicit: searching for
// the lowest-numbered Ready slot can replay newly refilled buffers ahead of
// older queued audio after the ring wraps.
constexpr std::size_t kBufferCount = 6;

enum class Source : std::uint8_t { None, Synth, Wav, Mp3, Diagnostic };
enum class BufferState : std::uint8_t { Free, Ready, Playing };

audio::Engine synth;
audio::KeyClick key_click;
volatile Source source = Source::None;
bool wav_paused = false;
int master_volume = 70;
int wav_volume = 50;
int play_volume = 100;
bool synth_is_mml = false;
bool initialized = false;
bool output_enabled = false;
bool pwm_running = false;
volatile bool pwm_irq_active = false;
uint pwm_slice = 0;
std::uint16_t pwm_wrap_value = 0;
std::uint16_t pwm_centre_value = 0;
volatile std::uint32_t underrun_count = 0;
bool underrun_active = false;
volatile BufferState buffer_state[kBufferCount] = {};
audio::BufferRingOrder<kBufferCount> buffer_order;
volatile int playing_buffer = -1;
volatile std::size_t playing_word = 0;
std::uint32_t pwm_buffers[kBufferCount][kPwmWordsPerBuffer] = {};
std::int16_t pcm_buffer[kFramesPerBuffer * 2] = {};

FILE* wav_file = nullptr;
bool wav_storage_locked = false;
audio::WavInfo wav_info;
std::uint32_t wav_remaining = 0;
audio::WavResamplePhase wav_resampler;
std::int16_t wav_left = 0;
std::int16_t wav_right = 0;
audio::WavReadAheadBuffer<kWavReadAheadBytes> wav_read_ahead;
bool wav_have_frame = false;
bool wav_read_failed = false;

mp3dec_ex_t mp3_decoder = {};
mp3dec_io_t mp3_io = {};
bool mp3_open = false;
mp3d_sample_t* mp3_samples = nullptr;
std::size_t mp3_sample_count = 0;
std::size_t mp3_sample_position = 0;
int mp3_channels = 0;
int mp3_sample_rate = 0;
audio::WavResamplePhase mp3_resampler;
bool mp3_read_failed = false;

bool cpu_boost_active = false;
bool cpu_boost_transition = false;
std::uint32_t cpu_clock_before_boost_mhz = 0;

std::uint32_t diagnostic_frame = 0;
std::uint32_t diagnostic_phase = 0;
char audio_error[48] = "OK";

void set_error(const char* text) {
    std::snprintf(audio_error, sizeof(audio_error), "%s",
                  text ? text : "AUDIO ERROR");
}

void close_wav() {
    if (mp3_open) {
        mp3dec_ex_close(&mp3_decoder);
        mp3_open = false;
    }
    mp3_samples = nullptr;
    mp3_sample_count = 0;
    mp3_sample_position = 0;
    mp3_channels = 0;
    mp3_sample_rate = 0;
    mp3_resampler.reset();
    mp3_read_failed = false;
    if (wav_file) {
        std::fclose(wav_file);
        wav_file = nullptr;
    }
    if (wav_storage_locked) {
        storage::unlock();
        wav_storage_locked = false;
    }
    wav_remaining = 0;
    wav_read_ahead.reset();
    wav_resampler.reset();
    wav_have_frame = false;
    wav_read_failed = false;
}

void acquire_cpu_boost() {
    const std::uint32_t current_mhz = system_clock_hz() / 1000000u;
    cpu_clock_before_boost_mhz = current_mhz;
    if (current_mhz >= 200u) return;

    cpu_boost_transition = true;
    bool changed = set_cpu_clock_mhz(200u);
    if (!changed && current_mhz < 150u) {
        // Wireless or another active subsystem can make the experimental
        // PLL transition unavailable. A rated 150 MHz fallback still needs
        // no user action and keeps playback usable.
        changed = set_cpu_clock_mhz(150u);
    }
    cpu_boost_transition = false;
    cpu_boost_active = changed;
}

void release_cpu_boost() {
    if (!cpu_boost_active || cpu_boost_transition) return;
    const std::uint32_t restore_mhz = cpu_clock_before_boost_mhz;
    cpu_boost_active = false;
    if (restore_mhz == 0u ||
        system_clock_hz() / 1000000u == restore_mhz) return;

    cpu_boost_transition = true;
    (void)set_cpu_clock_mhz(restore_mhz);
    cpu_boost_transition = false;
}

void configure_pwm_clock() {
    const audio::PwmTiming timing =
        audio::pwm_timing(clock_get_hz(clk_sys));
    pwm_wrap_value = timing.wrap;

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, pwm_wrap_value);
    pwm_init(pwm_slice, &config, false);
    pwm_centre_value = audio::pwm_level(0, pwm_wrap_value);
    pwm_set_both_levels(
        pwm_slice, pwm_centre_value, pwm_centre_value);
}

void prepare_pwm_for_start() {
    // Make the RP2350 pad state deterministic. Slow edges match the
    // board-proven PicoCalc configuration and reduce carrier energy entering
    // the analogue filter without changing the PCM data.
    gpio_set_drive_strength(kLeftPin, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(kRightPin, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(kLeftPin, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(kRightPin, GPIO_SLEW_RATE_SLOW);
    gpio_set_function(kLeftPin, GPIO_FUNC_PWM);
    gpio_set_function(kRightPin, GPIO_FUNC_PWM);
    pwm_set_counter(pwm_slice, 0);
    pwm_set_both_levels(
        pwm_slice, pwm_centre_value, pwm_centre_value);
    output_enabled = true;
}

void halt_pwm_output() {
    if (!initialized) return;
    pwm_set_irq0_enabled(pwm_slice, false);
    pwm_clear_irq(pwm_slice);
    pwm_irq_active = false;
    pwm_set_enabled(pwm_slice, false);
    pwm_running = false;
}

bool __not_in_flash_func(activate_next_buffer)() {
    if (playing_buffer >= 0) return true;
    const std::size_t next = buffer_order.consumer_index();
    if (buffer_state[next] != BufferState::Ready) return false;
    buffer_order.advance_consumer();
    buffer_state[next] = BufferState::Playing;
    playing_buffer = static_cast<int>(next);
    playing_word = 0;
    underrun_active = false;
    return true;
}

void __not_in_flash_func(write_pwm_word)(std::uint32_t packed) {
    pwm_set_both_levels(
        pwm_slice,
        static_cast<std::uint16_t>(packed),
        static_cast<std::uint16_t>(packed >> 16u));
}

void __not_in_flash_func(pwm_wrap_handler)() {
    pwm_clear_irq(pwm_slice);

    if (!activate_next_buffer()) {
        const bool source_active = source != Source::None;
        if (source_active && !underrun_active) {
            underrun_count = audio::record_audio_underrun(
                underrun_count, true);
            underrun_active = true;
        }
        pwm_set_both_levels(
            pwm_slice, pwm_centre_value, pwm_centre_value);

        // Once the final queued buffer has drained, retain centre duty without
        // spending CPU time on an otherwise idle 44.1 kHz interrupt.
        if (!source_active) {
            pwm_set_irq0_enabled(pwm_slice, false);
            pwm_irq_active = false;
        }
        return;
    }

    const int index = playing_buffer;
    write_pwm_word(pwm_buffers[index][playing_word]);
    ++playing_word;
    if (playing_word == kPwmWordsPerBuffer) {
        buffer_state[index] = BufferState::Free;
        playing_buffer = -1;
        playing_word = 0;
    }
    underrun_active = false;
}

void start_ready_pwm() {
    const std::uint32_t irq_state = save_and_disable_interrupts();
    if (activate_next_buffer()) {
        if (!output_enabled) prepare_pwm_for_start();
        if (!pwm_irq_active) {
            pwm_clear_irq(pwm_slice);
            pwm_set_irq0_enabled(pwm_slice, true);
            pwm_irq_active = true;
        }
        if (!pwm_running) {
            pwm_set_enabled(pwm_slice, true);
            pwm_running = true;
        }
    }
    restore_interrupts(irq_state);
}

void reset_output() {
    if (!initialized) return;
    const std::uint32_t irq_state = save_and_disable_interrupts();
    halt_pwm_output();

    playing_buffer = -1;
    playing_word = 0;
    for (auto& state : buffer_state) state = BufferState::Free;
    buffer_order.reset();
    underrun_active = false;

    // STANDBY and explicit stop must be electrically quiet.
    pwm_set_both_levels(pwm_slice, 0, 0);
    gpio_set_function(kLeftPin, GPIO_FUNC_SIO);
    gpio_set_function(kRightPin, GPIO_FUNC_SIO);
    gpio_set_dir(kLeftPin, GPIO_OUT);
    gpio_set_dir(kRightPin, GPIO_OUT);
    gpio_put(kLeftPin, 0);
    gpio_put(kRightPin, 0);
    output_enabled = false;

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

std::size_t read_wav_bytes(
    void* context,
    void* data,
    std::size_t size
) {
    FILE* file = static_cast<FILE*>(context);
    return file ? std::fread(data, 1, size, file) : 0;
}

std::size_t read_mp3_bytes(void* data, std::size_t size, void* context) {
    FILE* file = static_cast<FILE*>(context);
    return file ? std::fread(data, 1, size, file) : 0;
}

int seek_mp3_file(std::uint64_t position, void* context) {
    FILE* file = static_cast<FILE*>(context);
    if (!file || position > 0x7fffffffu) return -1;
    return std::fseek(file, static_cast<long>(position), SEEK_SET);
}

bool read_wav_frame() {
    if (!wav_file || wav_remaining == 0) return false;
    const std::uint32_t sample_bytes = wav_info.bits_per_sample / 8u;
    const std::uint32_t frame_bytes = wav_info.channels * sample_bytes;
    std::uint8_t data[4] = {};
    if (!wav_read_ahead.read_exact(data, frame_bytes)) {
        // The RIFF data chunk promised more bytes than the card delivered.
        // Mark the source terminal so service cannot spin forever producing
        // silence while retaining the storage lock.
        wav_remaining = 0;
        wav_have_frame = false;
        wav_read_failed = true;
        set_error("WAV READ ERROR");
        return false;
    }
    wav_remaining = wav_read_ahead.remaining();

    wav_left = audio::decode_pcm_sample(data, wav_info.bits_per_sample);
    wav_right = wav_info.channels == 2
        ? audio::decode_pcm_sample(data + sample_bytes,
                                   wav_info.bits_per_sample)
        : wav_left;
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
        // WAV volume is independent of BEEP/PLAY volume. The fixed
        // PicoCalc output ceiling is applied later at the common PWM boundary.
        const std::int16_t scaled_left = audio::apply_audio_volume(
            wav_left, static_cast<std::uint32_t>(wav_volume));
        const std::int16_t scaled_right = audio::apply_audio_volume(
            wav_right, static_cast<std::uint32_t>(wav_volume));
        output[i * 2] = static_cast<std::int16_t>(
            static_cast<int>(scaled_left) * gain / 256
        );
        output[i * 2 + 1] = static_cast<std::int16_t>(
            static_cast<int>(scaled_right) * gain / 256
        );
        produced = true;

        const unsigned input_frames = wav_resampler.advance(
            wav_info.sample_rate, audio::sample_rate);
        for (unsigned frame = 0; frame < input_frames; ++frame) {
            wav_have_frame = false;
            if (!read_wav_frame()) break;
        }
    }
    return !wav_read_failed &&
        (produced || wav_have_frame || wav_remaining > 0);
}

bool read_mp3_frame() {
    mp3dec_frame_info_t frame_info = {};
    mp3_samples = nullptr;
    mp3_sample_count = mp3dec_ex_read_frame(
        &mp3_decoder,
        &mp3_samples,
        &frame_info,
        MINIMP3_MAX_SAMPLES_PER_FRAME);
    mp3_sample_position = 0;
    if (mp3_sample_count == 0) {
        if (mp3_decoder.last_error != 0) {
            mp3_read_failed = true;
            set_error("MP3 DECODE ERROR");
        }
        return false;
    }
    if ((frame_info.channels != 1 && frame_info.channels != 2) ||
        (frame_info.hz != 44100 && frame_info.hz != 48000) ||
        frame_info.layer != 3 ||
        mp3_sample_count % static_cast<std::size_t>(frame_info.channels) != 0) {
        mp3_read_failed = true;
        set_error("UNSUPPORTED MP3");
        return false;
    }
    mp3_channels = frame_info.channels;
    mp3_sample_rate = frame_info.hz;
    return true;
}

bool render_mp3(std::int16_t* output, std::size_t frames) {
    bool produced = false;
    for (std::size_t i = 0; i < frames; ++i) {
        if (wav_paused) {
            output[i * 2] = 0;
            output[i * 2 + 1] = 0;
            continue;
        }
        if (mp3_sample_position >= mp3_sample_count &&
            !read_mp3_frame()) {
            output[i * 2] = 0;
            output[i * 2 + 1] = 0;
            continue;
        }

        const std::int16_t raw_left =
            mp3_samples[mp3_sample_position];
        const std::int16_t raw_right = mp3_channels == 2
            ? mp3_samples[mp3_sample_position + 1] : raw_left;
        const std::int16_t left = audio::apply_audio_volume(
            raw_left, static_cast<std::uint32_t>(wav_volume));
        const std::int16_t right = audio::apply_audio_volume(
            raw_right, static_cast<std::uint32_t>(wav_volume));
        output[i * 2] = left;
        output[i * 2 + 1] = right;
        produced = true;

        const unsigned input_frames = mp3_resampler.advance(
            static_cast<std::uint32_t>(mp3_sample_rate),
            audio::sample_rate);
        for (unsigned frame = 0; frame < input_frames; ++frame) {
            mp3_sample_position += static_cast<std::size_t>(mp3_channels);
            if (mp3_sample_position >= mp3_sample_count &&
                !read_mp3_frame()) break;
        }
    }
    return !mp3_read_failed &&
        (produced || mp3_sample_position < mp3_sample_count);
}

std::int16_t diagnostic_sine(std::uint32_t phase) {
    static constexpr std::int16_t quarter_wave[65] = {
        0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
        9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
        18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
        25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
        30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
        32609, 32678, 32728, 32757, 32767
    };
    const std::uint32_t index = phase >> 24u;
    const std::uint32_t quadrant = index >> 6u;
    const std::uint32_t offset = index & 63u;
    switch (quadrant) {
    case 0: return quarter_wave[offset];
    case 1: return quarter_wave[64u - offset];
    case 2: return static_cast<std::int16_t>(-quarter_wave[offset]);
    default:
        return static_cast<std::int16_t>(-quarter_wave[64u - offset]);
    }
}

bool render_diagnostic(std::int16_t* output, std::size_t frames) {
    constexpr std::uint32_t phase_step = static_cast<std::uint32_t>(
        ((static_cast<std::uint64_t>(audio::diagnostic_tone_hz) << 32u) +
         audio::sample_rate / 2u) / audio::sample_rate);

    for (std::size_t i = 0; i < frames; ++i) {
        const audio::DiagnosticToneStep step =
            audio::diagnostic_tone_step(diagnostic_frame);
        if (!step.active) {
            output[i * 2] = 0;
            output[i * 2 + 1] = 0;
            continue;
        }

        const std::int32_t sine = diagnostic_sine(diagnostic_phase);
        diagnostic_phase += phase_step;
        ++diagnostic_frame;
        const std::int16_t sample = audio::apply_audio_volume(
            static_cast<std::int16_t>(
                sine * static_cast<std::int32_t>(step.amplitude) / 32767),
            static_cast<std::uint32_t>(wav_volume));
        output[i * 2] = step.left ? sample : 0;
        output[i * 2 + 1] = step.right ? sample : 0;
    }
    return diagnostic_frame < audio::diagnostic_total_frames;
}

void fill_buffer(int index) {
    // Preserve the source class even if rendering this final buffer completes
    // playback and changes source to None.
    const bool wav_path =
        source == Source::Wav || source == Source::Mp3 ||
        source == Source::Diagnostic;

    bool active = false;
    if (source == Source::Synth) {
        synth.render(pcm_buffer, kFramesPerBuffer);
        active = synth.active();
    } else if (source == Source::Wav) {
        active = render_wav(pcm_buffer, kFramesPerBuffer);
    } else if (source == Source::Mp3) {
        active = render_mp3(pcm_buffer, kFramesPerBuffer);
    } else if (source == Source::Diagnostic) {
        active = render_diagnostic(pcm_buffer, kFramesPerBuffer);
    } else {
        std::memset(pcm_buffer, 0, sizeof(pcm_buffer));
    }

    // Master volume is common to BEEP, PLAY, WAV, MP3, and diagnostics.
    // Key Click stays independently calibrated and is mixed afterwards.
    for (std::size_t i = 0; i < kFramesPerBuffer * 2; ++i) {
        pcm_buffer[i] = audio::apply_audio_volume(
            pcm_buffer[i], static_cast<std::uint32_t>(master_volume));
    }
    if (key_click.active()) {
        // The WAV/MP3 output ceiling is twice the PLAY ceiling. Compensate
        // here so Key Click keeps the same physical level on both paths.
        key_click.mix(
            pcm_buffer, kFramesPerBuffer, wav_path ? 25u : 50u);
    }
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

    const std::uint32_t output_gain = wav_path
        ? audio::picocalc_wav_output_gain_per_mille
        : audio::picocalc_non_wav_output_gain_per_mille;
    for (std::size_t i = 0; i < kFramesPerBuffer; ++i) {
        const std::uint32_t packed = audio::pack_pwm_frame(
            pcm_buffer[i * 2],
            pcm_buffer[i * 2 + 1],
            pwm_wrap_value,
            output_gain);
        const std::size_t output =
            i * audio::pwm_repeats_per_sample;
        for (std::size_t repeat = 0;
             repeat < audio::pwm_repeats_per_sample; ++repeat)
            pwm_buffers[index][output + repeat] = packed;
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
    // Source volume is selected when BEEP or PLAY starts. Master volume is
    // applied later at the common PCM boundary.
    synth.set_volume(100);

    gpio_set_function(kLeftPin, GPIO_FUNC_PWM);
    gpio_set_function(kRightPin, GPIO_FUNC_PWM);
    pwm_slice = pwm_gpio_to_slice_num(kLeftPin);
    configure_pwm_clock();

    irq_add_shared_handler(
        PWM_IRQ_WRAP,
        pwm_wrap_handler,
        PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    // Audio generates 44.1 kHz wrap interrupts while active. Keep that exact
    // PicoCalc-proven sample path, but let CYW43/BTstack and USB IRQ work
    // preempt it so CYW43 and USB interrupt work cannot starve.
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    initialized = true;
    reset_output();
}

void audio_reconfigure_clock() {
    if (!initialized) return;
    audio_stop();
    configure_pwm_clock();
}

void audio_service() {
    if (!initialized) audio_init();
    if ((source == Source::Wav || source == Source::Mp3) &&
        (!storage::firmware_owns_card() || !storage::card_present())) {
        audio_stop();
        set_error("SD CARD REMOVED");
        return;
    }
    if (source == Source::None && !key_click.active()) {
        start_ready_pwm();
        bool queued = playing_buffer >= 0;
        for (const auto state : buffer_state)
            queued = queued || state == BufferState::Ready;
        if (!queued) release_cpu_boost();
        return;
    }
    for (std::size_t filled = 0; filled < kBufferCount; ++filled) {
        if (source == Source::None && !key_click.active()) break;
        const std::size_t index = buffer_order.producer_index();
        if (buffer_state[index] != BufferState::Free) break;

        fill_buffer(static_cast<int>(index));
        if (buffer_state[index] != BufferState::Ready) break;
        buffer_order.advance_producer();
    }
    start_ready_pwm();
}

void audio_stop() {
    key_click.cancel();
    synth.stop();
    synth_is_mml = false;
    close_wav();
    source = Source::None;
    wav_paused = false;
    reset_output();
    release_cpu_boost();
}

void audio_set_key_click(audio::KeyClickMode mode) {
    key_click.set_mode(mode);
}

void audio_key_click() {
    if (!key_click.trigger(true, false)) return;
    audio_service(); // Mix into the next free audio buffer; never stop PLAY/WAV.
}

void audio_pause() {
    if (source == Source::Synth) synth.pause();
    if (source == Source::Wav || source == Source::Mp3)
        wav_paused = true;
}

void audio_resume() {
    if (source == Source::Synth) synth.resume();
    if (source == Source::Wav || source == Source::Mp3)
        wav_paused = false;
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
    synth.set_volume(100);
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
    synth.set_volume(play_volume);
    const audio::Result result = synth.start_mml(voices, count);
    if (result != audio::Result::Ok) {
        set_error(synth_error(result));
        return false;
    }
    synth_is_mml = true;
    source = Source::Synth;
    set_error("OK");
    audio_service();
    return true;
}

bool audio_wavplay(const char* filename) {
    if (!filename || !*filename) {
        set_error("BAD AUDIO FILENAME");
        return false;
    }

    // Engineering diagnostic: generate known signed PCM directly in the
    // platform backend. This bypasses SD and all file decoders while retaining
    // the exact same PWM/IRQ output path.
    if (std::strcmp(filename, "@SINE440") == 0) {
        audio_stop();
        diagnostic_frame = 0;
        diagnostic_phase = 0;
        source = Source::Diagnostic;
        set_error("OK");
        audio_service();
        return true;
    }

    audio_stop();
    if (!storage::firmware_owns_card() || !storage::available() ||
        !storage::card_present()) {
        set_error("SD CARD NOT AVAILABLE");
        return false;
    }

    char path[96] = {}, relative[80] = {};
    if (!file_paths::normalize(filename, relative, sizeof(relative)) ||
        !file_paths::physical("/", relative, path, sizeof(path))) {
        set_error("BAD AUDIO PATH");
        return false;
    }

    auto open_selected_file = [&]() {
        if (!storage::try_lock()) {
            set_error(storage::last_error());
            return false;
        }
        wav_storage_locked = true;
        wav_file = std::fopen(path, "rb");
        if (!wav_file) {
            close_wav();
            set_error("AUDIO FILE NOT FOUND");
            return false;
        }
        return true;
    };

    if (!open_selected_file()) return false;
    if (std::fseek(wav_file, 0, SEEK_END) != 0) {
        close_wav();
        set_error("AUDIO READ ERROR");
        return false;
    }
    const long file_size = std::ftell(wav_file);
    if (file_size < 0 ||
        static_cast<unsigned long>(file_size) > 0xfffffffful) {
        close_wav();
        set_error("AUDIO FILE TOO LARGE");
        return false;
    }

    unsigned char header[12] = {};
    const bool is_wav =
        file_size >= static_cast<long>(sizeof(header)) &&
        file_read_at(wav_file, 0, header, sizeof(header)) &&
        audio::wav_file_header(header, sizeof(header));

    if (is_wav) {
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

        wav_read_ahead.reset(
            read_wav_bytes, wav_file, wav_info.data_size);
        wav_remaining = wav_read_ahead.remaining();
        wav_resampler.reset();
        wav_have_frame = false;
        wav_read_failed = false;
        wav_paused = false;
        source = Source::Wav;
        set_error("OK");
        audio_service();
        return true;
    }

    // The decoder identifies MP3 content (including ID3-prefixed files).
    // Change the system clock only while no SD transaction is active, then
    // reopen the file under the normal storage ownership lock.
    close_wav();
    acquire_cpu_boost();
    if (!open_selected_file()) {
        release_cpu_boost();
        return false;
    }

    mp3_io = {};
    mp3_io.read = read_mp3_bytes;
    mp3_io.read_data = wav_file;
    mp3_io.seek = seek_mp3_file;
    mp3_io.seek_data = wav_file;
    mp3_open = true;
    if (mp3dec_ex_open_cb(
            &mp3_decoder, &mp3_io, MP3D_DO_NOT_SCAN) != 0) {
        close_wav();
        release_cpu_boost();
        set_error("INVALID MP3");
        return false;
    }
    if (mp3_decoder.info.layer != 3 ||
        (mp3_decoder.info.channels != 1 &&
         mp3_decoder.info.channels != 2) ||
        (mp3_decoder.info.hz != 44100 &&
         mp3_decoder.info.hz != 48000)) {
        close_wav();
        release_cpu_boost();
        set_error("UNSUPPORTED MP3");
        return false;
    }

    mp3_channels = mp3_decoder.info.channels;
    mp3_sample_rate = mp3_decoder.info.hz;
    mp3_sample_count = 0;
    mp3_sample_position = 0;
    mp3_samples = nullptr;
    mp3_resampler.reset();
    mp3_read_failed = false;
    wav_paused = false;
    source = Source::Mp3;
    set_error("OK");
    audio_service();
    return true;
}

void audio_set_volume(int percent) {
    master_volume = std::max(0, std::min(100, percent));
}

int audio_volume() { return master_volume; }

void audio_set_play_volume(int percent) {
    play_volume = std::max(0, std::min(100, percent));
    if (source == Source::Synth && synth_is_mml)
        synth.set_volume(play_volume);
}

int audio_play_volume() { return play_volume; }

void audio_set_wav_volume(int percent) {
    wav_volume = std::max(10, std::min(100, percent));
}

int audio_wav_volume() { return wav_volume; }
std::uint32_t audio_underruns() { return underrun_count; }
const char* audio_last_error() { return audio_error; }

} // namespace rmb::platform
