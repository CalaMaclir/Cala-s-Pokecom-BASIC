#pragma once

namespace rmb::unified_keyboard {

void init();
int read_key();
bool last_key_repeat();
bool caps_lock_enabled();
bool shift_held();

} // namespace rmb::unified_keyboard
