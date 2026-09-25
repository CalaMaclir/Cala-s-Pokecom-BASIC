#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb {
constexpr std::size_t kMaxRamProgramLines = 256;
constexpr std::size_t kMaxSdProgramLines = 1024;
// Compatibility for RAM capacity displays; never use for SD or compiler limits.
constexpr std::size_t kMaxProgramLines = kMaxRamProgramLines;
constexpr std::size_t kMaxProgramLineLength = 192;
constexpr std::size_t kMaxSdProgramLineLength = 2048;
constexpr std::size_t kSdProgramSourceBufferLength =
    kMaxSdProgramLineLength + 32;
struct ProgramLine { std::int32_t number = 0; char text[kMaxProgramLineLength] = {}; };
static_assert(kMaxProgramLineLength == 192, "RAM line capacity must remain 191 characters");
enum class ProgramStorageMode : std::uint8_t { Auto, SdCard, InternalRam };
enum class ProgramBackend : std::uint8_t { Ram, Sd };
struct RamProgramStore { ProgramLine lines[kMaxRamProgramLines] = {}; };
struct SdProgramStore {
    struct Entry {
        std::int32_t number;
        std::uint32_t offset;
        std::uint32_t hash;
        std::uint16_t length;
    };
    Entry* lines = nullptr;
    std::size_t capacity = 0;
    char work[80] = {};
    // Reused for SD scan/read/verify. It is allocated with the SD backend,
    // never in ProgramLine or a compiler/REPL stack frame.
    mutable char scratch[kSdProgramSourceBufferLength] = {};
    SdProgramStore() = default;
    ~SdProgramStore();
    SdProgramStore(const SdProgramStore&) = delete;
    SdProgramStore& operator=(const SdProgramStore&) = delete;
    bool reserve(std::size_t count);
};

// Owns either RAM source OR an SD index. read_line_text() borrows one line
// until the next SD read; compiler/REPL consumers finish with it immediately.
// The compiler/VM work space is independent of this persistent source owner.
class ProgramStore {
public:
    ProgramStore() = default;
    ~ProgramStore();
    ProgramStore(const ProgramStore&) = delete;
    ProgramStore& operator=(const ProgramStore&) = delete;
    bool initialize(ProgramStorageMode mode);
    bool switch_mode(ProgramStorageMode mode, bool discard = false);
    bool resume();
    // Restore the last verified SD editing workspace described by RMBASIC.SES.
    // discard_current must be explicit when replacing a dirty RAM workspace.
    bool recover_session(ProgramStorageMode mode, bool discard_current = false);
    bool cleanup_orphans();
    bool session_recovered() const { return session_recovered_; }
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
    bool read_line_text(std::size_t index, std::int32_t& number,
                        const char*& text, std::size_t& length) const;
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
    bool session_recovered_ = false;
    mutable bool suspended_ = false;
    mutable const char* error_ = "OK";
    char filename_[80] = {};
    const char* root_ = "/";
    bool fail(const char* error, bool suspend = false) const;
    bool read_text_unlocked(std::size_t index, std::int32_t& number,
                            const char*& text, std::size_t& length) const;
    bool snapshot(const char* target, const ProgramStore& source,
                  SdProgramStore& index, std::size_t& count, std::size_t& bytes,
                  const ProgramLine* edit = nullptr, bool remove = false);
    bool scan(const char* name, SdProgramStore& index, std::size_t& count) const;
    bool verify(const SdProgramStore& index, std::size_t count) const;
    struct SessionMetadata {
        char work[80] = {};
        char filename[80] = {};
        bool dirty = false;
    };
    bool read_session_file(const char* name, SessionMetadata& session) const;
    bool write_session(const char* work, const char* filename, bool dirty);
    bool recover_session_locked(ProgramStorageMode mode);
    bool publish_sd(SdProgramStore* next, std::size_t count, std::size_t bytes,
                    const char* filename, bool dirty);
    bool cleanup_orphans_locked(const char* active_work,
                                const char* session_work);
    bool new_work_name(char* name) const;
    bool edit_sd(std::int32_t number, const char* text, bool remove);
    bool normalize(const char* input, char* output) const;
    void protect();
};
} // namespace rmb
