#include "picocalc_display.hpp"
#include "console_layout.hpp"
#include "platform.hpp"

#include <cstdint>
#include <cstring>
#include "hardware/spi.h"
#include "pico/stdlib.h"

namespace rmb::picocalc::display {

namespace {
spi_inst_t* const lcd_spi = spi1;
constexpr uint sck_pin = 10;
constexpr uint mosi_pin = 11;
constexpr uint miso_pin = 12;
constexpr uint cs_pin = 13;
constexpr uint dc_pin = 14;
constexpr uint rst_pin = 15;
constexpr uint32_t spi_hz = 25000000; // PicoCalc reference LCD SPI rate

constexpr int glyph_w = 5;
constexpr int glyph_h = 7;
constexpr int cell_w = 6;
constexpr int cell_h = 8;
constexpr int lcd_ram_height = 480; // ILI9488 internal vertical GRAM
constexpr int status_rows = console_layout::status_rows;
constexpr int status_pixels = status_rows * cell_h;
constexpr int function_key_rows = 1;
constexpr int function_key_pixels = function_key_rows * cell_h;

uint32_t fg = 0x00ff00;
uint32_t bg = 0x000000;
uint32_t gfx = 0xffffff;
int cursor_x = 0;
int cursor_y = 0;
int pen_x = 0;
int pen_y = 0;
int text_scroll_y = 0;
bool fixed_status_area = true;
bool fixed_function_key_bar = true;

int scroll_top_pixels() {
    return fixed_status_area ? status_pixels : 0;
}

int scroll_bottom_pixels() {
    return fixed_function_key_bar ? function_key_pixels : 0;
}

int scroll_area_height() {
    // Keep the controller's hardware scroll ring exactly as before: the
    // PicoCalc panel exposes only 320 visible lines although the controller
    // owns 480 GRAM rows. The bottom F-key row is therefore implemented as
    // an overlay inside this ring, not as VSCRDEF's bottom fixed area.
    return lcd_ram_height - scroll_top_pixels();
}

int console_bottom_pixels() {
    return height - scroll_bottom_pixels();
}

// Fast path for sequential PSET calls. A Mandelbrot-style scanline opens one
// LCD window per row and then streams RGB888 pixels without re-sending 2A/2B/2C
// for every pixel.
bool pixel_stream_active = false;
int pixel_stream_start_x = -1;
int pixel_stream_next_x = -1;
int pixel_stream_y = -1;
int pixel_stream_pixels = 0;
uint8_t pixel_stream_buffer[width * 3] = {};

char text_shadow[text_rows][text_columns] = {};
uint32_t text_row_fg[text_rows] = {};
uint32_t text_row_bg[text_rows] = {};
bool text_row_style_valid[text_rows] = {};
uint8_t graphics_nonblack[(width * height + 7) / 8] = {};

struct PaintSeed {
    std::int16_t x = 0;
    std::int16_t y = 0;
};
constexpr std::size_t paint_seed_capacity = 4096;
PaintSeed paint_seed_stack[paint_seed_capacity] = {};
uint8_t paint_row_buffer[width * 3] = {};

void invalidate_text_row_styles() {
    std::memset(text_row_style_valid, 0, sizeof(text_row_style_valid));
}

void clear_text_shadow() {
    for (int row = 0; row < text_rows; ++row) {
        std::memset(text_shadow[row], ' ', text_columns);
    }
    invalidate_text_row_styles();
}

void set_graphics_bit(int x, int y, bool nonblack) {
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height) return;
    const int index = y * width + x;
    const int byte_index = index >> 3;
    const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
    if (nonblack) graphics_nonblack[byte_index] |= mask;
    else graphics_nonblack[byte_index] &= static_cast<uint8_t>(~mask);
}

bool get_graphics_bit(int x, int y) {
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height) return false;
    const int index = y * width + x;
    return (graphics_nonblack[index >> 3] & (1u << (index & 7))) != 0;
}

void select(bool active) {
    gpio_put(cs_pin, active ? 0 : 1);
}

void begin_window_write_raw(int x0, int y0, int x1, int y1) {
    uint8_t b[4];

    // Keep CS asserted from address setup through the following pixel stream.
    select(true);

    gpio_put(dc_pin, 0);
    const uint8_t col_cmd = 0x2a;
    spi_write_blocking(lcd_spi, &col_cmd, 1);
    gpio_put(dc_pin, 1);
    b[0] = static_cast<uint8_t>(x0 >> 8);
    b[1] = static_cast<uint8_t>(x0);
    b[2] = static_cast<uint8_t>(x1 >> 8);
    b[3] = static_cast<uint8_t>(x1);
    spi_write_blocking(lcd_spi, b, 4);

    gpio_put(dc_pin, 0);
    const uint8_t page_cmd = 0x2b;
    spi_write_blocking(lcd_spi, &page_cmd, 1);
    gpio_put(dc_pin, 1);
    b[0] = static_cast<uint8_t>(y0 >> 8);
    b[1] = static_cast<uint8_t>(y0);
    b[2] = static_cast<uint8_t>(y1 >> 8);
    b[3] = static_cast<uint8_t>(y1);
    spi_write_blocking(lcd_spi, b, 4);

    gpio_put(dc_pin, 0);
    const uint8_t memwrite_cmd = 0x2c;
    spi_write_blocking(lcd_spi, &memwrite_cmd, 1);
    gpio_put(dc_pin, 1);
}


void begin_window_read_raw(int x0, int y0, int x1, int y1) {
    uint8_t b[4];

    select(true);

    gpio_put(dc_pin, 0);
    const uint8_t col_cmd = 0x2a;
    spi_write_blocking(lcd_spi, &col_cmd, 1);
    gpio_put(dc_pin, 1);
    b[0] = static_cast<uint8_t>(x0 >> 8);
    b[1] = static_cast<uint8_t>(x0);
    b[2] = static_cast<uint8_t>(x1 >> 8);
    b[3] = static_cast<uint8_t>(x1);
    spi_write_blocking(lcd_spi, b, 4);

    gpio_put(dc_pin, 0);
    const uint8_t page_cmd = 0x2b;
    spi_write_blocking(lcd_spi, &page_cmd, 1);
    gpio_put(dc_pin, 1);
    b[0] = static_cast<uint8_t>(y0 >> 8);
    b[1] = static_cast<uint8_t>(y0);
    b[2] = static_cast<uint8_t>(y1 >> 8);
    b[3] = static_cast<uint8_t>(y1);
    spi_write_blocking(lcd_spi, b, 4);

    gpio_put(dc_pin, 0);
    const uint8_t ramrd_cmd = 0x2e;
    spi_write_blocking(lcd_spi, &ramrd_cmd, 1);
    gpio_put(dc_pin, 1);
}

