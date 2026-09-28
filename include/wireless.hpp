#pragma once

#include "system_controls.hpp"

namespace rmb::wireless {

using BoardLedMode = system_controls::BoardLedMode;

bool init();
void deinit();
bool initialized();
const char* last_error();

bool set_board_led_mode(BoardLedMode mode);
BoardLedMode board_led_mode();
void service_board_led();
void suspend_board_led();
void resume_board_led();

} // namespace rmb::wireless
