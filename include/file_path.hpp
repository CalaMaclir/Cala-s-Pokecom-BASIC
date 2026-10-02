#pragma once
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace rmb::file_paths {
constexpr std::size_t capacity = 80; // complete root-relative path, including NUL
inline char upper(char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }
inline bool same(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) if (upper(*a++) != upper(*b++)) return false;
    return *a == *b;
}
inline const char* basename(const char* path) {
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}
inline bool valid_relative(const char* path, bool allow_root = false) {
    if (!path) return false;
    const std::size_t n = std::strlen(path);
    if (!n) return allow_root;
    if (n >= capacity || path[0] == '/' || path[n - 1] == '/') return false;
    bool component_start = true;
    char previous = 0;
    for (const char* p = path; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '/') {
            if (component_start || previous == '.' || previous == ' ') return false;
            component_start = true;
        } else {
            if (component_start && (c == '.' || c == ' ')) return false;
            if (c == '.' && previous == '.') return false;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '.' || c == ' '))
                return false;
            component_start = false;
        }
        previous = static_cast<char>(c);
    }
    return previous != '.' && previous != ' ';
}
inline bool normalize(const char* input, char* output, std::size_t size,
                      bool allow_root = false) {
    if (!input || !output || !size) return false;
    // One leading slash means the SD root. Repeated separators and dot
    // components are rejected, not silently repaired or resolved.
    if (*input == '/') ++input;
    if (!valid_relative(input, allow_root) || std::strlen(input) >= size) return false;
    std::memmove(output, input, std::strlen(input) + 1u);
    return true;
}
inline bool join(const char* directory, const char* name, char* output, std::size_t size) {
    if (!name || !*name || !valid_relative(directory, true)) return false;
    if (*name == '/') return normalize(name, output, size);
    if (!valid_relative(name)) return false;
    char full[capacity] = {};
    const int n = std::snprintf(full, sizeof(full), "%s%s%s", directory,
                                *directory ? "/" : "", name);
    return n >= 0 && n < static_cast<int>(sizeof(full)) && normalize(full, output, size);
}
inline bool parent(const char* input, char* output, std::size_t size) {
    char full[capacity] = {};
    if (!normalize(input, full, sizeof(full), true)) return false;
    char* slash = std::strrchr(full, '/');
    if (slash) *slash = '\0'; else full[0] = '\0';
    return normalize(full, output, size, true);
}
inline bool same_or_child(const char* child, const char* directory) {
    if (!child || !directory || !*directory) return false;
    while (*directory) {
        if (!*child || upper(*child++) != upper(*directory++)) return false;
    }
    return !*child || *child == '/';
}
inline bool relocate(const char* name, const char* from, const char* to,
                     char* output, std::size_t size) {
    if (!valid_relative(name) || !valid_relative(from) || !valid_relative(to)) return false;
    if (!same_or_child(name, from)) return normalize(name, output, size);
    char full[capacity] = {};
    const int n = std::snprintf(full, sizeof(full), "%s%s", to, name + std::strlen(from));
    return n >= 0 && n < static_cast<int>(sizeof(full)) && normalize(full, output, size);
}
inline bool physical(const char* root, const char* name, char* output,
                     std::size_t size, bool allow_root = false) {
    if (!root || !*root || !valid_relative(name, allow_root)) return false;
    const auto nroot = std::strlen(root);
    const int n = std::snprintf(output, size, "%s%s%s", root,
                                root[nroot - 1] == '/' ? "" : "/", name);
    return n >= 0 && n < static_cast<int>(size);
}
} // namespace rmb::file_paths