void end_pixel_stream() {
    if (!pixel_stream_active) return;

    if (pixel_stream_pixels > 0) {
        begin_window_write_raw(
            pixel_stream_start_x,
            pixel_stream_y,
            pixel_stream_start_x + pixel_stream_pixels - 1,
            pixel_stream_y
        );
        spi_write_blocking(
            lcd_spi,
            pixel_stream_buffer,
            static_cast<size_t>(pixel_stream_pixels * 3)
        );
        select(false);
    }

    pixel_stream_active = false;
    pixel_stream_start_x = -1;
    pixel_stream_next_x = -1;
    pixel_stream_y = -1;
    pixel_stream_pixels = 0;
}

void write_command(uint8_t cmd) {
    end_pixel_stream();
    gpio_put(dc_pin, 0);
    select(true);
    spi_write_blocking(lcd_spi, &cmd, 1);
    select(false);
}

void write_data(const uint8_t* data, size_t size) {
    end_pixel_stream();
    gpio_put(dc_pin, 1);
    select(true);
    spi_write_blocking(lcd_spi, data, size);
    select(false);
}

void write_data8(uint8_t value) {
    write_data(&value, 1);
}

void begin_window_write(int x0, int y0, int x1, int y1) {
    end_pixel_stream();
    begin_window_write_raw(x0, y0, x1, y1);
}

void fill_rect_ram(
    int x0,
    int y0,
    int x1,
    int y1,
    uint32_t rgb
) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= width) x1 = width - 1;
    if (y1 >= lcd_ram_height) y1 = lcd_ram_height - 1;
    if (x0 > x1 || y0 > y1) return;

    const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xff);
    const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xff);
    const uint8_t b = static_cast<uint8_t>(rgb & 0xff);

    // 128 RGB888 pixels per transfer reduces SDK call overhead for clears,
    // filled boxes and axis-aligned lines.
    uint8_t block[384];
    for (size_t i = 0; i < sizeof(block); i += 3) {
        block[i] = r;
        block[i + 1] = g;
        block[i + 2] = b;
    }

    begin_window_write(x0, y0, x1, y1);

    int pixels = (x1 - x0 + 1) * (y1 - y0 + 1);
    while (pixels > 0) {
        const int n = pixels > 128 ? 128 : pixels;
        spi_write_blocking(
            lcd_spi,
            block,
            static_cast<size_t>(n * 3)
        );
        pixels -= n;
    }

    select(false);
}

void fill_rect(int x0, int y0, int x1, int y1, uint32_t rgb) {
    // Normal graphics/text clear operations use the visible 320-pixel area.
    if (y0 < 0) y0 = 0;
    if (y1 >= height) y1 = height - 1;
    if (y0 > y1) return;
    fill_rect_ram(x0, y0, x1, y1, rgb);
}

void set_hw_scroll_start(int pixels) {
    const int area = scroll_area_height();
    if (area <= 0) return;

    pixels %= area;
    if (pixels < 0) pixels += area;

    const int address = scroll_top_pixels() + pixels;

    write_command(0x37);
    const uint8_t data[2] = {
        static_cast<uint8_t>((address >> 8) & 0xff),
        static_cast<uint8_t>(address & 0xff)
    };
    write_data(data, sizeof(data));
}

void configure_hw_scroll() {
    const int top = scroll_top_pixels();
    const int area = scroll_area_height();

    // Only the top status area uses the controller's fixed-area feature.
    // The bottom function-key row is redrawn as an overlay after scrolling;
    // using VSCRDEF's bottom fixed area places it outside PicoCalc's visible
    // 320-line viewport on this controller/panel combination.
    write_command(0x33);
    const uint8_t data[6] = {
        static_cast<uint8_t>((top >> 8) & 0xff),
        static_cast<uint8_t>(top & 0xff),
        static_cast<uint8_t>((area >> 8) & 0xff),
        static_cast<uint8_t>(area & 0xff),
        0x00, 0x00
    };
    write_data(data, sizeof(data));

    text_scroll_y = 0;
    set_hw_scroll_start(0);
}

int text_physical_y(int logical_y) {
    const int top = scroll_top_pixels();
    if (logical_y < top) return logical_y;

    const int area = scroll_area_height();
    int y = (logical_y - top) + text_scroll_y;
    y %= area;
    if (y < 0) y += area;
    return top + y;
}

void reset_hw_scroll() {
    if (text_scroll_y != 0) {
        text_scroll_y = 0;
        set_hw_scroll_start(0);
    }
}

