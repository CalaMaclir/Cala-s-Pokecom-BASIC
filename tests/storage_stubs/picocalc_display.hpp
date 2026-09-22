#pragma once
#include <cstdint>
namespace rmb::picocalc::display {
bool read_visible_row_bgr(int, int, int, std::uint8_t*);
}
