#pragma once
#include <cstdint>
#include <cstdio>

namespace rmb::image_io {
// The codec owns no persistent buffers or filesystem policy. Rows are B,G,R
// bytes, at most one physical LCD row; the caller owns/open/closes the file.
constexpr int screen_width = 320;
constexpr int screen_height = 320;
using ReadRow = bool (*)(void*, int y, int x1, int x2, std::uint8_t* bgr);
using DrawRow = void (*)(void*, int x, int y, const std::uint8_t* bgr, int pixels);
const char* save_bmp(FILE* file, int x1, int y1, int x2, int y2,
                     ReadRow read, void* context = nullptr);
const char* load_bmp(FILE* file, int x, int y, DrawRow draw,
                     void* context = nullptr);
} // namespace rmb::image_io
