#include "keyboard_i2c_fake.hpp"
#include "keyboard_i2c_read.hpp"
#include "input_hotkeys.hpp"

int main() {
    using namespace rmb::picocalc::keyboard;

    // Execute actual MCU event decoding, including state-2 repeat suppression.
    reset_fake_hardware();init();assert(boot_until_ready());
    for (bool caps : {false,true}) for (bool shift : {false,true}) {
        set_caps_lock(caps);
        key_events.emplace_back(1,0xa1);
        key_events.emplace_back(shift?1:3,0xa2);
        for (char letter : {'e','E','r','R','c','C','m','M','u','U','s','S','p','P'}) {
            key_events.emplace_back(1,letter);
            assert(poll_for_key()==rmb::input_hotkeys::alt_letter(letter));
            key_events.emplace_back(2,letter);
            key_events.emplace_back(3,letter);
            key_events.emplace_back(1,'?');
            assert(poll_for_key()=='?');
        }
        key_events.emplace_back(3,0xa1);key_events.emplace_back(3,0xa2);
        key_events.emplace_back(1,0xd2);assert(poll_for_key()==0xd2);
        key_events.emplace_back(1,' ');assert(poll_for_key()==' ');
        assert(rmb::input_hotkeys::alt_letter('b')==-1);
    }

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

    // Startup failures reinitialize only the RP2350 controller. No clocks or
    // STOP are driven, and an unread MCU key survives until READY.
    for (bool scl_low : {false, true}) {
        reset_fake_hardware();
        sda_forced_low = !scl_low; scl_forced_low = scl_low; init();
        service_keyboard_ms(3000);
        assert(!diag().ever_ready && i2c_init_calls == 0 && scl_low_pulses == 0);
        sda_forced_low = scl_forced_low = false;
        key_events.emplace_back(1, 'l');
        assert(boot_until_ready() && poll_for_key() == 'l');
    }
    for (int fault = 0; fault < 4; ++fault) {
        reset_fake_hardware(); init();
        if (fault == 0) pointer_failures = 1;
        if (fault == 1) read_failures = 1;
        if (fault == 2) short_reads = 1;
        if (fault == 3) { service_keyboard_ms(800); tx_fifo_full = true; }
        service_keyboard_ms(950);
        assert(diag().total_errors >= 1);
        assert(diag().recovery_attempts == 0 && scl_low_pulses == 0);
        assert(i2c_deinit_calls >= 1);
        tx_fifo_full = false;
        assert(boot_until_ready());
    }
    reset_fake_hardware(); controller_missing = true;
    write_failure_code = PICO_ERROR_GENERIC; init();
    service_keyboard_ms(600000); // Ten simulated minutes, no wall-clock sleep.
    assert(!diag().ever_ready && scl_low_pulses == 0);
    assert(diag().startup_attempts > 500 && diag().startup_attempts < 700);
    assert(diag().total_errors == diag().startup_attempts);
    assert(i2c_deinit_calls > 500 && diag().recovery_attempts == 0);
    controller_missing = false;
    assert(boot_until_ready(2000));

    // LOST releases host repeat/modifiers immediately, even during failed
    // recovery. Repeated successful recovery never consumes the key FIFO.
    for (int cycle = 0; cycle < 3; ++cycle) {
        key_events.emplace_back(1, 0xa1); // Alt
        key_events.emplace_back(1, 0xa2); // Shift
        key_events.emplace_back(1, 0xb7); // Right
        assert(poll_for_key() == 0xb7);
        set_caps_lock(true);
        assert(navigation_key_held(0xb7) && shift_held());
        controller_missing = true; write_failure_code = PICO_ERROR_GENERIC;
        for (int i = 0; i < 3; ++i) assert(read_key() == -1);
        assert(diag().health == Health::Lost);
        assert(!navigation_key_held(0xb7) && !shift_held() && !caps_lock_enabled());
        service_keyboard_ms(1500);
        assert(diag().health == Health::Lost && !navigation_key_held(0xb7));
        controller_missing = false;
        key_events.emplace_back(1, 'e');
        assert(poll_for_key(600) == 'e'); // no stale Alt+E or Shift
        assert(diag().health == Health::Ok);
        assert(key_events.empty());
    }

    // Actual startup + runtime deadlines cross 32-bit millis rollover.
    reset_fake_hardware();
    fake_us = static_cast<std::uint64_t>(UINT32_MAX - 400u) * 1000u;
    init(); assert(boot_until_ready());
    fake_us = static_cast<std::uint64_t>(UINT32_MAX) * 1000u * 2u - 500000u;
    pointer_failures = 3;
    for (int i = 0; i < 3; ++i) assert(read_key() == -1);
    controller_missing = true; service_keyboard_ms(700);
    controller_missing = false; key_events.emplace_back(1, 'w');
    assert(poll_for_key(600) == 'w');

    // Reclocking at every supported CPU profile preserves pending responses.
    for (int mhz : {75, 100, 150, 200}) {
        (void)mhz; // fake i2c_set_baudrate checks the 10 kHz contract.
        key_events.emplace_back(1, 't'); assert(read_key() == -1);
        const auto pending_register = selected_register;
        reconfigure_bus_clock();
        assert(selected_register == pending_register && key_events.empty());
        assert(poll_for_key() == 't');
    }
    assert(baud_calls == 4);

    std::puts("keyboard cold-boot startup and bounded recovery passed");
}
