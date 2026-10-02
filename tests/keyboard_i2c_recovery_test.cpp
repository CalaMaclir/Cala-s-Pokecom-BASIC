#include <cassert>
#include <cstdint>
#include <cstdio>

#include "keyboard_i2c_recovery.hpp"

using rmb::picocalc::keyboard::Health;
using rmb::picocalc::keyboard::I2cError;
using rmb::picocalc::keyboard::StartupPhase;
using rmb::picocalc::keyboard::detail::RecoveryPolicy;

int main() {
    RecoveryPolicy policy;
    assert(policy.diagnostics().health == Health::Initializing);
    policy.set_startup_phase(StartupPhase::WaitBusIdle);
    policy.set_lines(true, false);
    policy.waiting();
    policy.note_startup_attempt(750);
    policy.record_startup_failure(I2cError::AddressNack);
    assert(policy.diagnostics().health == Health::Waiting);
    assert(policy.diagnostics().startup_attempts == 1);
    assert(policy.diagnostics().first_try_recorded);
    assert(policy.diagnostics().first_try_ms == 750);
    assert(policy.diagnostics().total_errors == 1);
    assert(!policy.recovery_due(5000));

    policy.startup_ready(930);
    assert(policy.diagnostics().health == Health::Ok);
    assert(policy.diagnostics().ever_ready);
    assert(policy.diagnostics().first_ack_recorded);
    assert(policy.diagnostics().first_ack_ms == 930);
    assert(policy.diagnostics().consecutive_errors == 0);

    assert(!policy.record_failure(I2cError::ReadTimeout, 1000));
    assert(policy.diagnostics().health == Health::Degraded);
    assert(!policy.record_failure(I2cError::ReadTimeout, 1010));
    assert(policy.record_failure(I2cError::ReadTimeout, 1020));
    assert(policy.diagnostics().health == Health::Lost);
    policy.begin_recovery();
    policy.finish_recovery(true, I2cError::None, 1020);
    assert(policy.diagnostics().health == Health::Ok);
    assert(policy.diagnostics().recovery_attempts == 1);
    assert(policy.diagnostics().recoveries == 1);
    assert(policy.diagnostics().last_error == I2cError::ReadTimeout);

    policy.reset();
    assert(!policy.record_failure(I2cError::AddressNack, 100));
    assert(!policy.record_failure(I2cError::AddressNack, 101));
    assert(policy.record_failure(I2cError::AddressNack, 102));
    policy.begin_recovery();
    policy.finish_recovery(false, I2cError::SdaStuck, 102);
    assert(policy.diagnostics().health == Health::Lost);
    assert(policy.diagnostics().total_errors == 4);
    assert(!policy.recovery_due(1101));
    assert(policy.recovery_due(1102));

    policy.reset();
    policy.startup_ready(UINT32_MAX - 700u);
    constexpr std::uint32_t near_wrap = UINT32_MAX - 500u;
    assert(!policy.record_failure(I2cError::ReadNack, near_wrap));
    assert(!policy.record_failure(I2cError::ReadNack, near_wrap + 1u));
    assert(policy.record_failure(I2cError::ReadNack, near_wrap + 2u));
    policy.begin_recovery();
    policy.finish_recovery(false, I2cError::SclStuck, near_wrap + 2u);
    assert(!policy.recovery_due(499u));
    assert(policy.recovery_due(501u));

    std::puts("keyboard I2C startup and recovery policy passed");
}
