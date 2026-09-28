#pragma once

namespace rmb::unified_keyboard {

void init();
int read_key();
bool caps_lock_enabled();
bool shift_held();

} // namespace rmb::unified_keyboard
