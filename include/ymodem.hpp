#pragma once
#include "xmodem.hpp"
#include <cstddef>
#include <cstdint>

namespace rmb::ymodem {
using Error = xmodem::Error;
constexpr std::size_t kHeaderSize = 128;
constexpr std::size_t kDataSize = 1024;
constexpr std::size_t kFilenameSize = 80;

struct Header {
    char filename[kFilenameSize] = {};
    std::uint32_t size = 0;
    bool empty = false;
};
struct Result {
    Error error = Error::None;
    std::uint32_t bytes = 0;
    std::uint32_t files = 0;
    char filename[kFilenameSize] = {};
};
struct IO {
    void* context;
    int (*read)(void*, unsigned timeout_ms);
    bool (*write)(void*, const std::uint8_t*, std::size_t);
    int (*source)(void*, std::uint8_t*, std::size_t);
    bool (*sink)(void*, const std::uint8_t*, std::size_t);
    bool (*finish)(void*);
    bool (*begin_receive)(void*, const char*, std::uint32_t);
};

bool encode_header(const char* filename, std::uint32_t size,
                   std::uint8_t payload[kHeaderSize]);
bool decode_header(const std::uint8_t payload[kHeaderSize], Header& header);
Result receive(IO& io);
Result send(IO& io, const char* filename, std::uint32_t size);
const char* error_text(Error error);
}
