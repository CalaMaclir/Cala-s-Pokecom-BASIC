#pragma once
namespace rmb::platform {
void put_string(const char*);
bool break_requested();
}


#include <cstdint>
namespace rmb::platform {
void set_graphics_color(std::uint32_t);
std::uint32_t graphics_color();
void graphics_pixel(int, int);
void graphics_flush();
}