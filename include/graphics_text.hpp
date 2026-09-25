#pragma once

#include <cstdint>

namespace rmb::graphics_text {

constexpr int first_character = 0x20;
constexpr int last_character = 0x7e;
constexpr int character_count = last_character - first_character + 1;

enum class GlyphKind : std::uint8_t {
    Builtin,
    Mono,
    Indexed
};

enum class DefineResult : std::uint8_t {
    Ok,
    BadCharacter,
    BadLength,
    BadHex
};

void set_cursor(int x, int y);
int cursor_x();
int cursor_y();
void advance();

DefineResult define(char character, const char* hex_pixels);
void clear_definition(char character);
void clear_definitions();
GlyphKind glyph(char character, const std::uint8_t*& pixels);

bool set_palette_rgb(int index, int red, int green, int blue);
bool set_palette_rgb24(int index, std::uint32_t rgb);
void reset_palette();
const std::uint32_t* palette();

} // namespace rmb::graphics_text
