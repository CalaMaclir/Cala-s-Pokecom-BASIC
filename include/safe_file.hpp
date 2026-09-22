#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rmb {

// Transactional writer for one file in the SD root. Data is written to a
// fixed staging file and becomes visible only after flush/close and rename.
class SafeFileWriter {
public:
    ~SafeFileWriter();
    static bool valid_root_name(const char* name);
    bool open(const char* name, const char* staging, const char* root = "/");
    bool write(const std::uint8_t* data, std::size_t size);
    bool commit();
    void abort();
    const char* error() const { return error_; }
    std::uint32_t bytes() const { return bytes_; }

private:
    FILE* file_ = nullptr;
    bool owns_temp_ = false;
    char target_[192] = {};
    char temp_[192] = {};
    char backup_[192] = {};
    std::uint32_t bytes_ = 0;
    const char* error_ = "SD WRITE ERROR";
};

} // namespace rmb