bool glyph(char c, uint8_t out[5]) {
    const uint8_t* p = nullptr;
    switch (c) {
        case ' ': { static const uint8_t v[5]={0,0,0,0,0}; p=v; break; }
        case '!': { static const uint8_t v[5]={0,0,0x5f,0,0}; p=v; break; }
        case '"': { static const uint8_t v[5]={0,0x07,0,0x07,0}; p=v; break; }
        case '#': { static const uint8_t v[5]={0x14,0x7f,0x14,0x7f,0x14}; p=v; break; }
        case '$': { static const uint8_t v[5]={0x24,0x2a,0x7f,0x2a,0x12}; p=v; break; }
        case '%': { static const uint8_t v[5]={0x23,0x13,0x08,0x64,0x62}; p=v; break; }
        case '&': { static const uint8_t v[5]={0x36,0x49,0x55,0x22,0x50}; p=v; break; }
        case '\'':{ static const uint8_t v[5]={0,0x05,0x03,0,0}; p=v; break; }
        case '(': { static const uint8_t v[5]={0,0x1c,0x22,0x41,0}; p=v; break; }
        case ')': { static const uint8_t v[5]={0,0x41,0x22,0x1c,0}; p=v; break; }
        case '*': { static const uint8_t v[5]={0x14,0x08,0x3e,0x08,0x14}; p=v; break; }
        case '+': { static const uint8_t v[5]={0x08,0x08,0x3e,0x08,0x08}; p=v; break; }
        case ',': { static const uint8_t v[5]={0,0x50,0x30,0,0}; p=v; break; }
        case '-': { static const uint8_t v[5]={0x08,0x08,0x08,0x08,0x08}; p=v; break; }
        case '.': { static const uint8_t v[5]={0,0x60,0x60,0,0}; p=v; break; }
        case '/': { static const uint8_t v[5]={0x20,0x10,0x08,0x04,0x02}; p=v; break; }
        case '0': { static const uint8_t v[5]={0x3e,0x51,0x49,0x45,0x3e}; p=v; break; }
        case '1': { static const uint8_t v[5]={0,0x42,0x7f,0x40,0}; p=v; break; }
        case '2': { static const uint8_t v[5]={0x42,0x61,0x51,0x49,0x46}; p=v; break; }
        case '3': { static const uint8_t v[5]={0x21,0x41,0x45,0x4b,0x31}; p=v; break; }
        case '4': { static const uint8_t v[5]={0x18,0x14,0x12,0x7f,0x10}; p=v; break; }
        case '5': { static const uint8_t v[5]={0x27,0x45,0x45,0x45,0x39}; p=v; break; }
        case '6': { static const uint8_t v[5]={0x3c,0x4a,0x49,0x49,0x30}; p=v; break; }
        case '7': { static const uint8_t v[5]={0x01,0x71,0x09,0x05,0x03}; p=v; break; }
        case '8': { static const uint8_t v[5]={0x36,0x49,0x49,0x49,0x36}; p=v; break; }
        case '9': { static const uint8_t v[5]={0x06,0x49,0x49,0x29,0x1e}; p=v; break; }
        case ':': { static const uint8_t v[5]={0,0x36,0x36,0,0}; p=v; break; }
        case ';': { static const uint8_t v[5]={0,0x56,0x36,0,0}; p=v; break; }
        case '<': { static const uint8_t v[5]={0x08,0x14,0x22,0x41,0}; p=v; break; }
        case '=': { static const uint8_t v[5]={0x14,0x14,0x14,0x14,0x14}; p=v; break; }
        case '>': { static const uint8_t v[5]={0,0x41,0x22,0x14,0x08}; p=v; break; }
        case '?': { static const uint8_t v[5]={0x02,0x01,0x51,0x09,0x06}; p=v; break; }
        case '@': { static const uint8_t v[5]={0x32,0x49,0x79,0x41,0x3e}; p=v; break; }
        case 'A': { static const uint8_t v[5]={0x7e,0x11,0x11,0x11,0x7e}; p=v; break; }
        case 'B': { static const uint8_t v[5]={0x7f,0x49,0x49,0x49,0x36}; p=v; break; }
        case 'C': { static const uint8_t v[5]={0x3e,0x41,0x41,0x41,0x22}; p=v; break; }
        case 'D': { static const uint8_t v[5]={0x7f,0x41,0x41,0x22,0x1c}; p=v; break; }
        case 'E': { static const uint8_t v[5]={0x7f,0x49,0x49,0x49,0x41}; p=v; break; }
        case 'F': { static const uint8_t v[5]={0x7f,0x09,0x09,0x09,0x01}; p=v; break; }
        case 'G': { static const uint8_t v[5]={0x3e,0x41,0x49,0x49,0x7a}; p=v; break; }
        case 'H': { static const uint8_t v[5]={0x7f,0x08,0x08,0x08,0x7f}; p=v; break; }
        case 'I': { static const uint8_t v[5]={0,0x41,0x7f,0x41,0}; p=v; break; }
        case 'J': { static const uint8_t v[5]={0x20,0x40,0x41,0x3f,0x01}; p=v; break; }
        case 'K': { static const uint8_t v[5]={0x7f,0x08,0x14,0x22,0x41}; p=v; break; }
        case 'L': { static const uint8_t v[5]={0x7f,0x40,0x40,0x40,0x40}; p=v; break; }
        case 'M': { static const uint8_t v[5]={0x7f,0x02,0x0c,0x02,0x7f}; p=v; break; }
        case 'N': { static const uint8_t v[5]={0x7f,0x04,0x08,0x10,0x7f}; p=v; break; }
        case 'O': { static const uint8_t v[5]={0x3e,0x41,0x41,0x41,0x3e}; p=v; break; }
        case 'P': { static const uint8_t v[5]={0x7f,0x09,0x09,0x09,0x06}; p=v; break; }
        case 'Q': { static const uint8_t v[5]={0x3e,0x41,0x51,0x21,0x5e}; p=v; break; }
        case 'R': { static const uint8_t v[5]={0x7f,0x09,0x19,0x29,0x46}; p=v; break; }
        case 'S': { static const uint8_t v[5]={0x46,0x49,0x49,0x49,0x31}; p=v; break; }
        case 'T': { static const uint8_t v[5]={0x01,0x01,0x7f,0x01,0x01}; p=v; break; }
        case 'U': { static const uint8_t v[5]={0x3f,0x40,0x40,0x40,0x3f}; p=v; break; }
        case 'V': { static const uint8_t v[5]={0x1f,0x20,0x40,0x20,0x1f}; p=v; break; }
        case 'W': { static const uint8_t v[5]={0x3f,0x40,0x38,0x40,0x3f}; p=v; break; }
        case 'X': { static const uint8_t v[5]={0x63,0x14,0x08,0x14,0x63}; p=v; break; }
        case 'Y': { static const uint8_t v[5]={0x07,0x08,0x70,0x08,0x07}; p=v; break; }
        case 'Z': { static const uint8_t v[5]={0x61,0x51,0x49,0x45,0x43}; p=v; break; }
        case 'a': { static const uint8_t v[5]={0x20,0x54,0x54,0x54,0x78}; p=v; break; }
        case 'b': { static const uint8_t v[5]={0x7f,0x48,0x44,0x44,0x38}; p=v; break; }
        case 'c': { static const uint8_t v[5]={0x38,0x44,0x44,0x44,0x20}; p=v; break; }
        case 'd': { static const uint8_t v[5]={0x38,0x44,0x44,0x48,0x7f}; p=v; break; }
        case 'e': { static const uint8_t v[5]={0x38,0x54,0x54,0x54,0x18}; p=v; break; }
        case 'f': { static const uint8_t v[5]={0x08,0x7e,0x09,0x01,0x02}; p=v; break; }
        case 'g': { static const uint8_t v[5]={0x0c,0x52,0x52,0x52,0x3e}; p=v; break; }
        case 'h': { static const uint8_t v[5]={0x7f,0x08,0x04,0x04,0x78}; p=v; break; }
        case 'i': { static const uint8_t v[5]={0x00,0x44,0x7d,0x40,0x00}; p=v; break; }
        case 'j': { static const uint8_t v[5]={0x20,0x40,0x44,0x3d,0x00}; p=v; break; }
        case 'k': { static const uint8_t v[5]={0x7f,0x10,0x28,0x44,0x00}; p=v; break; }
        case 'l': { static const uint8_t v[5]={0x00,0x41,0x7f,0x40,0x00}; p=v; break; }
        case 'm': { static const uint8_t v[5]={0x7c,0x04,0x18,0x04,0x78}; p=v; break; }
        case 'n': { static const uint8_t v[5]={0x7c,0x08,0x04,0x04,0x78}; p=v; break; }
        case 'o': { static const uint8_t v[5]={0x38,0x44,0x44,0x44,0x38}; p=v; break; }
        case 'p': { static const uint8_t v[5]={0x7c,0x14,0x14,0x14,0x08}; p=v; break; }
        case 'q': { static const uint8_t v[5]={0x08,0x14,0x14,0x18,0x7c}; p=v; break; }
        case 'r': { static const uint8_t v[5]={0x7c,0x08,0x04,0x04,0x08}; p=v; break; }
        case 's': { static const uint8_t v[5]={0x48,0x54,0x54,0x54,0x20}; p=v; break; }
        case 't': { static const uint8_t v[5]={0x04,0x3f,0x44,0x40,0x20}; p=v; break; }
        case 'u': { static const uint8_t v[5]={0x3c,0x40,0x40,0x20,0x7c}; p=v; break; }
        case 'v': { static const uint8_t v[5]={0x1c,0x20,0x40,0x20,0x1c}; p=v; break; }
        case 'w': { static const uint8_t v[5]={0x3c,0x40,0x30,0x40,0x3c}; p=v; break; }
        case 'x': { static const uint8_t v[5]={0x44,0x28,0x10,0x28,0x44}; p=v; break; }
        case 'y': { static const uint8_t v[5]={0x0c,0x50,0x50,0x50,0x3c}; p=v; break; }
        case 'z': { static const uint8_t v[5]={0x44,0x64,0x54,0x4c,0x44}; p=v; break; }
        case '[': { static const uint8_t v[5]={0,0x7f,0x41,0x41,0}; p=v; break; }
        case '\\':{ static const uint8_t v[5]={0x02,0x04,0x08,0x10,0x20}; p=v; break; }
        case ']': { static const uint8_t v[5]={0,0x41,0x41,0x7f,0}; p=v; break; }
        case '^': { static const uint8_t v[5]={0x04,0x02,0x01,0x02,0x04}; p=v; break; }
        case '_': { static const uint8_t v[5]={0x40,0x40,0x40,0x40,0x40}; p=v; break; }
        default: return false;
    }

    std::memcpy(out, p, 5);
    return true;
}

