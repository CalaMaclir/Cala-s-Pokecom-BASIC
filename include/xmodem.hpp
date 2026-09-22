#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb::xmodem {
constexpr int timeout = -1, cancelled = -2, disconnected = -3;
enum class Error { None, Timeout, Cancelled, Disconnected, Crc, Protocol, Read, Write };
struct Result { Error error; std::uint32_t bytes; };
// read returns a byte or the negative codes above. No console I/O is allowed
// inside these callbacks. finish commits the receive file before the EOT ACK.
struct IO {
    void* context;
    int (*read)(void*, unsigned timeout_ms);
    bool (*write)(void*, const std::uint8_t*, std::size_t);
    int (*source)(void*, std::uint8_t*, std::size_t);
    bool (*sink)(void*, const std::uint8_t*, std::size_t);
    bool (*finish)(void*);
};
std::uint16_t crc16(const std::uint8_t* data, std::size_t size);
Result receive(IO& io);
Result send(IO& io);
const char* error_text(Error error);
}
