// Hardware/decoder fixtures only: transport and service bodies come from audio.cpp.
#include <cassert>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "audio_buffer_policy.hpp"
#include "key_click.hpp"

#define __not_in_flash_func(x) x
using uint = unsigned;
constexpr uint kLeftPin = 26, kRightPin = 27;
constexpr std::size_t kFramesPerBuffer = 1024;
constexpr std::size_t kPwmWordsPerBuffer = kFramesPerBuffer * rmb::audio::pwm_repeats_per_sample;
constexpr std::size_t kBufferCount = 6;
constexpr int GPIO_DRIVE_STRENGTH_4MA = 4, GPIO_SLEW_RATE_SLOW = 0;
constexpr int GPIO_FUNC_PWM = 1, GPIO_FUNC_SIO = 2, GPIO_OUT = 1;

bool hardware_pwm = false, hardware_irq = false;
int pin_function[2] = {GPIO_FUNC_SIO, GPIO_FUNC_SIO};
int pin_level[2] = {0, 0};
unsigned pwm_starts = 0, pwm_stops = 0, written_words = 0;
unsigned boost_releases = 0, remaining_source_buffers = 0;
bool interrupts_disabled = false;
std::uint32_t mock_now_ms = 0;
std::uint16_t pending_left = 0, pending_right = 0;
std::uint16_t latched_left = 0, latched_right = 0;
void pwm_set_irq0_enabled(uint, bool enabled) { hardware_irq = enabled; }
void pwm_clear_irq(uint) {}
void pwm_set_enabled(uint, bool enabled) {
    hardware_pwm = enabled;
    if (enabled) ++pwm_starts; else ++pwm_stops;
}
void pwm_set_both_levels(uint, std::uint16_t left, std::uint16_t right) {
    pending_left = left;
    pending_right = right;
    // SDK compare writes latch immediately only while the slice is stopped.
    if (!hardware_pwm) {
        latched_left = left;
        latched_right = right;
    }
}
void pwm_set_counter(uint, int) {}
void gpio_set_drive_strength(uint, int) {}
void gpio_set_slew_rate(uint, int) {}
void gpio_set_function(uint pin, int function) {
    if (function == GPIO_FUNC_PWM && pin_function[pin - kLeftPin] == GPIO_FUNC_SIO)
        assert(latched_left == 0 && latched_right == 0);
    pin_function[pin - kLeftPin] = function;
}
void gpio_set_dir(uint, int) {}
void gpio_put(uint pin, int level) { pin_level[pin - kLeftPin] = level; }
std::uint32_t save_and_disable_interrupts() {
    const bool previous = interrupts_disabled;
    interrupts_disabled = true;
    return previous;
}
void restore_interrupts(std::uint32_t previous) { interrupts_disabled = previous; }

namespace rmb::storage {
bool card_ok = true;
bool firmware_owns_card() { return card_ok; }
bool card_present() { return card_ok; }
}
namespace rmb::platform {
enum class Source : std::uint8_t { None, Synth, Wav, Mp3, Diagnostic };
enum class BufferState : std::uint8_t { Free, Ready, Playing };
struct SynthStub { void stop() {} } synth;
audio::KeyClick key_click;
volatile Source source = Source::None;
bool initialized = true, output_enabled = false, pwm_running = false;
// @IDLE_POLICY@
bool synth_is_mml = false, wav_paused = false, underrun_active = false;
char current_audio_file[80] = {};
volatile bool pwm_irq_active = false;
uint pwm_slice = 5;
std::uint16_t pwm_centre_value = 1700;
volatile std::uint32_t underrun_count = 0;
volatile BufferState buffer_state[kBufferCount] = {};
audio::BufferRingOrder<kBufferCount> buffer_order;
volatile int playing_buffer = -1;
volatile std::size_t playing_word = 0;
std::uint32_t pwm_buffers[kBufferCount][kPwmWordsPerBuffer] = {};
void audio_init() { initialized = true; }
std::uint32_t monotonic_millis() { return mock_now_ms; }
void audio_stop();
void close_wav() {}
void set_error(const char*) {}
void release_cpu_boost() { ++boost_releases; }
// Source decoding/mixing has its own tests. Supply ordered full buffers here.
void fill_buffer(int index) {
    if (source != Source::None) {
        assert(remaining_source_buffers > 0);
        if (--remaining_source_buffers == 0) source = Source::None;
    }
    std::int16_t pcm[kFramesPerBuffer * 2] = {};
    key_click.mix(pcm, kFramesPerBuffer);
    // Identifiable audio level distinct from the centre/bias ramp. End each
    // full buffer at centre, as the real click decoder's silent tail does.
    for (std::size_t i = 0; i < kPwmWordsPerBuffer; ++i) {
        const std::uint16_t level = i + 1 == kPwmWordsPerBuffer ? pwm_centre_value : 1800;
        pwm_buffers[index][i] = level | (static_cast<std::uint32_t>(level) << 16u);
    }
    buffer_state[index] = BufferState::Ready;
}
// @PRODUCTION_FUNCTIONS@
}