void draw_char(int x, int y, char c) {
    const int physical_y = text_physical_y(y);

    uint8_t cols[5] = {};
    if (!glyph(c, cols)) {
        glyph('?', cols);
    }

    const uint8_t fr = static_cast<uint8_t>((fg >> 16) & 0xff);
    const uint8_t fg8 = static_cast<uint8_t>((fg >> 8) & 0xff);
    const uint8_t fb = static_cast<uint8_t>(fg & 0xff);
    const uint8_t br = static_cast<uint8_t>((bg >> 16) & 0xff);
    const uint8_t bg8 = static_cast<uint8_t>((bg >> 8) & 0xff);
    const uint8_t bb = static_cast<uint8_t>(bg & 0xff);

    uint8_t pixels[cell_w * cell_h * 3] = {};
    std::size_t pos = 0;

    for (int row = 0; row < cell_h; ++row) {
        for (int col = 0; col < cell_w; ++col) {
            const bool on =
                col < glyph_w &&
                row < glyph_h &&
                (cols[col] & (1u << row));

            pixels[pos++] = on ? fr : br;
            pixels[pos++] = on ? fg8 : bg8;
            pixels[pos++] = on ? fb : bb;
        }
    }

    begin_window_write(
        x,
        physical_y,
        x + cell_w - 1,
        physical_y + cell_h - 1
    );
    spi_write_blocking(lcd_spi, pixels, sizeof(pixels));
    select(false);
}

void redraw_text_shadow() {
    reset_hw_scroll();

    for (int row = 0; row < text_rows; ++row) {
        for (int col = 0; col < text_columns; ++col) {
            draw_char(col * cell_w, row * cell_h, text_shadow[row][col]);
        }
    }

    fill_rect(text_columns * cell_w, 0, width - 1, height - 1, bg);

    // redraw_text_shadow() uses the current console colors for every row, so
    // any menu-row style cache from before normalization is no longer valid.
    invalidate_text_row_styles();
}

void normalize_scroll_for_graphics() {
    end_pixel_stream();

    // Graphics can overwrite any text/status cell.  Even if the character
    // shadow itself is unchanged, the pixels are no longer guaranteed to
    // match the cached text-row style.
    invalidate_text_row_styles();

    if (text_scroll_y == 0) {
        return;
    }

    // Graphics use fixed 0..319 coordinates. Normalize the controller once
    // when leaving the scrolled text console so graphics keep their expected
    // coordinate system. This expensive redraw happens only on mode transition,
    // not on every text scroll.
    redraw_text_shadow();
}

void redraw_function_key_overlay() {
    if (!fixed_function_key_bar) return;

    const int row = text_rows - 1;
    const uint32_t old_fg = fg;
    const uint32_t old_bg = bg;

    fg = text_row_fg[row];
    bg = text_row_bg[row];

    for (int col = 0; col < text_columns; ++col) {
        draw_char(col * cell_w, row * cell_h, text_shadow[row][col]);
    }

    const int py = text_physical_y(row * cell_h);
    fill_rect_ram(
        text_columns * cell_w,
        py,
        width - 1,
        py + cell_h - 1,
        bg
    );

    fg = old_fg;
    bg = old_bg;
    text_row_style_valid[row] = true;
}

