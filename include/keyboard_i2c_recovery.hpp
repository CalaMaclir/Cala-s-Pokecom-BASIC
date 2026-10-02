#pragma once

#include <cstdint>

namespace rmb::picocalc::keyboard {

enum class Health : std::uint8_t {
    Initializing,
    Waiting,
    Ok,
    Degraded,
    Recovering,
    Lost
};

enum class StartupPhase : std::uint8_t {
    BootQuiet,
    WaitBusIdle,
    InitController,
    ProbeSelect,
    ProbeWait,
    ProbeRead,
    Ready,
    PassiveBackoff
};

enum class I2cError : std::uint8_t {
    None,
    NotInitialized,
    AddressNack,
    DataNack,
    WriteNack,
    WriteTimeout,
    ShortWrite,
    ReadNack,
    ReadTimeout,
    ShortRead,
    SclStuck,
    SdaStuck
};

struct Diagnostics {
    Health health = Health::Initializing;
    I2cError last_error = I2cError::None;
    std::uint32_t total_errors = 0;
    std::uint32_t consecutive_errors = 0;
    std::uint32_t recovery_attempts = 0;
    std::uint32_t recoveries = 0;
    StartupPhase startup_phase = StartupPhase::BootQuiet;
    bool ever_ready = false;
    bool sda_high = false;
    bool scl_high = false;
    bool first_try_recorded = false;
    bool first_ack_recorded = false;
    std::uint32_t first_try_ms = 0;
    std::uint32_t first_ack_ms = 0;
    std::uint32_t startup_attempts = 0;
};

namespace detail {

class RecoveryPolicy {
public:
    static constexpr std::uint32_t failure_threshold = 3;
    static constexpr std::uint32_t retry_interval_ms = 1000;

    void reset() {
        state_ = {};
        next_recovery_ms_ = 0;
        recovery_deferred_ = false;
    }

    void set_startup_phase(StartupPhase phase) {
        state_.startup_phase = phase;
    }

    void set_lines(bool sda_high, bool scl_high) {
        state_.sda_high = sda_high;
        state_.scl_high = scl_high;
    }

    void waiting() {
        if (!state_.ever_ready) state_.health = Health::Waiting;
    }

    void note_startup_attempt(std::uint32_t now_ms) {
        increment(state_.startup_attempts);
        if (!state_.first_try_recorded) {
            state_.first_try_recorded = true;
            state_.first_try_ms = now_ms;
        }
    }

    void record_startup_failure(I2cError error) {
        state_.last_error = error;
        increment(state_.total_errors);
        increment(state_.consecutive_errors);
        state_.health = Health::Waiting;
    }

    void startup_ready(std::uint32_t now_ms) {
        state_.ever_ready = true;
        if (!state_.first_ack_recorded) {
            state_.first_ack_recorded = true;
            state_.first_ack_ms = now_ms;
        }
        state_.startup_phase = StartupPhase::Ready;
        ready();
    }

    void ready() {
        state_.health = Health::Ok;
        state_.consecutive_errors = 0;
    }

    bool record_failure(I2cError error, std::uint32_t now_ms) {
        state_.last_error = error;
        increment(state_.total_errors);
        increment(state_.consecutive_errors);
        state_.health =
            state_.consecutive_errors >= failure_threshold
                ? Health::Lost : Health::Degraded;
        return recovery_due(now_ms);
    }

    bool recovery_due(std::uint32_t now_ms) const {
        if (state_.health != Health::Lost) return false;
        return !recovery_deferred_ ||
            deadline_reached(now_ms, next_recovery_ms_);
    }

    void begin_recovery() {
        increment(state_.recovery_attempts);
        state_.health = Health::Recovering;
    }

    void finish_recovery(
        bool success,
        I2cError failure,
        std::uint32_t now_ms
    ) {
        if (success) {
            increment(state_.recoveries);
            state_.ever_ready = true;
            state_.startup_phase = StartupPhase::Ready;
            state_.health = Health::Ok;
            state_.consecutive_errors = 0;
            next_recovery_ms_ = 0;
            recovery_deferred_ = false;
            return;
        }

        if (failure != I2cError::None) {
            state_.last_error = failure;
            increment(state_.total_errors);
            increment(state_.consecutive_errors);
        }
        state_.health = Health::Lost;
        next_recovery_ms_ = now_ms + retry_interval_ms;
        recovery_deferred_ = true;
    }

    const Diagnostics& diagnostics() const {
        return state_;
    }

private:
    static bool deadline_reached(
        std::uint32_t now_ms,
        std::uint32_t deadline_ms
    ) {
        return static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
    }

    static void increment(std::uint32_t& value) {
        if (value != UINT32_MAX) ++value;
    }

    Diagnostics state_{};
    std::uint32_t next_recovery_ms_ = 0;
    bool recovery_deferred_ = false;
};

} // namespace detail
} // namespace rmb::picocalc::keyboard
