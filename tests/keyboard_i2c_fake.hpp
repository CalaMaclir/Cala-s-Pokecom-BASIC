#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <utility>
#include "picocalc_keyboard.hpp"
#include "hardware/i2c.h"
#include "pico/error.h"
#include "pico/stdlib.h"

std::uint64_t fake_us = 0;
std::uint8_t selected_register = 0, response[2] = {};
std::uint8_t last_write_register = 0, last_write_value = 0;
int pointer_failures = 0, read_failures = 0;
int write_failure_code = PICO_ERROR_TIMEOUT;
std::uint8_t bios_version_reply = 0x16, bios_header_reply = 0;
bool controller_missing = false, tx_fifo_full = false;
bool sda_forced_low = false, scl_forced_low = false;
int i2c_init_calls = 0, i2c_write_calls = 0, i2c_read_calls = 0;
int i2c_deinit_calls = 0, scl_low_pulses = 0, baud_calls = 0;
std::uint64_t first_write_us = std::numeric_limits<std::uint64_t>::max();
bool gpio_output_low[32] = {}, gpio_is_output[32] = {};
std::deque<std::pair<std::uint8_t, std::uint8_t>> key_events;
auto& diag() { return rmb::picocalc::keyboard::diagnostics(); }
void reset_fake_hardware() {
    fake_us = 0; selected_register = response[0] = response[1] = 0;
    last_write_register = last_write_value = 0;
    pointer_failures = read_failures = 0;
    write_failure_code = PICO_ERROR_TIMEOUT;
    bios_version_reply = 0x16; bios_header_reply = 0;
    controller_missing = tx_fifo_full = false;
    sda_forced_low = scl_forced_low = false;
    i2c_init_calls = i2c_write_calls = i2c_read_calls = 0;
    i2c_deinit_calls = scl_low_pulses = baud_calls = 0;
    first_write_us = std::numeric_limits<std::uint64_t>::max();
    for (bool& v : gpio_output_low) v = false;
    for (bool& v : gpio_is_output) v = false;
    key_events.clear();
}
i2c_inst_t fake_i2c;
i2c_inst_t* i2c1 = &fake_i2c;
std::uint32_t i2c_init(i2c_inst_t*, std::uint32_t baud) {
    assert(baud == 10000); ++i2c_init_calls; tx_fifo_full = false; return baud;
}
void i2c_deinit(i2c_inst_t*) { ++i2c_deinit_calls; }
std::uint32_t i2c_set_baudrate(i2c_inst_t*, std::uint32_t baud) {
    assert(baud == 10000); ++baud_calls; return baud;
}
int i2c_write_timeout_us(i2c_inst_t*, std::uint8_t device,
    const std::uint8_t* data, std::size_t length, bool nostop,
    std::uint32_t timeout) {
    ++i2c_write_calls; assert(!nostop);
    if (first_write_us == std::numeric_limits<std::uint64_t>::max())
        first_write_us = fake_us;
    if (device != 0x1f) return static_cast<int>(length);
    if (controller_missing || pointer_failures > 0) {
        if (pointer_failures > 0) --pointer_failures;
        fake_us += write_failure_code == PICO_ERROR_TIMEOUT ? timeout : 10;
        return write_failure_code;
    }
    if (length == 1) {
        selected_register = data[0];
        response[0] = selected_register; response[1] = 0x50;
        if (selected_register == 0x01) {
            response[0] = bios_header_reply; response[1] = bios_version_reply;
        }
        if (selected_register == 0x09) {
            response[0] = response[1] = 0;
            // Official MCU dequeues during the pointer WRITE.
            if (!key_events.empty()) {
                response[0] = key_events.front().first;
                response[1] = key_events.front().second;
                key_events.pop_front();
            }
        }
    } else if (length == 2) {
        last_write_register = data[0];
        last_write_value = data[1];
    }
    return static_cast<int>(length);
}
int i2c_read_timeout_us(i2c_inst_t*, std::uint8_t device,
    std::uint8_t* data, std::size_t length, bool nostop,
    std::uint32_t timeout) {
    ++i2c_read_calls; assert(!nostop);
    if (controller_missing || read_failures > 0) {
        if (read_failures > 0) --read_failures;
        fake_us += timeout; return PICO_ERROR_TIMEOUT;
    }
    if (device == 0x1f && length == 2) {
        data[0] = response[0]; data[1] = response[1];
    }
    return static_cast<int>(length);
}
std::size_t i2c_get_write_available(i2c_inst_t*) {
    return tx_fifo_full ? 0 : 16;
}
int i2c_read_blocking_until(i2c_inst_t* bus, std::uint8_t device,
    std::uint8_t* data, std::size_t length, bool nostop,
    absolute_time_t deadline) {
    assert(!tx_fifo_full && !time_reached(deadline));
    return i2c_read_timeout_us(bus, device, data, length, nostop,
        static_cast<std::uint32_t>(deadline - fake_us));
}
absolute_time_t get_absolute_time() { return fake_us; }
std::uint32_t to_ms_since_boot(absolute_time_t v) {
    return static_cast<std::uint32_t>(v / 1000u);
}
absolute_time_t make_timeout_time_us(std::uint64_t v) { return fake_us + v; }
bool time_reached(absolute_time_t v) { return fake_us >= v; }
void tight_loop_contents() { ++fake_us; }
void sleep_ms(std::uint32_t v) { fake_us += v * 1000u; }
void sleep_us(std::uint64_t v) { fake_us += v; }
void gpio_set_function(uint, int) {}
void gpio_pull_up(uint) {}
void gpio_put(uint pin, bool v) { gpio_output_low[pin] = !v; }
void gpio_set_dir(uint pin, bool output) {
    if (pin == 7 && output && gpio_output_low[pin]) ++scl_low_pulses;
    gpio_is_output[pin] = output;
}
bool gpio_get(uint pin) {
    if (pin == 6 && sda_forced_low) return false;
    if (pin == 7 && scl_forced_low) return false;
    return !(gpio_is_output[pin] && gpio_output_low[pin]);
}
void service_keyboard_ms(std::uint32_t duration_ms) {
    const std::uint64_t end = fake_us + static_cast<std::uint64_t>(duration_ms) * 1000u;
    while (fake_us < end) {
        const auto before = fake_us;
        (void)rmb::picocalc::keyboard::read_key();
        assert(fake_us - before <= 12000u);
        sleep_ms(4);
    }
}
bool boot_until_ready(std::uint32_t timeout_ms = 5000) {
    const std::uint64_t end = fake_us + static_cast<std::uint64_t>(timeout_ms) * 1000u;
    while (fake_us < end) {
        (void)rmb::picocalc::keyboard::read_key();
        if (diag().ever_ready &&
            diag().health == rmb::picocalc::keyboard::Health::Ok) return true;
        sleep_ms(4);
    }
    return false;
}
int poll_for_key(int attempts = 400) {
    for (int i = 0; i < attempts; ++i) {
        const auto before = fake_us;
        const int key = rmb::picocalc::keyboard::read_key();
        assert(fake_us - before <= 12000u);
        if (key >= 0) return key;
        sleep_ms(4);
    }
    return -1;
}
