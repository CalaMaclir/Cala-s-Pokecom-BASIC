#include "keyboard_i2c_fake.hpp"
#include "keyboard_i2c_read.hpp"

int main() {
    using namespace rmb::picocalc::keyboard;

    // Cold boot must be electrically quiet and backlight writes are queued.
    reset_fake_hardware(); init();
    assert(diag().health == Health::Initializing);
    assert(diag().startup_phase == StartupPhase::BootQuiet);
    assert(i2c_init_calls == 0 && i2c_write_calls == 0);
    assert(set_lcd_backlight(160));
    assert(i2c_write_calls == 0);
    fake_us = 749000u; assert(read_key() == -1);
    assert(i2c_init_calls == 0 && i2c_write_calls == 0);
    fake_us = 750000u; assert(read_key() == -1);
    assert(boot_until_ready());
    assert(first_write_us >= 780000u);
    assert(diag().startup_attempts == 1);
    assert(diag().first_try_recorded && diag().first_ack_recorded);
    assert(diag().first_ack_ms >= diag().first_try_ms + 16u);
    assert(diag().ever_ready && diag().health == Health::Ok);
    for (int i = 0; i < 4; ++i) {
        assert(read_key() == -1); sleep_ms(4);
    }
    assert(last_write_register == 0x85 && last_write_value == 160);

    // Empty FIFO is healthy, and a real event remains available.
    const auto errors_after_boot = diag().total_errors;
    for (int i = 0; i < 100; ++i) { assert(read_key() == -1); sleep_ms(4); }
    assert(diag().total_errors == errors_after_boot);
    key_events.emplace_back(1, 13); assert(poll_for_key() == 13);

    // Lines must remain continuously high before I2C1 is initialized.
    reset_fake_hardware(); sda_forced_low = true; init();
    service_keyboard_ms(1400);
    assert(i2c_init_calls == 0 && i2c_write_calls == 0);
    assert(diag().startup_phase == StartupPhase::WaitBusIdle);
    assert(!diag().sda_high && diag().scl_high);
    sda_forced_low = false;
    assert(boot_until_ready());

    // Before first ACK, address NACKs are passive: no GPIO clock recovery and
    // no active recovery counter. A later MCU response becomes READY.
    reset_fake_hardware(); controller_missing = true;
    write_failure_code = PICO_ERROR_GENERIC; init();
    service_keyboard_ms(3000);
    assert(!diag().ever_ready && diag().health == Health::Waiting);
    assert(diag().last_error == I2cError::AddressNack);
    assert(diag().startup_attempts >= 3);
    assert(diag().recovery_attempts == 0 && scl_low_pulses == 0);
    controller_missing = false;
    assert(boot_until_ready(3000));
    assert(diag().ever_ready && diag().first_ack_recorded);

    // Auxiliary success must not hide repeated key-register failures.
    int battery_percent; bool battery_charging;
    for (unsigned failure = 1; failure <= 2; ++failure) {
        pointer_failures = 1;
        assert(read_key() == -1);
        assert(read_battery(battery_percent, battery_charging));
        assert(diag().consecutive_errors == failure);
        assert(diag().health == Health::Degraded);
    }
    pointer_failures = 1;
    assert(read_key() == -1);
    assert(diag().health == Health::Lost);
    const auto recovery_deadline = fake_us + 300000u;
    while (diag().recoveries == 0 && fake_us < recovery_deadline) {
        assert(read_key() == -1); sleep_ms(4);
    }
    assert(diag().recoveries == 1 && diag().health == Health::Ok);

    // Runtime stuck SDA uses the active nine-clock recovery and backoff.
    pointer_failures = 3; sda_forced_low = true;
    for (int i = 0; i < 4; ++i) assert(read_key() == -1);
    assert(diag().health == Health::Lost);
    assert(diag().last_error == I2cError::SdaStuck && scl_low_pulses >= 9);
    const std::uint32_t recovery_attempts = diag().recovery_attempts;
    const auto backoff_start = fake_us;
    fake_us = backoff_start + 999000u;
    assert(read_key() == -1 && diag().recovery_attempts == recovery_attempts);
    fake_us = backoff_start + 1000000u;
    assert(read_key() == -1 && diag().recovery_attempts == recovery_attempts + 1);
    sda_forced_low = false; sleep_ms(1000);
    const auto second_recovery_deadline = fake_us + 300000u;
    while (diag().health != Health::Ok && fake_us < second_recovery_deadline) {
        assert(read_key() == -1); sleep_ms(4);
    }
    assert(diag().health == Health::Ok);

    // The SDK read preflight remains bounded with a full TX FIFO.
    tx_fifo_full = true;
    const auto before = fake_us; const int reads = i2c_read_calls;
    std::uint8_t data[2] = {};
    assert(detail::bounded_i2c_read(i2c1, 0x1f, data, 2, 5000)
        == PICO_ERROR_TIMEOUT);
    assert(fake_us - before == 5000u && i2c_read_calls == reads);
    tx_fifo_full = false;

    // Safe BIOS identification neither consumes FIFO nor clears key failures.
    key_events.emplace_back(1, 'v');
    unsigned char bios = 0xff;
    assert(read_bios_version(bios) && bios == 0x16);
    assert(selected_register == 0x01 && key_events.size() == 1);
    const int cached_writes = i2c_write_calls;
    assert(read_bios_version(bios) && i2c_write_calls == cached_writes);
    key_events.emplace_back(1, 'a'); assert(read_key() == -1);
    const int selected = selected_register;
    int percent; bool charging;
    assert(!read_battery(percent, charging));
    unsigned char level; assert(!get_lcd_backlight(level));
    RtcDateTime rtc; assert(!read_rtc(rtc));
    assert(selected_register == selected);
    reconfigure_bus_clock(); assert(baud_calls == 1);
    assert(poll_for_key() == 'v');

    std::puts("keyboard cold-boot startup and bounded recovery passed");
}