using namespace rmb::platform;
void tick_wrap() {
    assert(hardware_pwm);
    // Hardware latches a submitted compare BEFORE dispatching the wrap ISR.
    latched_left = pending_left;
    latched_right = pending_right;
    if (hardware_irq) pwm_wrap_handler();
}
void finish_rise(bool service = true) {
    unsigned wraps = 0;
    while (pwm_bias_state != PwmBiasState::Steady) {
        assert(pwm_bias_state == PwmBiasState::Rising ||
               pwm_bias_state == PwmBiasState::SettlingUp);
        const auto previous = pending_left;
        assert(playing_word == 0);
        tick_wrap();
        assert(pending_left >= previous && pending_left - previous <= 2);
        assert(pending_left == pending_right && pending_left <= pwm_centre_value);
        assert(playing_word == 0); // no discarded/muted click or source samples.
        if (service) audio_service(); // must not restart an in-progress ramp.
        assert(++wraps <= kPwmBiasRampWraps + 1);
    }
    if (wraps) assert(pending_left == pwm_centre_value && latched_left == pwm_centre_value);
}
void assert_quiet() {
    assert(!hardware_pwm && !hardware_irq && !pwm_running && !pwm_irq_active);
    assert(!output_enabled && !idle_power_off_pending && playing_buffer < 0 && !interrupts_disabled);
    for (int i = 0; i < 2; ++i) {
        assert(pin_function[i] == GPIO_FUNC_SIO && pin_level[i] == 0);
    }
    for (auto state : buffer_state) assert(state == BufferState::Free);
}
void drain_buffer(bool service = true) {
    assert(playing_buffer >= 0);
    finish_rise(service);
    const auto index = playing_buffer;
    for (std::size_t i = 0; i < kPwmWordsPerBuffer; ++i) {
        assert(hardware_pwm && hardware_irq);
        tick_wrap();
        assert(pending_left == static_cast<std::uint16_t>(pwm_buffers[index][i]));
        assert(pending_right == static_cast<std::uint16_t>(pwm_buffers[index][i] >> 16u));
        ++written_words;
        // Service may fill/start buffers, but must never cut a queued tail.
        if (service && i + 1 < kPwmWordsPerBuffer) audio_service();
    }
}
void begin_idle() {
    // The last compare write is latched on a later PWM wrap. Foreground service
    // must not arm shutdown until the ISR has observed the empty queue.
    audio_service();
    assert(!idle_power_off_pending && hardware_pwm && hardware_irq);
    tick_wrap();
    audio_service();
    assert(idle_power_off_pending && hardware_pwm && !hardware_irq);
}
void expire_idle(std::uint32_t already_elapsed_ms = 0) {
    const auto releases = boost_releases;
    mock_now_ms += 9999u - already_elapsed_ms;
    audio_service();
    assert(hardware_pwm && idle_power_off_pending && !hardware_irq);
    assert(boost_releases == releases); // clock restoration must not bypass grace.
    ++mock_now_ms;
    audio_service();
    assert(pwm_bias_state == PwmBiasState::Falling && hardware_pwm && hardware_irq);
    assert(boost_releases == releases);
    assert(pending_left == pwm_centre_value); // foreground does not step the bias.
    for (std::uint32_t i = 0; i < kPwmBiasRampWraps; ++i) {
        const auto previous = pending_left;
        tick_wrap();
        assert(pending_left <= previous && previous - pending_left <= 2);
        assert(pending_left == pending_right);
        audio_service();
        assert(hardware_pwm && hardware_irq && boost_releases == releases);
    }
    assert(pending_left == 0 && pwm_bias_state == PwmBiasState::SettlingDown);
    tick_wrap(); // zero has latched, but wait a full zero-duty cycle as well.
    audio_service();
    assert(latched_left == 0 && latched_right == 0 && hardware_irq);
    assert(hardware_pwm && boost_releases == releases);
    tick_wrap();
    assert(pwm_bias_state == PwmBiasState::OffReady && !hardware_irq);
    assert(latched_left == 0 && latched_right == 0);
    audio_service();
    assert_quiet();
}
int main() {
    // Startup idle must stay electrically quiet without repeated resets.
    for (int i = 0; i < 20; ++i) audio_service();
    assert_quiet();
    assert(pwm_stops == 0);

    // Nearby real clicks must reuse one carrier rather than switch it off/on.
    for (int click = 0; click < 20; ++click) {
        audio_key_click();
        assert(hardware_pwm && hardware_irq && !key_click.active());
        drain_buffer();
        begin_idle();
        const auto stops = pwm_stops;
        mock_now_ms += 100u;
        for (int i = 0; i < 20; ++i) audio_service();
        assert(pwm_stops == stops && hardware_pwm && !hardware_irq);
    }
    assert(pwm_starts == 1 && pwm_stops == 0);
    assert(written_words == 20 * kPwmWordsPerBuffer);
    // The prior 100 ms already count toward the final click's idle deadline.
    expire_idle(100u);
    const auto stops = pwm_stops;
    for (int i = 0; i < 20; ++i) audio_service();
    assert(pwm_stops == stops);

    // A click just before the deadline cancels it until that new tail drains.
    audio_key_click();
    drain_buffer();
    begin_idle();
    mock_now_ms += 9999u;
    audio_key_click();
    assert(!idle_power_off_pending && hardware_pwm);
    mock_now_ms += 20000u; // old deadline expires while a fresh buffer is queued.
    audio_service();
    assert(hardware_pwm && hardware_irq);
    drain_buffer();
    begin_idle();
    expire_idle();

    // Unsigned monotonic subtraction must also work across the 32-bit wrap.
    mock_now_ms = 0xfffff000u;
    audio_key_click();
    drain_buffer();
    begin_idle();
    expire_idle();

    // Final buffers are already queued when decoding marks a source finished.
    // Cross ring wrap and cover every source sharing the PWM output path.
    for (auto kind : {Source::Synth, Source::Wav, Source::Mp3, Source::Diagnostic}) {
        source = kind;
        remaining_source_buffers = 9;
        audio_service();
        const auto before = written_words;
        for (int buffer = 0; buffer < 9; ++buffer) {
            assert(hardware_pwm);
            drain_buffer();
            audio_service();
            if (buffer < 8) assert(hardware_pwm);
        }
        begin_idle();
        expire_idle();
        assert(written_words - before == 9 * kPwmWordsPerBuffer);
    }

    // A live source with an underrun or pause must keep its output available.
    source = Source::Wav;
    wav_paused = true;
    buffer_state[buffer_order.producer_index()] = BufferState::Ready;
    buffer_order.advance_producer();
    start_ready_pwm();
    drain_buffer(false);
    tick_wrap();
    assert(hardware_pwm && hardware_irq && underrun_count == 1);
    tick_wrap();
    assert(underrun_count == 1);
    remaining_source_buffers = 1;
    audio_service(); // resume/refill after the underrun.
    assert(hardware_pwm && hardware_irq);
    drain_buffer();
    audio_service();
    begin_idle();
    expire_idle();

    // New clicks during every part of shutdown reverse smoothly and preserve
    // their full audio buffers, including zero submitted/latched and OffReady.
    for (const auto elapsed : {1u, kPwmBiasRampWraps / 4u,
             kPwmBiasRampWraps / 2u, kPwmBiasRampWraps - 1u,
             kPwmBiasRampWraps, kPwmBiasRampWraps + 1u,
             kPwmBiasRampWraps + 2u}) {
        audio_key_click();
        drain_buffer();
        begin_idle();
        mock_now_ms += kIdlePowerOffDelayMs;
        audio_service();
        for (unsigned i = 0; i < elapsed; ++i) tick_wrap();
        const auto before = pending_left;
        const auto stop_count = pwm_stops;
        audio_key_click();
        assert(pwm_bias_state == PwmBiasState::Rising && !idle_power_off_pending);
        assert(pending_left == before && pwm_stops == stop_count);
        assert(playing_buffer >= 0 && playing_word == 0);
        const auto words = written_words;
        drain_buffer();
        assert(written_words - words == kPwmWordsPerBuffer);
        begin_idle();
        expire_idle();
    }

    // Explicit stop cancels a queued click. OFF must not reactivate PWM.
    audio_key_click();
    audio_stop();
    assert_quiet();
    // A forced stop while waiting must cancel the delayed-off state as well.
    audio_key_click();
    drain_buffer();
    begin_idle();
    audio_stop();
    assert_quiet();
    // Forced stop must also cancel either ramp without a later IRQ restart.
    for (bool falling : {false, true}) {
        audio_key_click();
        if (falling) {
            drain_buffer();
            begin_idle();
            mock_now_ms += kIdlePowerOffDelayMs;
            audio_service();
        }
        for (unsigned i = 0; i < 100u; ++i) tick_wrap();
        audio_stop();
        assert_quiet();
        audio_service();
        assert_quiet();
    }
    // Repeated grace/ramp/stop with the actual production service and ISR bodies.
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        audio_key_click(); drain_buffer(); begin_idle(); expire_idle(); assert_quiet();
        audio_key_click(); audio_stop(); audio_service(); assert_quiet();
    }
    audio_set_key_click(rmb::audio::KeyClickMode::Off);
    audio_key_click();
    audio_service();
    assert_quiet();
    assert(boost_releases > 0);
}

