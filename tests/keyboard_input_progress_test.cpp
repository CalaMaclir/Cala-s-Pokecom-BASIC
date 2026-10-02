// The CI generator inserts the actual platform.cpp command-input functions.
// This harness executes them with the actual keyboard + unified drivers.
#include "keyboard_i2c_fake.hpp"
#include "platform.hpp"
#include "serial_crlf_filter.hpp"
#include "unified_keyboard.hpp"
#include "bluetooth_hid_keyboard.hpp"
#include "bluetooth_hid_ble_keyboard.hpp"
#include <deque>

namespace rmb::bluetooth_hid {
int fake_key = -1;
int read_key() { const int v = fake_key; fake_key = -1; return v; }
void set_caps_lock(bool) {}
bool caps_lock_enabled() { return false; }
bool shift_held() { return false; }
}
namespace rmb::bluetooth_hid_ble {
int fake_key = -1;
int read_key() { const int v = fake_key; fake_key = -1; return v; }
void set_caps_lock(bool) {}
bool caps_lock_enabled() { return false; }
bool shift_held() { return false; }
}
namespace rmb::picocalc::display { void set_cursor_visible(bool) {} }
namespace rmb::platform {
enum class CommandInputSource { None, Local, Serial };
CommandInputSource command_input_source = CommandInputSource::None;
bool command_input_active = false;
int pending_command_key = -1;
detail::SerialCrLfFilter serial_crlf_filter;
StatusRefreshCallback status_refresh_callback = nullptr;
void* status_refresh_context = nullptr;
ScreenshotCallback screenshot_callback = nullptr;
void* screenshot_context = nullptr;
ServiceCallback background_service_callback = nullptr;
void* background_service_context = nullptr;
std::deque<int> serial_queue;
int serial_polls = 0;
bool console_uses_serial() { return true; }
bool console_uses_lcd() { return false; }
bool serial_transfer_active() { return false; }
int console_serial_read(unsigned timeout, bool = false) {
    ++serial_polls;
    if (serial_queue.empty()) { fake_us += timeout; return -1; }
    const int v = serial_queue.front(); serial_queue.pop_front(); return v;
}
void console_local_input() {}
void audio_key_click() {}
void enter_sleep_mode(bool) {}
// @PRODUCTION_FUNCTIONS@
}
void reset_input() {
    using namespace rmb::platform;
    serial_queue.clear(); serial_polls = 0;
    serial_crlf_filter = {}; end_command_input(); begin_command_input();
    background_service_callback = nullptr;
}
int main() {
    using namespace rmb::platform;
    using namespace rmb::picocalc::keyboard;

    // During the 750 ms quiet window, serial wins without any I2C transfer.
    reset_fake_hardware(); rmb::picocalc::keyboard::init(); reset_input();
    int iterations = 0;
    background_service_callback = [](void* context) {
        auto& n = *static_cast<int*>(context);
        if (++n == 40) serial_queue.push_back(13);
    };
    background_service_context = &iterations;
    assert(get_char() == 13);
    assert(iterations == 40 && serial_polls >= 40);
    assert(i2c_write_calls == 0 && !diag().ever_ready);
    end_command_input();

    // Persistent cold-boot address NACK remains passive and serial progresses.
    reset_fake_hardware(); controller_missing = true;
    write_failure_code = PICO_ERROR_GENERIC;
    rmb::picocalc::keyboard::init(); reset_input();
    iterations = 0; background_service_context = &iterations;
    background_service_callback = [](void* context) {
        auto& n = *static_cast<int*>(context);
        if (++n == 300) serial_queue.push_back(13);
    };
    assert(get_char() == 13 && iterations == 300 && serial_polls >= 300);
    assert(diag().health == Health::Waiting);
    assert(diag().startup_attempts > 0 && diag().recovery_attempts == 0);
    end_command_input();

    // The MCU may appear later without restarting the BASIC application.
    controller_missing = false;
    assert(boot_until_ready(3000));
    key_events.emplace_back(1, 13);
    reset_input(); assert(get_char() == 13); end_command_input();

    // Once READY, perpetual read failure still cannot starve late serial input.
    read_failures = 1000; reset_input(); iterations = 0;
    background_service_context = &iterations;
    background_service_callback = [](void* context) {
        auto& n = *static_cast<int*>(context);
        if (++n == 40) serial_queue.push_back(13);
    };
    const auto started = fake_us;
    assert(get_char() == 13);
    assert(iterations == 40 && serial_polls >= 40);
    assert(fake_us - started < 1000000u);
    end_command_input(); read_failures = 0;

    // Ready USB/UART is checked before faulty local hardware, including FIFO full.
    reset_input(); tx_fifo_full = true; serial_queue.push_back(13);
    const int reads = i2c_read_calls;
    assert(get_char() == 13 && i2c_read_calls == reads);
    tx_fifo_full = false; end_command_input();

    // Runtime SDA stuck and LOST/backoff still deliver serial.
    pointer_failures = 3; sda_forced_low = true;
    for (int i = 0; i < 4; ++i) assert(read_key() == -1);
    reset_input(); serial_queue.push_back(13); assert(get_char() == 13);
    end_command_input();
    sda_forced_low = false; sleep_ms(1000);
    while (diag().health != Health::Ok) { assert(read_key() == -1); sleep_ms(4); }

    // Serial owns exactly one line; the next prompt accepts local input.
    reset_input(); serial_queue.push_back('X'); serial_queue.push_back(13);
    assert(get_char() == 'X');
    key_events.emplace_back(1, 'a');
    assert(get_char() == 13);
    end_command_input(); begin_command_input();
    assert(get_char() == 'a'); end_command_input();

    // Local partial line -> runtime LOST -> serial Enter may finish the line.
    begin_command_input(); key_events.emplace_back(1, 'z');
    assert(get_char() == 'z');
    pointer_failures = 3;
    for (int i = 0; i < 3; ++i) assert(read_key() == -1);
    serial_queue.push_back(13); assert(get_char() == 13);
    end_command_input();

    // Classic and BLE input remain usable while the built-in controller is LOST.
    controller_missing = true; reset_input();
    rmb::bluetooth_hid::fake_key = 'c'; assert(get_char() == 'c');
    end_command_input(); begin_command_input();
    rmb::bluetooth_hid_ble::fake_key = 'd'; assert(get_char() == 'd');
    std::puts("production input progress with cold-boot deferral passed");
}
