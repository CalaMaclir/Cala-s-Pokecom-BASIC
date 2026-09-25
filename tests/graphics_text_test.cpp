#include "graphics_text.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

int main() {
    using namespace rmb::graphics_text;

    set_cursor(11, 19);
    assert(cursor_x() == 11 && cursor_y() == 19);
    advance();
    assert(cursor_x() == 19 && cursor_y() == 19);

    const std::uint8_t* pixels = nullptr;
    assert(glyph('A', pixels) == GlyphKind::Builtin);
    assert(pixels == nullptr);

    assert(define('A', "183C66667E666600") == DefineResult::Ok);
    assert(glyph('A', pixels) == GlyphKind::Mono);
    assert(pixels[3] == 1);
    assert(pixels[4] == 1);
    assert(pixels[0] == 0);
    assert(pixels[63] == 0); // transparent zero

    assert(define('A', "") == DefineResult::Ok);
    assert(glyph('A', pixels) == GlyphKind::Builtin);

    std::string indexed(128, '0');
    indexed[0] = '0'; indexed[1] = '2';
    indexed[126] = 'F'; indexed[127] = 'F';
    assert(define('B', indexed.c_str()) == DefineResult::Ok);
    assert(glyph('B', pixels) == GlyphKind::Indexed);
    assert(pixels[0] == 2 && pixels[1] == 0 && pixels[63] == 255);

    assert(define('C', "1234") == DefineResult::BadLength);
    assert(define('C', "183C66667E66660Z") == DefineResult::BadHex);
    assert(define(0x1f, "") == DefineResult::BadCharacter);

    clear_definitions();
    assert(glyph('B', pixels) == GlyphKind::Builtin);

    reset_palette();
    const std::uint32_t* colors = palette();
    assert(colors[0] == 0x000000); // reserved transparent
    assert(colors[1] == 0xffffff);
    assert(colors[2] == 0xff0000);
    assert(colors[3] == 0x00ff00);
    assert(colors[4] == 0x0000ff);
    assert(colors[5] == 0xffff00);
    assert(colors[6] == 0x00ffff);
    assert(colors[7] == 0xff00ff);

    assert(set_palette_rgb(8, 1, 2, 3));
    assert(palette()[8] == 0x010203);
    assert(set_palette_rgb24(9, 0xff8000));
    assert(palette()[9] == 0xff8000);
    assert(!set_palette_rgb(0, 1, 2, 3));
    assert(!set_palette_rgb(1, -1, 2, 3));
    assert(!set_palette_rgb24(256, 0));
    reset_palette();
    assert(palette()[8] == 0 && palette()[9] == 0);
}
