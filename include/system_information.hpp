#pragma once
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
namespace rmb {
struct SystemInformation {
    static constexpr std::size_t capacity = 88;
    struct Row { char text[128] = {}; bool heading = false; };
    Row rows[capacity] = {};
    std::size_t count = 0;
    bool overflow = false;
    void clear() { count = 0; overflow = false; }
    void section(const char* name) {
        if (count == capacity) { overflow = true; return; }
        auto& row = rows[count++]; row.heading = true;
        std::snprintf(row.text, sizeof(row.text), "%s", name);
    }
    void add(const char* label, const char* format, ...) {
        if (count == capacity) { overflow = true; return; }
        auto& row = rows[count++]; row.heading = false;
        const int offset = std::snprintf(row.text, sizeof(row.text), "%-17s : ", label);
        if (offset < 0 || static_cast<std::size_t>(offset) >= sizeof(row.text)) { overflow = true; return; }
        va_list args; va_start(args, format);
        const int n = std::vsnprintf(row.text + offset, sizeof(row.text) - offset, format, args);
        va_end(args);
        if (n < 0 || static_cast<std::size_t>(n) >= sizeof(row.text) - offset) overflow = true;
        // Plain text reports cannot inject terminal control sequences or new fields.
        for (char* c = row.text; *c; ++c)
            if (static_cast<unsigned char>(*c) < 32 || *c == 127) *c = '?';
    }
    // Read values from the same formatted snapshot used by diagnostics.
    const char* value(const char* label) const {
        char prefix[48] = {};
        const int length = std::snprintf(prefix, sizeof(prefix), "%-17s : ", label);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(prefix)) return nullptr;
        for (std::size_t i = 0; i < count; ++i)
            if (!rows[i].heading && !std::strncmp(rows[i].text, prefix, static_cast<std::size_t>(length)))
                return rows[i].text + length;
        return nullptr;
    }
    // Preserve the entire snapshot value on the LCD; long paths/errors wrap.
    std::size_t screen_line_count(std::size_t columns = 53) const {
        std::size_t lines = 0;
        for (std::size_t i=0; i<count; ++i)
            lines += std::max<std::size_t>(1, (std::strlen(rows[i].text)+columns-1)/columns);
        return lines;
    }
    void screen_line(std::size_t line, char* out, std::size_t size, std::size_t columns = 53) const {
        if (!out || !size) return;
        out[0] = 0;
        for (std::size_t i=0; i<count; ++i) {
            const auto length = std::strlen(rows[i].text);
            const auto lines = std::max<std::size_t>(1, (length+columns-1)/columns);
            if (line < lines) {
                const auto offset = line*columns;
                const auto n = std::min({columns, length-offset, size-1});
                std::memcpy(out, rows[i].text+offset, n);out[n] = 0;return;
            }
            line -= lines;
        }
    }
    using Sink = bool (*)(const char*, void*);
    bool render_serial(Sink sink, void* context) const {
        if (!sink || !sink("=== Cala's Pokecom BASIC System Information ===\r\n", context)) return false;
        for (std::size_t i=0; i<count; ++i) {
            if (rows[i].heading && !sink("\r\n", context)) return false;
            if (!sink(rows[i].text, context) || !sink("\r\n", context)) return false;
        }
        if (overflow && !sink("[Report truncated]\r\n", context)) return false;
        return sink("===============================================\r\n", context);
    }
};
}
