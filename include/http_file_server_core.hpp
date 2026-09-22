#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::http {

enum class Method { Get, Put, Delete };
enum class Resource { Index, Files, File };

struct Request {
    Method method = Method::Get;
    Resource resource = Resource::Index;
    char filename[80] = {};
    std::uint32_t content_length = 0;
};

// Parses one complete, NUL-terminated header block. The caller enforces the
// 2 KiB storage limit. On failure, status is an HTTP status code.
bool parse_request(
    const char* headers,
    const char* session_token,
    Request& output,
    int& status
);

std::size_t json_escape(
    const char* input,
    char* output,
    std::size_t capacity
);

} // namespace rmb::http