void scroll_shadow_rows(int rows) {
    if (rows <= 0) return;

    // Normal LIST/PRINT scrolling advances exactly one row. For that hot path
    // we can render the footer into the next (currently hidden) GRAM row
    // before moving VSCRSADD, then clear the old footer only after it has
    // become the newly exposed body row. This avoids visibly blanking and
    // redrawing the function-key bar on every line of a long listing.
    const bool smooth_footer_scroll =
        fixed_function_key_bar && rows == 1;

    if (fixed_function_key_bar && !smooth_footer_scroll) {
        const int footer_y = text_physical_y((text_rows - 1) * cell_h);
        fill_rect_ram(
            0,
            footer_y,
            width - 1,
            footer_y + cell_h - 1,
            bg
        );
    }

    // Hardware scrolling changes which physical GRAM row corresponds to each
    // logical text row. Rebuild style knowledge lazily on the next row draw.
    invalidate_text_row_styles();

    const int first_row = fixed_status_area ? status_rows : 0;
    const int last_row =
        text_rows - (fixed_function_key_bar ? function_key_rows : 0);
    const int scrolling_rows = last_row - first_row;
    const int top = scroll_top_pixels();
    const int area = scroll_area_height();

    if (rows >= scrolling_rows) {
        for (int row = first_row; row < last_row; ++row) {
            std::memset(text_shadow[row], ' ', text_columns);
        }
        reset_hw_scroll();
        fill_rect_ram(
            0,
            top,
            width - 1,
            top + area - 1,
            bg
        );
        cursor_y = top;
        redraw_function_key_overlay();
        return;
    }

    std::memmove(
        text_shadow[first_row],
        text_shadow[first_row + rows],
        static_cast<std::size_t>(scrolling_rows - rows) * text_columns
    );

    for (int row = last_row - rows; row < last_row; ++row) {
        std::memset(text_shadow[row], ' ', text_columns);
    }

    const int next_scroll_y =
        (text_scroll_y + rows * cell_h) % area;

    auto clear_exposed_rows = [&]() {
        for (int row = last_row - rows; row < last_row; ++row) {
            int relative =
                (row * cell_h - top) + next_scroll_y;
            relative %= area;
            if (relative < 0) relative += area;
            const int physical_y = top + relative;

            fill_rect_ram(
                0,
                physical_y,
                width - 1,
                physical_y + cell_h - 1,
                bg
            );
        }
    };

    if (smooth_footer_scroll) {
        // With the old scroll origin, the next footer location is one text row
        // beyond the visible 320-line viewport, so this repaint is invisible.
        const int old_scroll_y = text_scroll_y;
        text_scroll_y = next_scroll_y;
        redraw_function_key_overlay();
        text_scroll_y = old_scroll_y;
    } else {
        // Multi-row jumps retain the conservative legacy ordering.
        clear_exposed_rows();
    }

    text_scroll_y = next_scroll_y;
    set_hw_scroll_start(text_scroll_y);

    if (smooth_footer_scroll) {
        // The old footer is now the bottom body row. Blank it only after the
        // hardware scroll moved it out of the footer position.
        clear_exposed_rows();
    } else {
        redraw_function_key_overlay();
    }

    cursor_y -= rows * cell_h;
    if (cursor_y < top) cursor_y = top;
}

void hardware_reset() {
    gpio_put(rst_pin, 1);
    sleep_ms(10);
    gpio_put(rst_pin, 0);
    sleep_ms(10);
    gpio_put(rst_pin, 1);
    sleep_ms(200);
}

void controller_init() {
    hardware_reset();

    // ST7365P/ILI9488-compatible initialization used by PicoCalc.
    write_command(0xe0);
    const uint8_t gamma_p[] = {0x00,0x03,0x09,0x08,0x16,0x0a,0x3f,0x78,0x4c,0x09,0x0a,0x08,0x16,0x1a,0x0f};
    write_data(gamma_p, sizeof(gamma_p));

    write_command(0xe1);
    const uint8_t gamma_n[] = {0x00,0x16,0x19,0x03,0x0f,0x05,0x32,0x45,0x46,0x04,0x0e,0x0d,0x35,0x37,0x0f};
    write_data(gamma_n, sizeof(gamma_n));

    write_command(0xc0);
    const uint8_t pwr1[] = {0x17,0x15};
    write_data(pwr1, sizeof(pwr1));

    write_command(0xc1); write_data8(0x41);

    write_command(0xc5);
    const uint8_t vcom[] = {0x00,0x12,0x80};
    write_data(vcom, sizeof(vcom));

    write_command(0x36); write_data8(0x48);
    write_command(0x3a); write_data8(0x66);
    write_command(0xb0); write_data8(0x00);
    write_command(0xb1); write_data8(0xa0);
    write_command(0x21);
    write_command(0xb4); write_data8(0x02);

    write_command(0xb6);
    const uint8_t display_fn[] = {0x02,0x02,0x3b};
    write_data(display_fn, sizeof(display_fn));

    write_command(0xb7); write_data8(0xc6);
    write_command(0xe9); write_data8(0x00);

    write_command(0xf7);
    const uint8_t adj[] = {0xa9,0x51,0x2c,0x82};
    write_data(adj, sizeof(adj));

    write_command(0x11);
    sleep_ms(120);
    write_command(0x29);
    sleep_ms(120);
    write_command(0x36);
    write_data8(0x48);
}

} // namespace

