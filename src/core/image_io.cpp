#include "image_io.hpp"
#include <algorithm>
#include <climits>

namespace rmb::image_io {
namespace {
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::int64_t signed32(const std::uint8_t* p) {
    const auto value = le32(p);
    return value & 0x80000000u ? std::int64_t(value) - 0x100000000LL : value;
}
void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (i * 8));
}
}

const char* save_bmp(FILE* file, int x1, int y1, int x2, int y2,
                     ReadRow read, void* context) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(screen_width - 1, x2); y2 = std::min(screen_height - 1, y2);
    if (x1 > x2 || y1 > y2) return "BAD SCREENSHOT REGION";
    const auto width = static_cast<std::uint32_t>(x2 - x1 + 1);
    const auto height = static_cast<std::uint32_t>(y2 - y1 + 1);
    const auto bytes = width * 3u;
    const auto stride = (bytes + 3u) & ~3u;
    std::uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    put32(header + 2, 54u + stride * height);
    put32(header + 10, 54u); put32(header + 14, 40u);
    put32(header + 18, width); put32(header + 22, height);
    put16(header + 26, 1u); put16(header + 28, 24u);
    put32(header + 34, stride * height);
    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
        return "SCREENSHOT WRITE ERROR";
    std::uint8_t row[screen_width * 3 + 3] = {};
    for (int y = y2; y >= y1; --y) {
        if (!read(context, y, x1, x2, row)) return "SCREENSHOT WRITE ERROR";
        // Preserve the existing writer's zero padding and bottom-up layout.
        std::fill(row + bytes, row + stride, 0);
        if (std::fwrite(row, 1, stride, file) != stride) return "SCREENSHOT WRITE ERROR";
    }
    return nullptr;
}

const char* load_bmp(FILE* file, int x, int y, DrawRow draw, void* context) {
    // Validate all sizes before the first pixel is drawn. Use 64-bit arithmetic
    // even on ARM/newlib, where long and file offsets are only 32 bits.
    std::uint8_t header[54] = {};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header))
        return "INVALID BMP HEADER";
    if (header[0] != 'B' || header[1] != 'M') return "UNSUPPORTED IMAGE FORMAT";
    const auto dib = le32(header + 14);
    if ((dib != 40 && dib != 108 && dib != 124) || le16(header + 26) != 1 ||
        le16(header + 28) != 24 || le32(header + 30) != 0)
        return "UNSUPPORTED BMP FORMAT";
    const auto width = signed32(header + 18);
    const auto signed_height = signed32(header + 22);
    if (width <= 0 || signed_height == 0 || signed_height == INT32_MIN)
        return "INVALID BMP SIZE";
    const auto height = signed_height < 0 ? -signed_height : signed_height;
    const std::uint64_t stride = (std::uint64_t(width) * 3u + 3u) & ~std::uint64_t(3);
    const std::uint64_t offset = le32(header + 10);
    const std::uint64_t required = offset + stride * std::uint64_t(height);
    const std::uint64_t declared = le32(header + 2);
    const std::uint64_t image_bytes = le32(header + 34);
    if (offset < 14u + dib || required > LONG_MAX || declared < required ||
        (image_bytes != 0 && image_bytes != stride * std::uint64_t(height)))
        return "INVALID BMP SIZE";
    if (std::fseek(file, 0, SEEK_END) != 0) return "IMAGE READ ERROR";
    const long actual = std::ftell(file);
    if (actual < 0) return "IMAGE READ ERROR";
    if (std::uint64_t(actual) < required || std::uint64_t(actual) < declared)
        return "TRUNCATED BMP";

    const std::int64_t left = std::max<std::int64_t>(0, x);
    const std::int64_t top = std::max<std::int64_t>(0, y);
    const std::int64_t right = std::min<std::int64_t>(screen_width, std::int64_t(x) + width);
    const std::int64_t bottom = std::min<std::int64_t>(screen_height, std::int64_t(y) + height);
    // Even wholly clipped files must pass format/length validation above.
    if (left >= right || top >= bottom) return nullptr;
    const int pixels = static_cast<int>(right - left);
    std::uint8_t row[screen_width * 3];
    for (auto dy = top; dy < bottom; ++dy) {
        const auto sy = dy - y;
        const auto file_y = signed_height < 0 ? sy : height - 1 - sy;
        const auto position = offset + std::uint64_t(file_y) * stride +
                              std::uint64_t(left - x) * 3u;
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(row, 3u, pixels, file) != static_cast<std::size_t>(pixels))
            return "IMAGE READ ERROR";
        draw(context, static_cast<int>(left), static_cast<int>(dy), row, pixels);
    }
    return nullptr;
}
} // namespace rmb::image_io
