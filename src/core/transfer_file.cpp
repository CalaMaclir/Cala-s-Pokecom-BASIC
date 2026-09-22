#include "transfer_file.hpp"
#include "program_file_guard.hpp"
#include <cctype>
#include <cstring>
#include <sys/stat.h>

namespace rmb {
namespace {
bool equal_name(const char* a, const char* b) {
    while (*a && *b) {
        if (std::toupper(static_cast<unsigned char>(*a++)) !=
            std::toupper(static_cast<unsigned char>(*b++))) return false;
    }
    return *a == *b;
}
}
TransferFile::~TransferFile() { abort(); }
bool TransferFile::open(const char* name, bool receive, const char* root,const char* staging) {
    if (file_) return false;
    if (program_files::reserved(name) || (receive && program_files::in_use(name))) {
        error_ = "FILE IN USE"; return false;
    }
    error_ = "BAD FILENAME";
    if (!SafeFileWriter::valid_root_name(name) ||
        equal_name(name, "XMODEM.TMP") || equal_name(name, "XMODEM.BAK") ||
        equal_name(name, "YMODEM.TMP") || equal_name(name, "YMODEM.BAK")) return false;
    if (std::snprintf(target_, sizeof(target_), "%s%s", root, name) >= int(sizeof(target_))) return false;
    receiving_ = receive;
    bytes_ = 0; size_ = 0; pending_size_ = 0;
    auto len = std::strlen(name);
    basic_ = len >= 4 && equal_name(name + len - 4, ".BAS");
    if (receive) {
        if (!writer_.open(name, staging, root)) {
            error_ = writer_.error();
            return false;
        }
    } else {
        error_ = "FILE NOT FOUND";
        struct stat s;
        if (stat(target_, &s) != 0 || !S_ISREG(s.st_mode)) return false;
        if(s.st_size<0||static_cast<unsigned long long>(s.st_size)>UINT32_MAX){error_="FILE TOO LARGE";return false;}
        size_=static_cast<std::uint32_t>(s.st_size);
        file_ = std::fopen(target_, "rb");
        if (!file_) error_ = "SD READ ERROR";
    }
    return receive || file_ != nullptr;
}
int TransferFile::read(std::uint8_t* out, std::size_t size) {
    if (!file_ || receiving_) return -1;
    auto n = std::fread(out, 1, size, file_);
    return std::ferror(file_) ? -1 : static_cast<int>(n);
}
bool TransferFile::write(const std::uint8_t* data, std::size_t size) {
    error_ = "SD WRITE ERROR";
    if (!receiving_ || size > sizeof(pending_)) return false;
    if (pending_size_ && !writer_.write(pending_, pending_size_)) return false;
    bytes_ += pending_size_;
    std::memcpy(pending_, data, size);
    pending_size_ = size;
    return true;
}
bool TransferFile::write_exact(const std::uint8_t* data,std::size_t size) {
    error_="SD WRITE ERROR";
    if(!receiving_||pending_size_||!writer_.write(data,size))return false;
    bytes_+=size;return true;
}
bool TransferFile::commit() {
    if (!receiving_) return false;
    error_ = "SD WRITE ERROR";
    // Only BASIC text has a defined EOF padding policy. Binary bytes are exact
    // received blocks; XMODEM has no original-length metadata.
    if (basic_) while (pending_size_ && pending_[pending_size_ - 1] == 0x1a) --pending_size_;
    bool ok = writer_.write(pending_, pending_size_);
    bytes_ += pending_size_; pending_size_ = 0;
    if (ok) ok = writer_.commit();
    error_ = writer_.error();
    return ok;
}
bool TransferFile::commit_exact(std::uint32_t expected) {
    if(!receiving_||pending_size_||bytes_!=expected){error_="PROTOCOL ERROR";return false;}
    bool ok=writer_.commit();error_=writer_.error();return ok;
}
void TransferFile::abort() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    writer_.abort();
}
}
