#include "wireless.hpp"

namespace rmb::wireless {
namespace {
bool ready = false;
BoardLedMode mode = BoardLedMode::Off;
}

bool init() { ready = true; return true; }
void deinit() { ready = false; }
bool initialized() { return ready; }
const char* last_error() { return ready ? "READY" : "NOT INITIALIZED"; }
bool set_board_led_mode(BoardLedMode value) {
    if (value != BoardLedMode::Off) ready = true;
    mode = value;
    return true;
}
BoardLedMode board_led_mode() { return mode; }
void service_board_led() {}
void suspend_board_led() {}
void resume_board_led() {}
} // namespace rmb::wireless
