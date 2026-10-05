#include "picocalc_keyboard.hpp"
#include "keyboard_i2c_read.hpp"

#include <cstdint>

#include "hardware/i2c.h"
#include "pico/error.h"
#include "pico/stdlib.h"

namespace rmb::picocalc::keyboard {

namespace {
i2c_inst_t* const bus = i2c1;
constexpr uint sda_pin = 6;
constexpr uint scl_pin = 7;
constexpr std::uint32_t bus_hz = 10000;
constexpr std::uint8_t address = 0x1f;
constexpr std::uint8_t key_register = 0x09;
constexpr std::uint8_t bios_version_register = 0x01;
constexpr std::uint8_t lcd_backlight_register = 0x05;
constexpr std::uint8_t keyboard_backlight_register = 0x0a;
constexpr std::uint8_t battery_register = 0x0b;

constexpr std::uint8_t key_mod_alt = 0xa1;
constexpr std::uint8_t key_mod_shl = 0xa2;
constexpr std::uint8_t key_mod_shr = 0xa3;
constexpr std::uint8_t key_mod_ctrl = 0xa5;
constexpr std::uint8_t key_caps_lock = 0xc1;

constexpr std::uint32_t boot_quiet_ms = 750;
constexpr std::uint32_t bus_idle_stable_ms = 30;
constexpr std::uint32_t safe_probe_settle_ms = 16;

bool initialized = false;
bool ctrl_held = false;
bool shift_left_held = false;
bool shift_right_held = false;
bool alt_held = false;
bool caps_lock = false;
int held_navigation_key = -1;
bool returned_repeat = false;
RtcError rtc_error = RtcError::None;
detail::RecoveryPolicy recovery_policy;
StartupPhase startup_phase = StartupPhase::BootQuiet;
std::uint32_t startup_deadline_ms = 0;
std::uint32_t bus_idle_since_ms = 0;
std::uint32_t startup_probe_due_ms = 0;
std::uint32_t startup_backoff_ms = 100;
bool bus_idle_tracking = false;
bool key_read_pending = false;
std::uint32_t key_read_due_ms = 0;
bool recovery_probe_pending = false;
bool recovery_probe_read_pending = false;
std::uint32_t recovery_probe_due_ms = 0;
bool bios_query_pending = false;
bool bios_reply_received = false;
std::uint8_t cached_bios_version = 0;
bool lcd_backlight_known = false;
bool lcd_backlight_pending = false;
std::uint8_t desired_lcd_backlight = 160;
bool keyboard_backlight_known = false;
bool keyboard_backlight_pending = false;
std::uint8_t desired_keyboard_backlight = 0;

bool navigation_repeatable(int key) {
    return key == 0xb4 || key == 0xb5 || key == 0xb6 || key == 0xb7;
}

std::uint8_t bcd_to_bin(std::uint8_t value) {
    return static_cast<std::uint8_t>(
        ((value >> 4) * 10u) + (value & 0x0fu));
}

std::uint8_t bin_to_bcd(int value) {
    return static_cast<std::uint8_t>(
        ((value / 10) << 4) | (value % 10));
}

std::uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

void reset_host_key_state() {
    caps_lock = false;
    shift_left_held = false;
    shift_right_held = false;
    alt_held = false;
    ctrl_held = false;
    held_navigation_key = -1;
}

void record_runtime_failure(I2cError error) {
    recovery_policy.record_failure(error, now_ms());
    if (recovery_policy.diagnostics().health == Health::Lost) {
        // Release host-side keys immediately, even if recovery never succeeds.
        // Do not select/drain the MCU FIFO or discard queued backlight desires.
        reset_host_key_state();
        bios_reply_received = false;
        bios_query_pending = false;
    }
}

void configure_i2c_bus() {
    i2c_init(bus, bus_hz);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
}

bool deadline_reached(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

I2cError transfer_error(int result, int expected, bool write) {
    if (result == expected) return I2cError::None;
    if (result == PICO_ERROR_TIMEOUT) {
        return write ? I2cError::WriteTimeout : I2cError::ReadTimeout;
    }
    if (write && result == PICO_ERROR_GENERIC) {
        // Pico SDK uses GENERIC for a 7-bit address NACK, but also for an
        // unclassified TX abort. Keep that ambiguity visible in the label.
        return I2cError::AddressNack;
    }
    if (write && result >= 0 && result < expected) {
        return I2cError::DataNack;
    }
    if (result < 0) {
        return write ? I2cError::WriteNack : I2cError::ReadNack;
    }
    return write ? I2cError::ShortWrite : I2cError::ShortRead;
}

I2cError read_register_raw(
    std::uint8_t reg_id,
    std::uint8_t (&data)[2],
    std::uint32_t settle_ms
) {
    if (!initialized) return I2cError::NotInitialized;

    std::uint8_t reg = reg_id;
    int result = i2c_write_timeout_us(
        bus, address, &reg, 1, false, 5000);
    I2cError error = transfer_error(result, 1, true);
    if (error != I2cError::None) return error;

    if (settle_ms != 0) sleep_ms(settle_ms);

    result = detail::bounded_i2c_read(
        bus, address, data, 2, 5000);
    return transfer_error(result, 2, false);
}

I2cError write_register_raw(std::uint8_t reg_id, std::uint8_t value) {
    if (!initialized) return I2cError::NotInitialized;

    std::uint8_t message[2] = {
        static_cast<std::uint8_t>(reg_id | 0x80u),
        value
    };
    const int result = i2c_write_timeout_us(
        bus, address, message, 2, false, 5000);
    return transfer_error(result, 2, true);
}

void release_line(uint pin) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void drive_line_low(uint pin) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
}

bool wait_line_high(uint pin) {
    for (int i = 0; i < 100; ++i) {
        if (gpio_get(pin)) return true;
        sleep_us(1);
    }
    return false;
}

bool recover_i2c_bus(I2cError& failure) {
    if (initialized) i2c_deinit(bus);
    initialized = false;

    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    release_line(sda_pin);
    release_line(scl_pin);
    sleep_us(10);

    bool physical_ok = true;
    if (!wait_line_high(scl_pin)) {
        failure = I2cError::SclStuck;
        physical_ok = false;
    }

    if (physical_ok && !gpio_get(sda_pin)) {
        for (int pulse = 0; pulse < 9 && !gpio_get(sda_pin); ++pulse) {
            drive_line_low(scl_pin);
            sleep_us(50);
            release_line(scl_pin);
            if (!wait_line_high(scl_pin)) {
                failure = I2cError::SclStuck;
                physical_ok = false;
                break;
            }
            sleep_us(50);
        }
    }

    if (physical_ok) {
        // Generate a STOP condition without resetting the keyboard MCU:
        // SDA low while SCL is low, then release SCL followed by SDA.
        drive_line_low(scl_pin);
        drive_line_low(sda_pin);
        sleep_us(50);
        release_line(scl_pin);
        if (!wait_line_high(scl_pin)) {
            failure = I2cError::SclStuck;
            physical_ok = false;
        } else {
            sleep_us(50);
            release_line(sda_pin);
            sleep_us(50);
            if (!gpio_get(sda_pin)) {
                failure = I2cError::SdaStuck;
                physical_ok = false;
            }
        }
    }

    configure_i2c_bus();
    initialized = true;
    if (!physical_ok) return false;

    return true;
}

void set_startup_phase(StartupPhase phase) {
    startup_phase = phase;
    recovery_policy.set_startup_phase(phase);
}

void update_line_diagnostics() {
    recovery_policy.set_lines(gpio_get(sda_pin), gpio_get(scl_pin));
}

void prepare_released_bus() {
    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    release_line(sda_pin);
    release_line(scl_pin);
    update_line_diagnostics();
}

void schedule_startup_backoff(I2cError error, std::uint32_t now) {
    recovery_policy.record_startup_failure(error);
    startup_deadline_ms = now + startup_backoff_ms;
    if (startup_backoff_ms < 250u) startup_backoff_ms = 250u;
    else if (startup_backoff_ms < 500u) startup_backoff_ms = 500u;
    else startup_backoff_ms = 1000u;
    set_startup_phase(StartupPhase::PassiveBackoff);
}

bool service_startup() {
    if (startup_phase == StartupPhase::Ready) return false;

    const std::uint32_t now = now_ms();
    update_line_diagnostics();
    switch (startup_phase) {
    case StartupPhase::BootQuiet:
        if (!deadline_reached(now, startup_deadline_ms)) return true;
        recovery_policy.waiting();
        bus_idle_tracking = false;
        set_startup_phase(StartupPhase::WaitBusIdle);
        return true;

    case StartupPhase::WaitBusIdle: {
        const auto& state = recovery_policy.diagnostics();
        if (!state.sda_high || !state.scl_high) {
            bus_idle_tracking = false;
            return true;
        }
        if (!bus_idle_tracking) {
            bus_idle_tracking = true;
            bus_idle_since_ms = now;
            return true;
        }
        if (static_cast<std::uint32_t>(now - bus_idle_since_ms) <
            bus_idle_stable_ms) return true;
        set_startup_phase(StartupPhase::InitController);
        return true;
    }

    case StartupPhase::InitController:
        configure_i2c_bus();
        initialized = true;
        set_startup_phase(StartupPhase::ProbeSelect);
        return true;

    case StartupPhase::ProbeSelect: {
        recovery_policy.note_startup_attempt(now);
        std::uint8_t reg = lcd_backlight_register;
        const I2cError error = transfer_error(
            i2c_write_timeout_us(bus, address, &reg, 1, false, 5000),
            1,
            true
        );
        if (error == I2cError::None) {
            startup_probe_due_ms = now + safe_probe_settle_ms;
            set_startup_phase(StartupPhase::ProbeWait);
        } else {
            schedule_startup_backoff(error, now);
        }
        return true;
    }

    case StartupPhase::ProbeWait:
        if (!deadline_reached(now, startup_probe_due_ms)) return true;
        set_startup_phase(StartupPhase::ProbeRead);
        return true;

    case StartupPhase::ProbeRead: {
        std::uint8_t data[2] = {};
        const I2cError error = transfer_error(
            detail::bounded_i2c_read(bus, address, data, 2, 5000),
            2,
            false
        );
        if (error == I2cError::None) {
            recovery_policy.startup_ready(now);
            set_startup_phase(StartupPhase::Ready);
            startup_backoff_ms = 100;
            reset_host_key_state();
            bios_reply_received = false;
            bios_query_pending = false;
            if (lcd_backlight_known) lcd_backlight_pending = true;
            if (keyboard_backlight_known) keyboard_backlight_pending = true;
        } else {
            schedule_startup_backoff(error, now);
        }
        return true;
    }

    case StartupPhase::PassiveBackoff:
        if (!deadline_reached(now, startup_deadline_ms)) return true;
        if (initialized) i2c_deinit(bus);
        initialized = false;
        prepare_released_bus();
        bus_idle_tracking = false;
        recovery_policy.waiting();
        set_startup_phase(StartupPhase::WaitBusIdle);
        return true;

    case StartupPhase::Ready:
        return false;
    }
    return true;
}

bool service_pending_backlight() {
    const auto& state = recovery_policy.diagnostics();
    if (!state.ever_ready || state.health != Health::Ok ||
        key_read_pending || recovery_probe_pending ||
        recovery_probe_read_pending) return false;

    if (lcd_backlight_pending) {
        const I2cError error = write_register_raw(
            lcd_backlight_register, desired_lcd_backlight);
        if (error == I2cError::None) lcd_backlight_pending = false;
        else record_runtime_failure(error);
        return true;
    }
    if (keyboard_backlight_pending) {
        const I2cError error = write_register_raw(
            keyboard_backlight_register, desired_keyboard_backlight);
        if (error == I2cError::None) keyboard_backlight_pending = false;
        else record_runtime_failure(error);
        return true;
    }
    return false;
}

// Each key poll performs one bounded phase. Startup address NACKs are passive:
// GPIO clocks/STOP are used only after this controller has once been healthy.
bool service_recovery() {
    const std::uint32_t now = now_ms();
    if (recovery_probe_read_pending) {
        if (!deadline_reached(now, recovery_probe_due_ms)) return true;
        std::uint8_t data[2] = {};
        const I2cError failure = transfer_error(
            detail::bounded_i2c_read(bus, address, data, 2, 5000),
            2,
            false
        );
        recovery_probe_read_pending = false;
        recovery_policy.finish_recovery(
            failure == I2cError::None, failure, now);
        if (failure == I2cError::None) {
            reset_host_key_state();
            bios_reply_received = false;
            bios_query_pending = false;
            if (lcd_backlight_known) lcd_backlight_pending = true;
            if (keyboard_backlight_known) keyboard_backlight_pending = true;
        }
        return true;
    }
    if (recovery_probe_pending) {
        std::uint8_t reg = lcd_backlight_register;
        const I2cError failure = transfer_error(
            i2c_write_timeout_us(bus, address, &reg, 1, false, 5000),
            1,
            true
        );
        recovery_probe_pending = false;
        if (failure == I2cError::None) {
            recovery_probe_read_pending = true;
            recovery_probe_due_ms = now + safe_probe_settle_ms;
        } else {
            recovery_policy.finish_recovery(false, failure, now);
        }
        return true;
    }
    if (!recovery_policy.recovery_due(now)) return false;
    recovery_policy.begin_recovery();
    key_read_pending = false;
    bios_query_pending = false;
    I2cError failure = I2cError::None;
    if (!recover_i2c_bus(failure)) {
        recovery_policy.finish_recovery(false, failure, now_ms());
    } else {
        update_line_diagnostics();
        recovery_probe_pending = true;
    }
    return true;
}

template <typename Operation>
bool perform_keyboard_transaction(Operation operation) {
    const auto& state = recovery_policy.diagnostics();
    // Another register selection would overwrite a pending response. Auxiliary
    // operations also wait until the cold-boot handshake has succeeded.
    if (!state.ever_ready || startup_phase != StartupPhase::Ready ||
        key_read_pending || recovery_probe_pending ||
        recovery_probe_read_pending || state.health == Health::Lost ||
        state.health == Health::Recovering) return false;
    const I2cError error = operation();
    if (error == I2cError::None) {
        // Battery/backlight success does not prove the key FIFO is working.
        // Only a successful key response or explicit probe resets key health.
        return true;
    }
    record_runtime_failure(error);
    return false;
}

bool read_u8_register(std::uint8_t reg_id, std::uint8_t& value) {
    std::uint8_t data[2] = {};
    const bool ok = perform_keyboard_transaction([&]() {
        return read_register_raw(reg_id, data, 2);
    });
    if (ok) value = data[1];
    return ok;
}

bool write_u8_register(std::uint8_t reg_id, std::uint8_t value) {
    return perform_keyboard_transaction([&]() {
        return write_register_raw(reg_id, value);
    });
}

bool poll_key_event(std::uint8_t& state, int& code) {
    if (service_startup()) return false;
    if (service_recovery()) return false;
    const Health health = recovery_policy.diagnostics().health;
    if (health == Health::Lost || health == Health::Recovering) return false;
    if (service_pending_backlight()) return false;
    if (bios_query_pending && !key_read_pending) {
        bios_query_pending = false;
        unsigned char ignored = 0;
        (void)read_bios_version(ignored);
        return false;
    }
    I2cError error = I2cError::None;
    if (!key_read_pending) {
        std::uint8_t reg = key_register;
        const int result = i2c_write_timeout_us(
            bus, address, &reg, 1, false, 5000);
        error = transfer_error(result, 1, true);
        if (error == I2cError::None) {
            // ClockworkPi's reference driver allows 16 ms for MCU handling.
            key_read_pending = true;
            key_read_due_ms = now_ms() + 16u;
            return false;
        }
    } else {
        if (!deadline_reached(now_ms(), key_read_due_ms)) return false;
        std::uint8_t data[2] = {};
        const int result = detail::bounded_i2c_read(
            bus, address, data, 2, 5000);
        key_read_pending = false;
        error = transfer_error(result, 2, false);
        if (error == I2cError::None) {
            recovery_policy.ready(); // state 0 is a successful empty FIFO.
            state = data[0];
            code = data[1];
            return true;
        }
    }
    record_runtime_failure(error);
    return false;
}

} // namespace

void init() {
    if (initialized) i2c_deinit(bus);
    initialized = false;
    recovery_policy.reset();
    startup_phase = StartupPhase::BootQuiet;
    startup_deadline_ms = now_ms() + boot_quiet_ms;
    bus_idle_since_ms = 0;
    startup_probe_due_ms = 0;
    startup_backoff_ms = 100;
    bus_idle_tracking = false;
    key_read_pending = false;
    recovery_probe_pending = false;
    recovery_probe_read_pending = false;
    recovery_probe_due_ms = 0;
    bios_query_pending = false;
    bios_reply_received = false;
    cached_bios_version = 0;
    lcd_backlight_known = false;
    lcd_backlight_pending = false;
    desired_lcd_backlight = 160;
    keyboard_backlight_known = false;
    keyboard_backlight_pending = false;
    desired_keyboard_backlight = 0;
    reset_host_key_state();

    // Do not drive I2C during the keyboard MCU / PICO_EN / pull-up power race.
    // Runtime polling advances the finite startup state machine later.
    prepare_released_bus();
    recovery_policy.set_startup_phase(StartupPhase::BootQuiet);
}

void reconfigure_bus_clock() {
    if (!initialized) return;

    // RP2350 I2C timing is derived from clk_sys. CPU speed changes therefore
    // require the divider to be recomputed even though the requested bus rate
    // remains unchanged.
    i2c_set_baudrate(bus, bus_hz);
}

bool set_lcd_backlight(unsigned char value) {
    desired_lcd_backlight = static_cast<std::uint8_t>(value);
    lcd_backlight_known = true;
    lcd_backlight_pending = true;
    const auto& state = recovery_policy.diagnostics();
    if (!state.ever_ready || startup_phase != StartupPhase::Ready) return true;
    if (write_u8_register(lcd_backlight_register, desired_lcd_backlight)) {
        lcd_backlight_pending = false;
        return true;
    }
    return false;
}

bool get_lcd_backlight(unsigned char& value) {
    const auto& state = recovery_policy.diagnostics();
    if (!state.ever_ready || startup_phase != StartupPhase::Ready ||
        lcd_backlight_pending) {
        if (!lcd_backlight_known) return false;
        value = desired_lcd_backlight;
        return true;
    }
    std::uint8_t current = 0;
    if (!read_u8_register(lcd_backlight_register, current)) return false;
    value = current;
    desired_lcd_backlight = current;
    lcd_backlight_known = true;
    return true;
}

bool cached_bios_version_value(unsigned char& value) {
    value = cached_bios_version;
    return bios_reply_received;
}

bool read_bios_version(unsigned char& value) {
    if (bios_reply_received) {
        value = cached_bios_version;
        return true;
    }
    const auto& diagnostic = recovery_policy.diagnostics();
    if (!diagnostic.ever_ready || startup_phase != StartupPhase::Ready) {
        bios_query_pending = true;
        return false;
    }
    const Health health = diagnostic.health;
    if (health == Health::Lost || health == Health::Recovering) return false;
    if (key_read_pending) {
        bios_query_pending = true;
        return false;
    }
    std::uint8_t data[2] = {};
    if (!perform_keyboard_transaction([&]() {
            return read_register_raw(bios_version_register, data, 2);
        })) {
        return false;
    }
    // Official v1.6 replies [0, BIOSVERSION]. Older firmware's default
    // register handler replies [0, 0]; that is not an I2C error or a version.
    if (data[0] != 0) return false;
    bios_reply_received = true;
    cached_bios_version = data[1];
    value = cached_bios_version;
    return true;
}

bool set_keyboard_backlight(unsigned char value) {
    desired_keyboard_backlight = static_cast<std::uint8_t>(value);
    keyboard_backlight_known = true;
    keyboard_backlight_pending = true;
    const auto& state = recovery_policy.diagnostics();
    if (!state.ever_ready || startup_phase != StartupPhase::Ready) return true;
    if (write_u8_register(
            keyboard_backlight_register, desired_keyboard_backlight)) {
        keyboard_backlight_pending = false;
        return true;
    }
    return false;
}

bool get_keyboard_backlight(unsigned char& value) {
    const auto& state = recovery_policy.diagnostics();
    if (!state.ever_ready || startup_phase != StartupPhase::Ready ||
        keyboard_backlight_pending) {
        if (!keyboard_backlight_known) return false;
        value = desired_keyboard_backlight;
        return true;
    }
    std::uint8_t current = 0;
    if (!read_u8_register(keyboard_backlight_register, current)) return false;
    value = current;
    desired_keyboard_backlight = current;
    keyboard_backlight_known = true;
    return true;
}

bool read_battery(int& percent, bool& charging) {
    percent = -1;
    charging = false;

    std::uint8_t data[2] = {};
    const bool ok = perform_keyboard_transaction([&]() {
        return read_register_raw(battery_register, data, 16);
    });
    if (!ok) return false;

    // Controller returns register id in byte 0 and battery/charging in byte 1.
    const std::uint8_t raw = data[1];
    charging = (raw & 0x80u) != 0;
    percent = static_cast<int>(raw & 0x7fu);
    if (percent > 100) percent = 100;
    return true;
}

bool caps_lock_enabled() {
    return caps_lock;
}

void set_caps_lock(bool enabled) {
    caps_lock = enabled;
}

bool shift_held() {
    return shift_left_held || shift_right_held;
}

bool navigation_key_held(int key) {
    return held_navigation_key == key;
}

bool read_rtc(RtcDateTime& value, unsigned char rtc_device_address) {
    if (!initialized || !recovery_policy.diagnostics().ever_ready ||
        startup_phase != StartupPhase::Ready || key_read_pending ||
        recovery_probe_pending || recovery_probe_read_pending) {
        rtc_error = RtcError::NotInitialized;
        return false;
    }

    // The keyboard controller and battery gauge share this I2C bus. Never
    // deinit/reclock the bus just because the optional PCF8563 does not ACK.
    // A failed RTC probe must leave keyboard input completely untouched.
    constexpr std::uint32_t rtc_timeout_us = 20000;

    std::uint8_t reg = 0x02;
    const int written = i2c_write_timeout_us(
        bus,
        rtc_device_address,
        &reg,
        1,
        false,
        rtc_timeout_us
    );

    if (written != 1) {
        rtc_error = RtcError::PointerWriteFailed;
        return false;
    }

    sleep_ms(2);

    std::uint8_t data[7] = {};
    const int read = detail::bounded_i2c_read(
        bus,
        rtc_device_address,
        data,
        sizeof(data),
        rtc_timeout_us
    );

    if (read != static_cast<int>(sizeof(data))) {
        rtc_error = RtcError::ReadFailed;
        return false;
    }

    RtcDateTime candidate;
    candidate.second = bcd_to_bin(data[0] & 0x7fu);
    candidate.minute = bcd_to_bin(data[1] & 0x7fu);
    candidate.hour = bcd_to_bin(data[2] & 0x3fu);
    candidate.day = bcd_to_bin(data[3] & 0x3fu);
    candidate.month = bcd_to_bin(data[5] & 0x1fu);
    candidate.year = 2000 + bcd_to_bin(data[6]);

    if (candidate.month < 1 || candidate.month > 12 ||
        candidate.day < 1 || candidate.day > 31 ||
        candidate.hour > 23 ||
        candidate.minute > 59 ||
        candidate.second > 59) {
        rtc_error = RtcError::ReadFailed;
        return false;
    }

    value = candidate;
    rtc_error = RtcError::None;
    return true;
}

bool write_rtc(
    const RtcDateTime& value,
    unsigned char rtc_device_address
) {
    if (!initialized || !recovery_policy.diagnostics().ever_ready ||
        startup_phase != StartupPhase::Ready || key_read_pending ||
        recovery_probe_pending || recovery_probe_read_pending) {
        rtc_error = RtcError::NotInitialized;
        return false;
    }
    if (value.year < 2000 || value.year > 2099 ||
        value.month < 1 || value.month > 12 ||
        value.day < 1 || value.day > 31 ||
        value.hour < 0 || value.hour > 23 ||
        value.minute < 0 || value.minute > 59 ||
        value.second < 0 || value.second > 59) {
        rtc_error = RtcError::InvalidValue;
        return false;
    }

    std::uint8_t data[10] = {
        0x00,
        0x00,
        0x00,
        bin_to_bcd(value.second),
        bin_to_bcd(value.minute),
        bin_to_bcd(value.hour),
        bin_to_bcd(value.day),
        0x01,
        bin_to_bcd(value.month),
        bin_to_bcd(value.year - 2000)
    };

    constexpr std::uint32_t rtc_timeout_us = 20000;
    const int written = i2c_write_timeout_us(
        bus,
        rtc_device_address,
        data,
        sizeof(data),
        false,
        rtc_timeout_us
    );

    if (written != static_cast<int>(sizeof(data))) {
        rtc_error = RtcError::WriteFailed;
        return false;
    }

    sleep_ms(20);

    RtcDateTime verify;
    if (!read_rtc(verify, rtc_device_address)) {
        rtc_error = RtcError::ReadbackFailed;
        return false;
    }

    if (verify.year != value.year ||
        verify.month != value.month ||
        verify.day != value.day ||
        verify.hour != value.hour ||
        verify.minute != value.minute) {
        rtc_error = RtcError::ReadbackMismatch;
        return false;
    }

    rtc_error = RtcError::None;
    return true;
}

RtcError last_rtc_error() {
    return rtc_error;
}

const Diagnostics& diagnostics() {
    return recovery_policy.diagnostics();
}

const char* health_name(Health health) {
    switch (health) {
    case Health::Initializing: return "INITIALIZING";
    case Health::Waiting: return "WAIT MCU";
    case Health::Ok: return "OK";
    case Health::Degraded: return "DEGRADED";
    case Health::Recovering: return "RECOVERING";
    case Health::Lost: return "LOST";
    }
    return "UNKNOWN";
}

const char* startup_phase_name(StartupPhase phase) {
    switch (phase) {
    case StartupPhase::BootQuiet: return "BOOT QUIET";
    case StartupPhase::WaitBusIdle: return "WAIT BUS";
    case StartupPhase::InitController: return "INIT I2C";
    case StartupPhase::ProbeSelect: return "PROBE SELECT";
    case StartupPhase::ProbeWait: return "PROBE WAIT";
    case StartupPhase::ProbeRead: return "PROBE READ";
    case StartupPhase::Ready: return "READY";
    case StartupPhase::PassiveBackoff: return "BACKOFF";
    }
    return "UNKNOWN";
}

const char* error_name(I2cError error) {
    switch (error) {
    case I2cError::None: return "NONE";
    case I2cError::NotInitialized: return "NOT INITIALIZED";
    case I2cError::AddressNack: return "ADDR NACK/ABORT";
    case I2cError::DataNack: return "DATA NACK";
    case I2cError::WriteNack: return "WRITE NACK";
    case I2cError::WriteTimeout: return "WRITE TIMEOUT";
    case I2cError::ShortWrite: return "SHORT WRITE";
    case I2cError::ReadNack: return "READ NACK";
    case I2cError::ReadTimeout: return "READ TIMEOUT";
    case I2cError::ShortRead: return "SHORT READ";
    case I2cError::SclStuck: return "SCL STUCK";
    case I2cError::SdaStuck: return "SDA STUCK";
    }
    return "UNKNOWN";
}

bool poll_pending() {
    return startup_phase != StartupPhase::Ready || key_read_pending ||
        recovery_probe_pending || recovery_probe_read_pending ||
        bios_query_pending || lcd_backlight_pending ||
        keyboard_backlight_pending;
}

bool last_key_repeat() { return returned_repeat; }

int read_key() {
    returned_repeat = false;
    std::uint8_t state = 0;
    int c = 0;
    if (!poll_key_event(state, c)) return -1;
    if (state == 0 || c == 0) return -1;

    // Modifier events are independent events in the official PicoCalc
    // keyboard firmware. Track them host-side so character case is reliable
    // even if the MCU's Caps/Shift state has become confusing.
    if (c == key_mod_ctrl || c == 0x7e) {
        if (state == 1) ctrl_held = true;
        else if (state == 3) ctrl_held = false;
        return -1;
    }
    if (c == key_mod_shl) {
        if (state == 1) shift_left_held = true;
        else if (state == 3) shift_left_held = false;
        return -1;
    }
    if (c == key_mod_shr) {
        if (state == 1) shift_right_held = true;
        else if (state == 3) shift_right_held = false;
        return -1;
    }
    if (c == key_mod_alt) {
        if (state == 1) alt_held = true;
        else if (state == 3) alt_held = false;
        return -1;
    }
    if (c == key_caps_lock) {
        if (state == 1) {
            caps_lock = !caps_lock;
            return key_caps_lock;
        }
        return -1;
    }

    if (state == 3) {
        if (held_navigation_key == c) held_navigation_key = -1;
        return -1;
    }
    // State 2 is emitted by the keyboard MCU while a key is held. The editor
    // drains these events without drawing and renders once after state 3.
    if (state == 2) {
        returned_repeat = navigation_repeatable(c);
        return returned_repeat ? c : -1;
    }
    if (state != 1) return -1;
    if (navigation_repeatable(c)) held_navigation_key = c;

    // The current PicoCalc keyboard firmware emits KEY_POWER on a short
    // press. Older firmware does not, so Alt+P is kept as a host-side
    // fallback. Alt+Space is deliberately not used here: the keyboard MCU
    // reserves it for cycling the keyboard backlight.
    if (c == key_power ||
        (alt_held && (c == 'P' || c == 'p'))) {
        return key_hotkey_sleep;
    }

    // Alt+S is a global screenshot hotkey. Consume it before normal
    // Caps/Shift case normalization so the shortcut is layout-independent.
    // Alt+B is reserved by the PicoCalc keyboard MCU for battery display.
    if (alt_held) {
        const int hotkey = input_hotkeys::alt_letter(c);
        if (hotkey >= 0) return hotkey;
    }

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        const bool shift = shift_left_held || shift_right_held;
        const bool uppercase = caps_lock != shift; // Caps XOR Shift

        if (uppercase && c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        } else if (!uppercase && c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }

    if (ctrl_held && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;
    } else if (ctrl_held && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 1;
    }

    return c;
}

} // namespace rmb::picocalc::keyboard
