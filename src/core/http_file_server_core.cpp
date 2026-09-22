#include "http_file_server_core.hpp"
#include "safe_file.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace rmb::http {
namespace {
constexpr std::uint32_t kMaxUpload = 32u * 1024u * 1024u;

bool iequal_n(const char* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool decode_value(const char* begin, std::size_t length, char* out, std::size_t capacity) {
    std::size_t used = 0;
    for (std::size_t i = 0; i < length; ++i) {
        unsigned char value = static_cast<unsigned char>(begin[i]);
        if (value == '%') {
            if (i + 2 >= length) return false;
            const int hi = hex_value(begin[i + 1]);
            const int lo = hex_value(begin[i + 2]);
            if (hi < 0 || lo < 0) return false;
            value = static_cast<unsigned char>((hi << 4) | lo);
            i += 2;
        }
        if (used + 1 >= capacity || value == 0) return false;
        out[used++] = static_cast<char>(value);
    }
    out[used] = '\0';
    return true;
}

bool query_value(const char* query, const char* key, char* out, std::size_t capacity) {
    if (!query) return false;
    const std::size_t key_length = std::strlen(key);
    const char* item = query;
    while (*item) {
        const char* end = std::strchr(item, '&');
        if (!end) end = item + std::strlen(item);
        const char* equal = static_cast<const char*>(std::memchr(item, '=', end - item));
        if (equal && std::size_t(equal - item) == key_length &&
            std::memcmp(item, key, key_length) == 0)
            return decode_value(equal + 1, end - equal - 1, out, capacity);
        item = *end ? end + 1 : end;
    }
    return false;
}

bool parse_content_length(const char* headers, std::uint32_t& value, bool& found, bool& too_large) {
    found = false;
    too_large = false;
    value = 0;
    const char* line = std::strstr(headers, "\r\n") + 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char* end = std::strstr(line, "\r\n");
        if (!end) return false;
        const char* colon = static_cast<const char*>(std::memchr(line, ':', end - line));
        if (!colon) return false;
        const std::size_t name_length = colon - line;
        const char* text = colon + 1;
        while (text < end && (*text == ' ' || *text == '\t')) ++text;
        if (name_length == 14 && iequal_n(line, "Content-Length", 14)) {
            if (found || text == end) return false;
            std::uint64_t parsed = 0;
            while (text < end && std::isdigit(static_cast<unsigned char>(*text))) {
                parsed = parsed * 10u + static_cast<unsigned>(*text++ - '0');
                if (parsed > kMaxUpload) { too_large = true; return false; }
            }
            while (text < end && (*text == ' ' || *text == '\t')) ++text;
            if (text != end) return false;
            value = static_cast<std::uint32_t>(parsed);
            found = true;
        } else if (name_length == 17 && iequal_n(line, "Transfer-Encoding", 17)) {
            return false;
        }
        line = end + 2;
    }
    return true;
}
}

bool parse_request(const char* headers, const char* token, Request& out, int& status) {
    status = 400;
    if (!headers || !token || !*token) return false;
    const char* first_end = std::strstr(headers, "\r\n");
    if (!first_end || first_end - headers > 255) return false;

    char method[8] = {}, target[256] = {}, version[16] = {}, tail = 0;
    char first[256] = {};
    std::memcpy(first, headers, first_end - headers);
    if (std::sscanf(first, "%7s %255s %15s %c", method, target, version, &tail) != 3)
        return false;
    if (std::strcmp(version, "HTTP/1.0") && std::strcmp(version, "HTTP/1.1")) return false;
    if (!std::strcmp(method, "GET")) out.method = Method::Get;
    else if (!std::strcmp(method, "PUT")) out.method = Method::Put;
    else if (!std::strcmp(method, "DELETE")) out.method = Method::Delete;
    else { status = 405; return false; }

    char* query = std::strchr(target, '?');
    if (query) *query++ = '\0';
    char supplied[16] = {};
    if (!query_value(query, "k", supplied, sizeof(supplied)) || std::strcmp(supplied, token)) {
        status = 403;
        return false;
    }

    if (!std::strcmp(target, "/")) out.resource = Resource::Index;
    else if (!std::strcmp(target, "/api/files")) out.resource = Resource::Files;
    else if (!std::strcmp(target, "/api/file")) out.resource = Resource::File;
    else { status = 404; return false; }

    if ((out.resource == Resource::Index || out.resource == Resource::Files) &&
        out.method != Method::Get) { status = 405; return false; }
    if (out.resource == Resource::File) {
        if (!query_value(query, "name", out.filename, sizeof(out.filename)) ||
            !SafeFileWriter::valid_root_name(out.filename)) return false;
    }

    bool have_length = false, too_large = false;
    if (!parse_content_length(headers, out.content_length, have_length, too_large)) {
        status = too_large ? 413 : 400;
        return false;
    }
    if (out.method == Method::Put && !have_length) {
        status = 411;
        return false;
    }
    return true;
}

std::size_t json_escape(const char* input, char* output, std::size_t capacity) {
    if (!input || !output || !capacity) return 0;
    std::size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
        char escaped[7] = {};
        const char* text = escaped;
        std::size_t count = 0;
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = *p; count = 2; }
        else if (*p < 0x20) { std::snprintf(escaped, sizeof(escaped), "\\u%04X", *p); count = 6; }
        else { escaped[0] = static_cast<char>(*p); count = 1; }
        if (used + count + 1 > capacity) return 0;
        std::memcpy(output + used, text, count);
        used += count;
    }
    output[used] = '\0';
    return used;
}

} // namespace rmb::http
