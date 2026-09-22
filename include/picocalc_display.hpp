#pragma once

#include <cstdint>

namespace rmb::picocalc::display {

constexpr int width = 320;
constexpr int height = 320;
constexpr int text_columns = 53;
constexpr int text_rows = 40;

void init();
void clear(uint32_t rgb = 0x000000);
void put_char(char c);
void put_string(const char* text);
void clear_to_eol();
void set_text_color(uint32_t foreground, uint32_t background);
void set_cursor_visible(bool visible);
void set_status_area_enabled(bool enabled);
bool status_area_enabled();
void set_function_key_bar_enabled(bool enabled);
bool function_key_bar_enabled();
void draw_text_row(
    int row,
    const char* text,
    uint32_t foreground,
    uint32_t background
);

int cursor_column();
int cursor_row();
void set_cursor_position(int column, int row);
void scroll_text_rows(int rows);

void set_graphics_color(uint32_t rgb);
uint32_t graphics_color();
void graphics_clear(uint32_t rgb = 0x000000);
void graphics_pixel(int x, int y);
void graphics_line(int x1, int y1, int x2, int y2);
void graphics_line_to(int x, int y);
void graphics_circle(int cx, int cy, int radius);
void graphics_box(int x1, int y1, int x2, int y2, bool filled);
bool graphics_paint(int x, int y);
void graphics_flush();
bool graphics_point_nonblack(int x, int y);

// Read one row of the currently visible LCD viewport as B,G,R bytes.
// x1/x2 are inclusive physical screen coordinates.
bool read_visible_row_bgr(int y, int x1, int x2, std::uint8_t* out);

} // namespace rmb::picocalc::display
