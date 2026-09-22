#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb {
constexpr std::size_t kMaxRamProgramLines = 256;
constexpr std::size_t kMaxSdProgramLines = 1024;
// Compatibility for RAM capacity displays; never use for SD or compiler limits.
constexpr std::size_t kMaxProgramLines = kMaxRamProgramLines;
constexpr std::size_t kMaxProgramLineLength = 192;
struct ProgramLine { std::int32_t number = 0; char text[kMaxProgramLineLength] = {}; };
enum class ProgramStorageMode : std::uint8_t { Auto, SdCard, InternalRam };
enum class ProgramBackend : std::uint8_t { Ram, Sd };
struct RamProgramStore { ProgramLine lines[kMaxRamProgramLines] = {}; };
struct SdProgramStore {
    struct Entry { std::int32_t number; std::uint32_t offset, hash; };
    Entry* lines = nullptr;
    std::size_t capacity = 0;
    char work[80] = {};
    SdProgramStore() = default;
    ~SdProgramStore();
    SdProgramStore(const SdProgramStore&) = delete;
    SdProgramStore& operator=(const SdProgramStore&) = delete;
    bool reserve(std::size_t count);
};

// Owns either RAM source OR an SD index. Consumers copy one line through
// read_line(); no reference into a backend is exposed. The compiler/VM work
// space is independent of this persistent source owner.
class ProgramStore {
public:
    ProgramStore() = default;
    ~ProgramStore();
    ProgramStore(const ProgramStore&) = delete;
    ProgramStore& operator=(const ProgramStore&) = delete;
    bool initialize(ProgramStorageMode mode);
    bool switch_mode(ProgramStorageMode mode, bool discard = false);
    bool resume();
    // USB ownership invalidates every SD offset/hash. The old index is never
    // re-enabled after host access; resume_after_usb() rebuilds transactionally.
    bool suspend_for_usb();
    void cancel_usb_suspend();
    bool resume_after_usb();
    bool ready() const;
    bool set_line(std::int32_t number, const char* text);
    bool erase_line(std::int32_t number);
    bool clear();
    bool read_line(std::size_t index, ProgramLine& out) const;
    bool load(const char* name);
    bool save(const char* name);
    std::size_t size() const { return count_; }
    std::size_t size_bytes() const { return bytes_; }
    bool is_dirty() const { return dirty_; }
    void set_dirty(bool value) { dirty_ = value; }
    bool suspended() const { return suspended_; }
    ProgramBackend backend_type() const { return backend_; }
    ProgramStorageMode mode() const { return mode_; }
    const char* filename() const { return filename_; }
    const char* error() const { return error_; }
    static const char* mode_name(ProgramStorageMode mode);
    // Root injection for host filesystem tests; firmware uses the SD root.
    void set_root(const char* root) { root_ = root; }
private:
    RamProgramStore* ram_ = nullptr;
    SdProgramStore* sd_ = nullptr;
    std::size_t count_ = 0, bytes_ = 0;
    ProgramBackend backend_ = ProgramBackend::Ram;
    ProgramStorageMode mode_ = ProgramStorageMode::Auto;
    bool dirty_ = false, active_ = false;
    mutable bool suspended_ = false;
    mutable const char* error_ = "OK";
    char filename_[80] = {};
    const char* root_ = "/";
    bool fail(const char* error, bool suspend = false) const;
    bool read_unlocked(std::size_t index, ProgramLine& out) const;
    bool snapshot(const char* target, const ProgramStore& source,
                  SdProgramStore& index, std::size_t& count, std::size_t& bytes,
                  const ProgramLine* edit = nullptr, bool remove = false);
    bool scan(const char* name, SdProgramStore& index, std::size_t& count) const;
    bool verify(const SdProgramStore& index, std::size_t count) const;
    void commit_sd(SdProgramStore* next, std::size_t count, std::size_t bytes);
    bool new_work_name(char* name) const;
    bool edit_sd(std::int32_t number, const char* text, bool remove);
    bool normalize(const char* input, char* output) const;
    void protect();
};
} // namespace rmb
