#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "safe_file.hpp"

namespace rmb {
// Bounded-memory root-file I/O. Fixed staging names are never overwritten:
// a leftover TMP/BAK requires inspection, preserving interrupted transfers.
class TransferFile {
public:
    ~TransferFile();
    bool open(const char* name, bool receive, const char* root = "/",
              const char* staging = "XMODEM");
    int read(std::uint8_t* out, std::size_t size);
    bool write(const std::uint8_t* data, std::size_t size);
    bool write_exact(const std::uint8_t* data, std::size_t size);
    bool commit();
    bool commit_exact(std::uint32_t expected);
    void abort();
    const char* error() const { return error_; }
    std::uint32_t bytes() const { return bytes_; }
    std::uint32_t size() const { return size_; }
private:
    FILE* file_ = nullptr;
    SafeFileWriter writer_;
    bool receiving_ = false, basic_ = false;
    char target_[192] = {};
    std::uint8_t pending_[128] = {};
    std::size_t pending_size_ = 0;
    std::uint32_t bytes_ = 0, size_ = 0;
    const char* error_ = "SD WRITE ERROR";
};
}