void init() {
    gpio_init(sck_pin);
    gpio_init(mosi_pin);
    gpio_init(miso_pin);
    gpio_init(cs_pin);
    gpio_init(dc_pin);
    gpio_init(rst_pin);

    gpio_set_dir(cs_pin, GPIO_OUT);
    gpio_set_dir(dc_pin, GPIO_OUT);
    gpio_set_dir(rst_pin, GPIO_OUT);
    gpio_put(cs_pin, 1);
    gpio_put(rst_pin, 1);

    spi_init(lcd_spi, spi_hz);
    spi_set_format(lcd_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(miso_pin, GPIO_FUNC_SPI);
    gpio_set_input_hysteresis_enabled(miso_pin, true);

    controller_init();
    configure_hw_scroll();
    clear_text_shadow();
    std::memset(graphics_nonblack, 0, sizeof(graphics_nonblack));
    clear(bg);
}

void clear(uint32_t rgb) {
    end_pixel_stream();
    reset_hw_scroll();
    fill_rect(0, 0, width - 1, height - 1, rgb);
    clear_text_shadow();
    std::memset(graphics_nonblack, rgb == 0 ? 0x00 : 0xff, sizeof(graphics_nonblack));
    cursor_x = 0;
    cursor_y = 0;
}

void set_text_color(uint32_t foreground, uint32_t background) {
    fg = foreground;
    bg = background;
}

void set_status_area_enabled(bool enabled) {
    if (fixed_status_area == enabled) return;

    reset_hw_scroll();
    fixed_status_area = enabled;
    configure_hw_scroll();

    const int top = scroll_top_pixels();
    if (cursor_y < top) cursor_y = top;
}

bool status_area_enabled() {
    return fixed_status_area;
}

void set_function_key_bar_enabled(bool enabled) {
    if (fixed_function_key_bar == enabled) return;

    // Do not alter VSCRDEF here. PicoCalc's visible panel does not expose the
    // controller's nominal bottom fixed area in the way a 480-line panel
    // would. The footer is a software-managed overlay in the normal ring.
    if (!enabled) {
        const int footer_y = text_physical_y((text_rows - 1) * cell_h);
        fill_rect_ram(
            0,
            footer_y,
            width - 1,
            footer_y + cell_h - 1,
            0x000000
        );
        std::memset(
            text_shadow[text_rows - 1],
            ' ',
            text_columns
        );
        text_row_style_valid[text_rows - 1] = false;
    }

    // Preserve the last output and the cursor when restoring the footer.
    // Scroll while row 39 still belongs to the body; clamping it to row 38
    // would overwrite the preceding RUN timing with the next prompt.
    if (enabled && cursor_y >= height - function_key_pixels) {
        scroll_shadow_rows(1);
    }
    fixed_function_key_bar = enabled;

    const int bottom = console_bottom_pixels();
    if (cursor_y >= bottom) {
        cursor_y = bottom - cell_h;
        if (cursor_y < scroll_top_pixels()) {
            cursor_y = scroll_top_pixels();
        }
    }
}

bool function_key_bar_enabled() {
    return fixed_function_key_bar;
}

void draw_text_row(
    int row,
    const char* text,
    uint32_t foreground,
    uint32_t background
) {
    if (row < 0 || row >= text_rows) return;

    char target[text_columns] = {};
    bool text_ended = text == nullptr;
    for (int col = 0; col < text_columns; ++col) {
        char ch = ' ';
        if (!text_ended) {
            if (*text == '\0') {
                text_ended = true;
            } else {
                ch = *text++;
            }
        }
        target[col] = ch;
    }

    const bool same_style =
        text_row_style_valid[row] &&
        text_row_fg[row] == foreground &&
        text_row_bg[row] == background;

    // The common MENU path repeatedly submits every visible row.  If neither
    // text nor style changed, avoid all SPI traffic for this row.
    if (same_style &&
        std::memcmp(target, text_shadow[row], text_columns) == 0) {
        return;
    }

    const uint32_t old_fg = fg;
    const uint32_t old_bg = bg;
    const int old_x = cursor_x;
    const int old_y = cursor_y;

    fg = foreground;
    bg = background;

    if (!same_style) {
        // Selection highlighting changes foreground/background for the whole
        // line, so redraw the complete row only for the old/new selection.
        for (int col = 0; col < text_columns; ++col) {
            text_shadow[row][col] = target[col];
            draw_char(col * cell_w, row * cell_h, target[col]);
        }

        const int py = text_physical_y(row * cell_h);
        fill_rect_ram(
            text_columns * cell_w,
            py,
            width - 1,
            py + cell_h - 1,
            background
        );
    } else {
        // Same style: redraw only characters whose glyph actually changed
        // (for example a clock digit in the status row).
        for (int col = 0; col < text_columns; ++col) {
            if (text_shadow[row][col] == target[col]) {
                continue;
            }
            text_shadow[row][col] = target[col];
            draw_char(col * cell_w, row * cell_h, target[col]);
        }
    }

    text_row_fg[row] = foreground;
    text_row_bg[row] = background;
    text_row_style_valid[row] = true;

    fg = old_fg;
    bg = old_bg;
    cursor_x = old_x;
    cursor_y = old_y;
}

void set_cursor_visible(bool visible) {
    if (cursor_x + cell_w > width || cursor_y + cell_h > height) {
        return;
    }

    // Draw an underline cursor in the otherwise unused eighth scan line.
    // Hiding it simply restores the background, so it never damages text.
    const int physical_y =
        text_physical_y(cursor_y + glyph_h);

    fill_rect_ram(
        cursor_x,
        physical_y,
        cursor_x + glyph_w - 1,
        physical_y,
        visible ? fg : bg
    );
}

void put_char(char c) {
    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += cell_h;
        const int bottom = console_bottom_pixels();
        if (cursor_y + cell_h > bottom) {
            scroll_shadow_rows(1);
            cursor_y = bottom - cell_h;
        }
        return;
    }

    if (c == '\b' || c == 0x7f) {
        if (cursor_x >= cell_w) {
            cursor_x -= cell_w;
            const int col = cursor_x / cell_w;
            const int row = cursor_y / cell_h;
            if (row >= 0 && row < text_rows && col >= 0 && col < text_columns) {
                text_shadow[row][col] = ' ';
                text_row_style_valid[row] = false;
            }
            draw_char(cursor_x, cursor_y, ' ');
        }
        return;
    }

    if (c < 0x20 || c > 0x7e) {
        return;
    }

    if (cursor_x + cell_w > width) {
        cursor_x = 0;
        cursor_y += cell_h;
    }
    const int bottom = console_bottom_pixels();
    if (cursor_y + cell_h > bottom) {
        scroll_shadow_rows(1);
        cursor_y = bottom - cell_h;
    }

    const int col = cursor_x / cell_w;
    const int row = cursor_y / cell_h;
    if (row >= 0 && row < text_rows && col >= 0 && col < text_columns) {
        text_shadow[row][col] = c;
        text_row_style_valid[row] = false;
    }

    draw_char(cursor_x, cursor_y, c);
    cursor_x += cell_w;
}

void put_string(const char* text) {
    if (!text) return;
    while (*text) {
        put_char(*text++);
    }
}

void clear_to_eol() {
    const int row = cursor_y / cell_h;
    if (row < 0 || row >= text_rows) return;

    int start_col = cursor_x / cell_w;
    if (start_col < 0) start_col = 0;
    if (start_col >= text_columns) return;

    for (int col = start_col; col < text_columns; ++col) {
        text_shadow[row][col] = ' ';
    }
    text_row_style_valid[row] = false;

    const int physical_y = text_physical_y(row * cell_h);
    const int x0 = start_col * cell_w;
    fill_rect_ram(
        x0,
        physical_y,
        width - 1,
        physical_y + cell_h - 1,
        bg
    );
}

int cursor_column() {
    return cursor_x / cell_w;
}

int cursor_row() {
    return cursor_y / cell_h;
}

void set_cursor_position(int column, int row) {
    if (column < 0) column = 0;
    if (row < 0) row = 0;
    if (column >= text_columns) column = text_columns - 1;
    const int max_row =
        text_rows - (fixed_function_key_bar ? function_key_rows : 0) - 1;
    if (row > max_row) row = max_row;

    cursor_x = column * cell_w;
    cursor_y = row * cell_h;
}

void scroll_text_rows(int rows) {
    scroll_shadow_rows(rows);
}

void set_graphics_color(uint32_t rgb) {
    gfx = rgb & 0x00ffffffu;
}

uint32_t graphics_color() {
    return gfx;
}

