#include "graphics_text.hpp"

#include <cstring>

namespace rmb::graphics_text {
namespace {

GlyphKind kinds[character_count] = {};
std::uint8_t glyph_pixels[character_count][64] = {};
std::uint32_t colors[256] = {};
bool palette_initialized = false;
int current_x = 0;
int current_y = 0;

int glyph_index(char character) {
    const unsigned value = static_cast<unsigned char>(character);
    if (value < first_character || value > last_character) return -1;
    return static_cast<int>(value) - first_character;
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool byte_at(const char* text, int offset, std::uint8_t& value) {
    const int high = hex_digit(text[offset]);
    const int low = hex_digit(text[offset + 1]);
    if (high < 0 || low < 0) return false;
    value = static_cast<std::uint8_t>((high << 4) | low);
    return true;
}

void ensure_palette() {
    if (!palette_initialized) reset_palette();
}

} // namespace

void set_cursor(int x, int y) {
    current_x = x;
    current_y = y;
}

int cursor_x() { return current_x; }
int cursor_y() { return current_y; }
void advance() { current_x += 8; }

DefineResult define(char character, const char* hex_pixels) {
    const int index = glyph_index(character);
    if (index < 0) return DefineResult::BadCharacter;
    if (!hex_pixels) return DefineResult::BadLength;

    const std::size_t length = std::strlen(hex_pixels);
    if (length == 0) {
        clear_definition(character);
        return DefineResult::Ok;
    }
    if (length != 16 && length != 128) {
        return DefineResult::BadLength;
    }

    std::uint8_t decoded[64] = {};
    if (length == 16) {
        for (int row = 0; row < 8; ++row) {
            std::uint8_t bits = 0;
            if (!byte_at(hex_pixels, row * 2, bits)) {
                return DefineResult::BadHex;
            }
            for (int col = 0; col < 8; ++col) {
                decoded[row * 8 + col] =
                    (bits & (0x80u >> col)) ? 1 : 0;
            }
        }
        kinds[index] = GlyphKind::Mono;
    } else {
        for (int pixel = 0; pixel < 64; ++pixel) {
            if (!byte_at(hex_pixels, pixel * 2, decoded[pixel])) {
                return DefineResult::BadHex;
            }
        }
        kinds[index] = GlyphKind::Indexed;
    }

    std::memcpy(glyph_pixels[index], decoded, sizeof(decoded));
    return DefineResult::Ok;
}

void clear_definition(char character) {
    const int index = glyph_index(character);
    if (index < 0) return;
    kinds[index] = GlyphKind::Builtin;
    std::memset(glyph_pixels[index], 0, sizeof(glyph_pixels[index]));
}

void clear_definitions() {
    for (int i = 0; i < character_count; ++i) {
        kinds[i] = GlyphKind::Builtin;
    }
    std::memset(glyph_pixels, 0, sizeof(glyph_pixels));
}

GlyphKind glyph(char character, const std::uint8_t*& pixels) {
    const int index = glyph_index(character);
    if (index < 0 || kinds[index] == GlyphKind::Builtin) {
        pixels = nullptr;
        return GlyphKind::Builtin;
    }
    pixels = glyph_pixels[index];
    return kinds[index];
}

bool set_palette_rgb(int index, int red, int green, int blue) {
    if (index < 1 || index > 255 ||
        red < 0 || red > 255 ||
        green < 0 || green > 255 ||
        blue < 0 || blue > 255) {
        return false;
    }
    ensure_palette();
    colors[index] =
        (static_cast<std::uint32_t>(red) << 16) |
        (static_cast<std::uint32_t>(green) << 8) |
        static_cast<std::uint32_t>(blue);
    return true;
}

bool set_palette_rgb24(int index, std::uint32_t rgb) {
    if (index < 1 || index > 255 || rgb > 0x00ffffffu) return false;
    ensure_palette();
    colors[index] = rgb;
    return true;
}

void reset_palette() {
    std::memset(colors, 0, sizeof(colors));
    colors[1] = 0xffffff;
    colors[2] = 0xff0000;
    colors[3] = 0x00ff00;
    colors[4] = 0x0000ff;
    colors[5] = 0xffff00;
    colors[6] = 0x00ffff;
    colors[7] = 0xff00ff;
    palette_initialized = true;
}

const std::uint32_t* palette() {
    ensure_palette();
    return colors;
}

} // namespace rmb::graphics_text
