#include "safe_file.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <sys/stat.h>

namespace rmb {
namespace {
bool absent(const char* path) {
    struct stat value;
    return stat(path, &value) != 0 && errno == ENOENT;
}
bool equal_name(const char* a, const char* b) {
    while (*a && *b) {
        if (std::toupper(static_cast<unsigned char>(*a++)) !=
            std::toupper(static_cast<unsigned char>(*b++))) return false;
    }
    return *a == *b;
}
}

SafeFileWriter::~SafeFileWriter() { abort(); }

bool SafeFileWriter::valid_root_name(const char* name) {
    if (!name || !*name || std::strlen(name) > 79 || name[0] == '.' ||
        name[std::strlen(name) - 1] == '.') return false;
    if (std::strstr(name, "..")) return false;
    for (const char* p = name; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool SafeFileWriter::open(const char* name, const char* staging, const char* root) {
    if (file_ || owns_temp_) return false;
    error_ = "BAD FILENAME";
    if (!valid_root_name(name) || !staging || !*staging || !root) return false;
    char reserved[96] = {};
    std::snprintf(reserved, sizeof(reserved), "%s.TMP", staging);
    if (equal_name(name, reserved)) return false;
    std::snprintf(reserved, sizeof(reserved), "%s.BAK", staging);
    if (equal_name(name, reserved)) return false;
    if (std::snprintf(target_, sizeof(target_), "%s%s", root, name) >= int(sizeof(target_)) ||
        std::snprintf(temp_, sizeof(temp_), "%s%s.TMP", root, staging) >= int(sizeof(temp_)) ||
        std::snprintf(backup_, sizeof(backup_), "%s%s.BAK", root, staging) >= int(sizeof(backup_))) return false;
    if (!absent(temp_) || !absent(backup_)) {
        error_ = "STAGING FILE EXISTS";
        return false;
    }
    struct stat value;
    if (stat(target_, &value) == 0) {
        if (!S_ISREG(value.st_mode)) { error_ = "NOT A FILE"; return false; }
    } else if (errno != ENOENT) {
        error_ = "SD WRITE ERROR";
        return false;
    }
    error_ = "SD WRITE ERROR";
    file_ = std::fopen(temp_, "wb");
    owns_temp_ = file_ != nullptr;
    bytes_ = 0;
    return file_ != nullptr;
}

bool SafeFileWriter::write(const std::uint8_t* data, std::size_t size) {
    error_ = "SD WRITE ERROR";
    if (!file_ || (!data && size)) return false;
    if (size && std::fwrite(data, 1, size, file_) != size) return false;
    bytes_ += static_cast<std::uint32_t>(size);
    return true;
}

bool SafeFileWriter::commit() {
    if (!file_) return false;
    error_ = "SD WRITE ERROR";
    bool ok = std::fflush(file_) == 0;
    if (std::fclose(file_) != 0) ok = false;
    file_ = nullptr;
    if (!ok) return false;

    const bool had_old = !absent(target_);
    if (had_old && std::rename(target_, backup_) != 0) return false;
    if (std::rename(temp_, target_) != 0) {
        if (had_old && std::rename(backup_, target_) != 0)
            error_ = "SD WRITE ERROR - OLD FILE IN BACKUP";
        return false;
    }
    owns_temp_ = false;
    if (had_old && std::remove(backup_) != 0)
        error_ = "COMPLETE - OLD FILE IN BACKUP";
    else
        error_ = "TRANSFER COMPLETE";
    return true;
}

void SafeFileWriter::abort() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    if (owns_temp_) {
        if (std::remove(temp_) != 0)
            error_ = "SD WRITE ERROR - CHECK STAGING FILES";
        owns_temp_ = false;
    }
}

} // namespace rmb
