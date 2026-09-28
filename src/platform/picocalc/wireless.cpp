#include "wireless.hpp"

#include <cstdio>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

namespace rmb::wireless {
namespace {
bool ready = false;
bool led_suspended = false;
bool led_level = false;
BoardLedMode led_mode = BoardLedMode::Off;
char error[64] = "NOT INITIALIZED";

void write_led(bool level) {
    if (!ready || led_level == level) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, level ? 1 : 0);
    led_level = level;
}
} // namespace

bool init() {
    if (ready) return true;
    if (cyw43_arch_init() != 0) {
        std::snprintf(error, sizeof(error), "CYW43 INIT FAILED");
        return false;
    }
    ready = true;
    led_level = false;
    std::snprintf(error, sizeof(error), "READY");
    return true;
}

void deinit() {
    if (!ready) return;
    write_led(false);
    cyw43_arch_deinit();
    ready = false;
    led_suspended = false;
    led_level = false;
    std::snprintf(error, sizeof(error), "NOT INITIALIZED");
}

bool initialized() { return ready; }
const char* last_error() { return error; }

bool set_board_led_mode(BoardLedMode mode) {
    if (mode != BoardLedMode::Off && !init()) return false;
    led_mode = mode;
    led_suspended = false;
    service_board_led();
    return true;
}

BoardLedMode board_led_mode() { return led_mode; }

void service_board_led() {
    if (led_suspended || led_mode == BoardLedMode::Off) {
        write_led(false);
        return;
    }
    if (!ready && !init()) return;
    write_led(system_controls::board_led_output(
        led_mode, to_ms_since_boot(get_absolute_time())));
}

void suspend_board_led() {
    led_suspended = true;
    write_led(false);
}

void resume_board_led() {
    led_suspended = false;
    service_board_led();
}

} // namespace rmb::wireless