void graphics_clear(uint32_t rgb) {
    end_pixel_stream();
    reset_hw_scroll();
    fill_rect(0, 0, width - 1, height - 1, rgb);
    std::memset(graphics_nonblack, rgb == 0 ? 0x00 : 0xff, sizeof(graphics_nonblack));
    clear_text_shadow();
    cursor_x = 0;
    cursor_y = 0;
    pen_x = 0;
    pen_y = 0;
}

void graphics_pixel(int x, int y) {
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height) {
        end_pixel_stream();
        return;
    }

    // Continue buffering only strictly sequential X coordinates on one row.
    if (!(pixel_stream_active &&
          y == pixel_stream_y &&
          x == pixel_stream_next_x &&
          pixel_stream_pixels < width)) {
        end_pixel_stream();
        normalize_scroll_for_graphics();

        pixel_stream_active = true;
        pixel_stream_start_x = x;
        pixel_stream_next_x = x;
        pixel_stream_y = y;
        pixel_stream_pixels = 0;
    }

    const int pos = pixel_stream_pixels * 3;
    pixel_stream_buffer[pos] =
        static_cast<uint8_t>((gfx >> 16) & 0xff);
    pixel_stream_buffer[pos + 1] =
        static_cast<uint8_t>((gfx >> 8) & 0xff);
    pixel_stream_buffer[pos + 2] =
        static_cast<uint8_t>(gfx & 0xff);

    ++pixel_stream_pixels;
    pixel_stream_next_x = x + 1;

    set_graphics_bit(x, y, gfx != 0);
    pen_x = x;
    pen_y = y;

    // Full physical rows are emitted as one 960-byte SPI transfer.
    if (pixel_stream_next_x >= width ||
        pixel_stream_pixels >= width) {
        end_pixel_stream();
    }
}

namespace {

void draw_indexed_glyph(
    int x,
    int y,
    const uint8_t pixels[64],
    const uint32_t* palette,
    bool indexed,
    uint32_t mono_color
) {
    normalize_scroll_for_graphics();

    for (int row = 0; row < 8; ++row) {
        const int py = y + row;
        if (py < 0 || py >= height) continue;

        int col = 0;
        while (col < 8) {
            while (col < 8 && pixels[row * 8 + col] == 0) ++col;
            if (col == 8) break;
            const int run_start = col;
            while (col < 8 && pixels[row * 8 + col] != 0) ++col;
            const int run_end = col - 1;

            const int visible_start = (x + run_start < 0) ? -x : run_start;
            const int visible_end =
                (x + run_end >= width) ? width - 1 - x : run_end;
            if (visible_start > visible_end) continue;

            uint8_t rgb[8 * 3] = {};
            int pos = 0;
            for (int draw_col = visible_start;
                 draw_col <= visible_end;
                 ++draw_col) {
                const uint8_t index = pixels[row * 8 + draw_col];
                const uint32_t color =
                    indexed ? palette[index] : (mono_color & 0x00ffffffu);
                rgb[pos++] = static_cast<uint8_t>((color >> 16) & 0xff);
                rgb[pos++] = static_cast<uint8_t>((color >> 8) & 0xff);
                rgb[pos++] = static_cast<uint8_t>(color & 0xff);
                set_graphics_bit(x + draw_col, py, color != 0);
            }

            begin_window_write(
                x + visible_start,
                py,
                x + visible_end,
                py
            );
            spi_write_blocking(lcd_spi, rgb, static_cast<size_t>(pos));
            select(false);
        }
    }
}

} // namespace

void graphics_draw_builtin8(int x, int y, char character, uint32_t color) {
    uint8_t columns[5] = {};
    if (!glyph(character, columns)) glyph('?', columns);

    uint8_t pixels[64] = {};
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (columns[col] & (1u << row)) {
                pixels[row * 8 + col] = 1;
            }
        }
    }
    draw_indexed_glyph(x, y, pixels, nullptr, false, color);
}

void graphics_draw_glyph8(
    int x,
    int y,
    const uint8_t pixels[64],
    const uint32_t palette[256],
    bool indexed,
    uint32_t mono_color
) {
    if (!pixels || (indexed && !palette)) return;
    draw_indexed_glyph(x, y, pixels, palette, indexed, mono_color);
}

void graphics_line(int x1, int y1, int x2, int y2) {
    normalize_scroll_for_graphics();

    // Common BASIC graphics cases: avoid one LCD window transaction per pixel.
    if (y1 == y2) {
        int ax = x1 < x2 ? x1 : x2;
        int bx = x1 < x2 ? x2 : x1;

        if (y1 >= 0 && y1 < height && bx >= 0 && ax < width) {
            if (ax < 0) ax = 0;
            if (bx >= width) bx = width - 1;

            fill_rect(ax, y1, bx, y1, gfx);
            for (int x = ax; x <= bx; ++x) {
                set_graphics_bit(x, y1, gfx != 0);
            }
        }

        pen_x = x2;
        pen_y = y2;
        return;
    }

    if (x1 == x2) {
        int ay = y1 < y2 ? y1 : y2;
        int by = y1 < y2 ? y2 : y1;

        if (x1 >= 0 && x1 < width && by >= 0 && ay < height) {
            if (ay < 0) ay = 0;
            if (by >= height) by = height - 1;

            fill_rect(x1, ay, x1, by, gfx);
            for (int y = ay; y <= by; ++y) {
                set_graphics_bit(x1, y, gfx != 0);
            }
        }

        pen_x = x2;
        pen_y = y2;
        return;
    }

    int dx = x2 >= x1 ? x2 - x1 : x1 - x2;
    int sx = x1 < x2 ? 1 : -1;
    int dy = y2 >= y1 ? y1 - y2 : y2 - y1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if ((unsigned)x1 < (unsigned)width && (unsigned)y1 < (unsigned)height) {
            fill_rect(x1, y1, x1, y1, gfx);
            set_graphics_bit(x1, y1, gfx != 0);
        }
        if (x1 == x2 && y1 == y2) break;

        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }

    pen_x = x2;
    pen_y = y2;
}

void graphics_line_to(int x, int y) {
    graphics_line(pen_x, pen_y, x, y);
}

