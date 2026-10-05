// Real storage + codec, with SD driver and LCD replaced by bounded host models.
#define main storage_ownership_regression
#include "storage_ownership_test.cpp"
#undef main
#include "image_io.hpp"
#include "soak_support.hpp"
#include <filesystem>
#include <vector>
#include <climits>
#include <cstdlib>
#include <algorithm>

namespace {
std::string root;
std::string opened_path;
std::array<std::uint32_t, 320 * 320> framebuffer{};
std::uint32_t color = 0x123456;
unsigned draws = 0;
std::vector<std::uint8_t> read_bytes(const char* name) {
    FILE* f = std::fopen(name, "rb"); assert(f);
    assert(std::fseek(f, 0, SEEK_END) == 0);
    const long size = std::ftell(f); assert(size >= 0);
    std::rewind(f); std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    assert(std::fread(result.data(), 1, result.size(), f) == result.size());
    assert(std::fclose(f) == 0); return result;
}
void write_bytes(const char* name, const std::vector<std::uint8_t>& bytes) {
    FILE* f = std::fopen(name, "wb"); assert(f);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    assert(std::fclose(f) == 0);
}
void put32(std::vector<std::uint8_t>& b, int at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at+i] = static_cast<std::uint8_t>(v >> (i*8));
}
void reject(const std::vector<std::uint8_t>& bytes, const char* error) {
    write_bytes("/BAD.BMP", bytes); const auto before = framebuffer; const auto n = draws;
    assert(!rmb::storage::load_image("BAD", -100, 0));
    assert(std::strstr(rmb::storage::last_error(), error));
    assert(before == framebuffer && n == draws && color == 0x123456);
}
}
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    opened_path = path;
    if (opened_path == "/FULL.BMP") return __real_fopen("/dev/full", mode);
    return __real_fopen((root + path).c_str(), mode);
}
namespace rmb::platform {
void put_string(const char*) {}
void set_graphics_color(std::uint32_t rgb) { color = rgb; }
std::uint32_t graphics_color() { return color; }
void graphics_pixel(int x, int y) {
    assert(x >= 0 && x < 320 && y >= 0 && y < 320);
    framebuffer[y * 320 + x] = color; ++draws;
}
void graphics_flush() {}
}
namespace rmb::picocalc::display {
bool read_visible_row_bgr(int y, int x1, int x2, std::uint8_t* out) {
    for (int x = x1; x <= x2; ++x) {
        const auto rgb = framebuffer[y * 320 + x];
        *out++ = rgb & 255; *out++ = (rgb >> 8) & 255; *out++ = (rgb >> 16) & 255;
    }
    return true;
}
}
int main() {
    char temp[] = "/tmp/cpb-image-XXXXXX"; assert(mkdtemp(temp)); root = temp;
    storage_ownership_regression(); // Driver/lease/MSC/card-removal regression.
    using namespace rmb::storage;
    for (int y = 0; y < 320; ++y) for (int x = 0; x < 320; ++x)
        framebuffer[y*320+x] = (std::uint32_t(x & 255) << 16) |
            (std::uint32_t(y & 255) << 8) | ((x+y) & 255);
    const auto original = framebuffer;
    assert(save_screenshot("SCREEN")); assert(opened_path == "/SCREEN.BMP");
    const auto full = read_bytes("/SCREEN.BMP");
    assert(full.size() == 54 + 320 * 320 * 3 && full[28] == 24 && full[30] == 0);
    framebuffer.fill(0); assert(load_image("/SCREEN.BMP", 0, 0));
    assert(framebuffer == original && color == 0x123456);
    // Asymmetric 3x2 region exercises BGR, bottom-up order and 3 padding bytes.
    assert(save_screenshot("TINY.bmp", 10, 21, 12, 20));
    auto tiny = read_bytes("/TINY.bmp"); assert(tiny.size() == 78);
    assert(tiny[63] == 0 && tiny[64] == 0 && tiny[65] == 0);
    framebuffer.fill(0); assert(load_image("TINY.bmp", 2, 4));
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x)
        assert(framebuffer[(y+4)*320+x+2] == original[(y+20)*320+x+10]);
    // Top-down equivalent and extended V4/V5 headers with a non-default offset.
    std::swap_ranges(tiny.begin()+54, tiny.begin()+66, tiny.begin()+66);
    put32(tiny, 22, static_cast<std::uint32_t>(-2));
    for (int dib : {40, 108, 124}) {
        auto b = tiny;
        if (dib != 40) b.insert(b.begin()+54, dib-40, 0);
        put32(b,14,dib); put32(b,10,14+dib); put32(b,2,b.size());
        write_bytes("/TOP.BMP", b); framebuffer.fill(0); assert(load_image("TOP",0,0));
        assert(framebuffer[0] == original[20*320+10]);
        assert(framebuffer[320+2] == original[21*320+12]);
    }
    framebuffer.fill(0); assert(load_image("TINY.bmp", -1, -1));
    assert(framebuffer[0] == original[21*320+11]);
    framebuffer.fill(0); assert(load_image("TINY.bmp",319,319));
    assert(framebuffer[319*320+319] == original[20*320+10]);
    for (int v : {-1000, 320, INT_MIN, INT_MAX}) {
        const auto before=framebuffer; assert(load_image("SCREEN",v,v)); assert(before==framebuffer);
    }
    std::filesystem::create_directories(root+"/images with spaces");
    assert(save_screenshot("/images with spaces/NEW",0,0,2,1));
    assert(opened_path == "/images with spaces/NEW.BMP");
    assert(load_image("images with spaces/NEW",0,0));
    for (const char* bad : {"", "/", "../BAD", "a/../BAD", "//BAD", "a//BAD", "a\\BAD"}) {
        assert(!load_image(bad,0,0)); assert(!save_screenshot(bad));
    }
    std::string long_name(80,'A'); assert(!load_image(long_name.c_str(),0,0));
    assert(!load_image("NONE",0,0)); assert(!std::strcmp(last_error(),"IMAGE FILE NOT FOUND"));
    assert(try_lock()); assert(!load_image("SCREEN",0,0));
    assert(!std::strcmp(last_error(),"STORAGE BUSY")); unlock();
    assert(begin_usb_host_ownership()); assert(!load_image("SCREEN",0,0));
    assert(!save_screenshot("OTHER")); assert(request_usb_safe_return(true)); poll();
    assert(firmware_owns_card());
    // Malformed/unsupported/truncated data must not paint, even when clipped.
    reject({1,2,3},"INVALID BMP HEADER");
    auto b=tiny; b[0]='P'; reject(b,"UNSUPPORTED IMAGE FORMAT");
    b=tiny; b[28]=16; reject(b,"UNSUPPORTED BMP FORMAT");
    b=tiny; b[30]=1; reject(b,"UNSUPPORTED BMP FORMAT");
    b=tiny; b[26]=2; reject(b,"UNSUPPORTED BMP FORMAT");
    b=tiny; put32(b,14,12); reject(b,"UNSUPPORTED BMP FORMAT");
    for (int at : {18,22}) { b=tiny; put32(b,at,0); reject(b,"INVALID BMP SIZE"); }
    b=tiny; put32(b,22,0x80000000u); reject(b,"INVALID BMP SIZE");
    b=tiny; put32(b,18,0xffffffffu); reject(b,"INVALID BMP SIZE");
    b=tiny; put32(b,18,0x7fffffffu); reject(b,"INVALID BMP SIZE");
    b=tiny; put32(b,10,53); reject(b,"INVALID BMP SIZE");
    b=tiny; put32(b,2,54); reject(b,"INVALID BMP SIZE");
    b=tiny; put32(b,34,1); reject(b,"INVALID BMP SIZE");
    b=tiny; b.pop_back(); reject(b,"TRUNCATED BMP");
    assert(!save_screenshot("OUT",400,400,500,500));
    assert(!save_screenshot("FULL")); assert(!std::strcmp(last_error(),"SCREENSHOT WRITE ERROR"));
    // Wider-than-screen BMP validates the complete payload but reads <=960 bytes/row.
    b.assign(54+1600*2,0); std::copy(tiny.begin(),tiny.begin()+54,b.begin());
    put32(b,18,533); put32(b,22,2); put32(b,34,3200); put32(b,2,b.size());
    write_bytes("/WIDE.BMP",b); assert(load_image("WIDE",-100,0));
    std::uint32_t random=1;
    for(int i=0;i<1000;++i) {
        b=tiny; random=random*1664525u+1013904223u;
        b[random%54]^=static_cast<std::uint8_t>(random>>24);
        write_bytes("/FUZZ.BMP",b); (void)load_image("FUZZ",INT_MAX,INT_MIN);
    }
    // Real fopen/fread/fwrite/fclose and production BMP codec, one process.
    const auto baseline = cpb_soak::resources();
    cpb_soak::record(baseline.heap < 0 ? "image" : "image-native", 0, baseline);
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) {
        framebuffer = original;
        framebuffer[i % framebuffer.size()] = i * 7919u & 0xffffffu;
        const auto expected = framebuffer;
        assert(save_screenshot("SOAK"));
        framebuffer.fill(0); assert(load_image("SOAK", 0, 0));
        assert(framebuffer == expected && color == 0x123456);
        assert(!load_image("MISSING", 0, 0));
        assert(!save_screenshot("FULL")); // flush/write failure followed by recovery.
        assert(save_screenshot("SOAK_RECOVER", 10, 20, 12, 21));
        assert(firmware_owns_card() && !busy());
        assert(baseline.heap < 0 || cpb_soak::resources().heap <= baseline.heap + 65536);
        if ((i + 1) % 50 == 0 || i + 1 == cpb_soak::iterations())
            cpb_soak::record(baseline.heap < 0 ? "image" : "image-native", i + 1, baseline);
    }
    std::filesystem::remove_all(root);
    std::puts("Image storage/codec round-trip, clipping, paths, malformed BMP: PASS");
}