void graphics_circle(int cx, int cy, int radius) {
    normalize_scroll_for_graphics();
    if (radius < 0) radius = -radius;

    int x = radius;
    int y = 0;
    int err = 1 - x;

    while (x >= y) {
        const int pts[8][2] = {
            {cx + x, cy + y}, {cx + y, cy + x},
            {cx - y, cy + x}, {cx - x, cy + y},
            {cx - x, cy - y}, {cx - y, cy - x},
            {cx + y, cy - x}, {cx + x, cy - y}
        };

        for (const auto& pt : pts) {
            if ((unsigned)pt[0] < (unsigned)width &&
                (unsigned)pt[1] < (unsigned)height) {
                fill_rect(pt[0], pt[1], pt[0], pt[1], gfx);
                set_graphics_bit(pt[0], pt[1], gfx != 0);
            }
        }

        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x + 1);
        }
    }

    pen_x = cx;
    pen_y = cy;
}

void graphics_box(int x1, int y1, int x2, int y2, bool filled) {
    normalize_scroll_for_graphics();
    if (x1 > x2) { const int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { const int t = y1; y1 = y2; y2 = t; }

    if (filled) {
        const int cx1 = x1 < 0 ? 0 : x1;
        const int cy1 = y1 < 0 ? 0 : y1;
        const int cx2 = x2 >= width ? width - 1 : x2;
        const int cy2 = y2 >= height ? height - 1 : y2;

        if (cx1 <= cx2 && cy1 <= cy2) {
            fill_rect(cx1, cy1, cx2, cy2, gfx);
            for (int y = cy1; y <= cy2; ++y) {
                for (int x = cx1; x <= cx2; ++x) {
                    set_graphics_bit(x, y, gfx != 0);
                }
            }
        }
    } else {
        graphics_line(x1, y1, x2, y1);
        graphics_line(x2, y1, x2, y2);
        graphics_line(x2, y2, x1, y2);
        graphics_line(x1, y2, x1, y1);
    }

    pen_x = x2;
    pen_y = y2;
}

void graphics_flush() {
    end_pixel_stream();
}

bool read_visible_row_bgr(
    int y,
    int x1,
    int x2,
    std::uint8_t* out
) {
    if (!out || y < 0 || y >= height) return false;

    if (x1 > x2) {
        const int t = x1;
        x1 = x2;
        x2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x2 >= width) x2 = width - 1;
    if (x1 > x2) return false;

    end_pixel_stream();

    // The visible viewport can be hardware-scrolled through the ILI9488's
    // 480-row GRAM. Map logical screen Y to the physical row currently shown.
    const int physical_y = text_physical_y(y);

    begin_window_read_raw(x1, physical_y, x2, physical_y);

    // ClockworkPi's reference driver uses a conservative 6 MHz for RAMRD.
    spi_set_baudrate(lcd_spi, 6000000);

    uint8_t dummy = 0;
    spi_read_blocking(lcd_spi, 0, &dummy, 1);

    const int pixels = x2 - x1 + 1;
    spi_read_blocking(
        lcd_spi,
        0,
        out,
        static_cast<size_t>(pixels * 3)
    );

    select(false);
    spi_set_baudrate(lcd_spi, spi_hz);

    // ILI9488 returns RGB. BMP stores BGR, so swap R/B in place.
    for (int i = 0; i < pixels; ++i) {
        uint8_t* p = out + i * 3;
        const uint8_t t = p[0];
        p[0] = p[2];
        p[2] = t;
    }

    return true;
}

bool graphics_paint(int x, int y) {
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height) {
        return false;
    }

    end_pixel_stream();
    normalize_scroll_for_graphics();

    if (!read_visible_row_bgr(y, 0, width - 1, paint_row_buffer)) {
        return false;
    }

    const uint8_t target_b = paint_row_buffer[x * 3];
    const uint8_t target_g = paint_row_buffer[x * 3 + 1];
    const uint8_t target_r = paint_row_buffer[x * 3 + 2];

    const uint8_t fill_r = static_cast<uint8_t>((gfx >> 16) & 0xff);
    const uint8_t fill_g = static_cast<uint8_t>((gfx >> 8) & 0xff);
    const uint8_t fill_b = static_cast<uint8_t>(gfx & 0xff);

    if (target_r == fill_r &&
        target_g == fill_g &&
        target_b == fill_b) {
        return true;
    }

    auto matches = [&](const uint8_t* row, int px) -> bool {
        const uint8_t* p = row + px * 3;
        return p[0] == target_b &&
               p[1] == target_g &&
               p[2] == target_r;
    };

    std::size_t seed_count = 0;
    std::uint32_t service_counter = 0;
    paint_seed_stack[seed_count++] = {
        static_cast<std::int16_t>(x),
        static_cast<std::int16_t>(y)
    };

    while (seed_count > 0) {
        // Flood fill can spend hundreds of milliseconds in one VM opcode.
        // Refill audio cooperatively inside the algorithm so the DMA queue
        // cannot drain while PAINT is traversing a large region.
        if ((service_counter++ & 7u) == 0u) {
            rmb::platform::audio_service();
        }

        const PaintSeed seed = paint_seed_stack[--seed_count];
        const int sy = seed.y;
        const int sx = seed.x;

        if ((unsigned)sx >= (unsigned)width ||
            (unsigned)sy >= (unsigned)height) {
            continue;
        }

        if (!read_visible_row_bgr(
                sy, 0, width - 1, paint_row_buffer
            )) {
            return false;
        }

        if (!matches(paint_row_buffer, sx)) {
            continue;
        }

        int left = sx;
        int right = sx;

        while (left > 0 &&
               matches(paint_row_buffer, left - 1)) {
            --left;
        }
        while (right + 1 < width &&
               matches(paint_row_buffer, right + 1)) {
            ++right;
        }

        // Horizontal runs map directly to the display driver's optimized line
        // fill path and update the non-black bitmap consistently.
        graphics_line(left, sy, right, sy);

        for (int dy = -1; dy <= 1; dy += 2) {
            const int ny = sy + dy;
            if ((unsigned)ny >= (unsigned)height) continue;

            if (!read_visible_row_bgr(
                    ny, 0, width - 1, paint_row_buffer
                )) {
                return false;
            }

            int px = left;
            while (px <= right) {
                while (px <= right &&
                       !matches(paint_row_buffer, px)) {
                    ++px;
                }
                if (px > right) break;

                const int run_seed = px;
                while (px <= right &&
                       matches(paint_row_buffer, px)) {
                    ++px;
                }

                if (seed_count >= paint_seed_capacity) {
                    return false;
                }

                paint_seed_stack[seed_count++] = {
                    static_cast<std::int16_t>(run_seed),
                    static_cast<std::int16_t>(ny)
                };
            }
        }
    }

    pen_x = x;
    pen_y = y;
    return true;
}

bool graphics_point_nonblack(int x, int y) {
    normalize_scroll_for_graphics();
    return get_graphics_bit(x, y);
}

} // namespace rmb::picocalc::display
