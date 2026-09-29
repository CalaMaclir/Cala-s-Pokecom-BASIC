#pragma once
#include <cstddef>
#include <cstdint>

namespace rmb {
constexpr std::size_t kMaxRamProgramLines = 256;
constexpr std::size_t kMaxSdProgramLines = 1024;
constexpr std::size_t kMaxPsramProgramLines = kMaxSdProgramLines;
// Compatibility for SRAM fallback/direct-input boundaries.
constexpr std::size_t kMaxProgramLines = kMaxRamProgramLines;
constexpr std::size_t kMaxProgramLineLength = 192;
constexpr std::size_t kMaxSdProgramLineLength = 2048;
constexpr std::size_t kMaxPsramProgramLineLength = kMaxSdProgramLineLength;
constexpr std::size_t kSdProgramSourceBufferLength =
    kMaxSdProgramLineLength + 32;
struct ProgramLine { std::int32_t number = 0; char text[kMaxProgramLineLength] = {}; };
static_assert(kMaxProgramLineLength == 192, "RAM line capacity must remain 191 characters");
enum class ProgramStorageMode : std::uint8_t { Auto, SdCard, InternalRam };
enum class ProgramBackend : std::uint8_t { Ram, Sd };
struct RamProgramStore { ProgramLine lines[kMaxRamProgramLines] = {}; };

struct PsramProgramIndexEntry {
    std::int32_t number = 0;
    std::uint32_t hash = 0;
    std::uint16_t slot = 0;
    std::uint16_t length = 0;
};
static_assert(
    sizeof(PsramProgramIndexEntry) == 12,
    "PSRAM ProgramStore index must stay compact");
constexpr std::uint32_t kPsramProgramIndexBytes =
    static_cast<std::uint32_t>(
        sizeof(PsramProgramIndexEntry) * kMaxPsramProgramLines);
constexpr std::uint32_t kPsramProgramTextBytes =
    static_cast<std::uint32_t>(
        kMaxPsramProgramLineLength * kMaxPsramProgramLines);
constexpr std::uint32_t kPsramProgramStoreBytes =
    kPsramProgramIndexBytes + kPsramProgramTextBytes;

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
    using LineVisitor = bool (*)(
        std::size_t index,
        std::int32_t number,
        const char* body,
        std::size_t length,
        void* context
    );

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
    bool read_line_metadata(
        std::size_t index,
        std::int32_t& number,
        std::size_t& length
    ) const;
    bool read_line_text(std::size_t index, std::int32_t& number,
                        const char*& text, std::size_t& length) const;
    bool visit_line_range(
        std::size_t first,
        std::size_t count,
        LineVisitor visitor,
        void* context
    ) const;
    bool load(const char* name);
    bool save(const char* name);
    bool note_source_renamed(
        const char* old_name,
        const char* new_name
    );
    std::size_t size() const { return count_; }
    std::size_t size_bytes() const { return bytes_; }
    std::size_t line_capacity() const;
    std::size_t line_length_capacity() const;
    bool internal_psram_extended() const {
        return backend_ == ProgramBackend::Ram && ram_psram_;
    }
    std::uint64_t revision() const { return revision_; }
    std::uint32_t sd_cache_hits() const { return sd_cache_hits_; }
    std::uint32_t sd_cache_misses() const { return sd_cache_misses_; }
    std::uint32_t sd_cache_bytes() const { return sd_cache_bytes_; }
    void release_sd_cache();
    bool is_dirty() const { return dirty_; }
    bool set_dirty(bool value);
    bool suspended() const { return suspended_; }
    ProgramBackend backend_type() const { return backend_; }
    ProgramStorageMode mode() const { return mode_; }
    const char* filename() const { return filename_; }
    const char* error() const { return error_; }
    static const char* mode_name(ProgramStorageMode mode);
    // Root injection for host filesystem tests; firmware uses the SD root.
    void set_root(const char* root) { root_ = root; }
private:
    struct PendingEdit {
        std::int32_t number = 0;
        const char* text = nullptr;
        std::size_t length = 0;
    };
    RamProgramStore* ram_ = nullptr;
    // INTERNAL mode prefers an indexed 1024 x 2047 PSRAM store. The index
    // moves independently from 2 KiB text slots, so insertion/deletion shifts
    // only compact metadata. If the large PSRAM region cannot be claimed,
    // the legacy 256 x 191 SRAM backend remains available.
    bool ram_psram_ = false;
    std::uint32_t ram_psram_base_ = 0;
    std::uint32_t ram_psram_bytes_ = 0;
    mutable char* ram_long_scratch_ = nullptr;

    struct SdCacheMeta {
        std::uint32_t hash = 0;
        std::uint16_t index = 0;
        std::uint16_t length = 0;
        bool valid = false;
    };
    SdProgramStore* sd_ = nullptr;
    mutable SdCacheMeta* sd_cache_meta_ = nullptr;
    mutable std::size_t sd_cache_slots_ = 0;
    mutable std::size_t sd_cache_next_ = 0;
    mutable std::uint32_t sd_cache_base_ = 0;
    mutable std::uint32_t sd_cache_bytes_ = 0;
    mutable std::uint32_t sd_cache_hits_ = 0;
    mutable std::uint32_t sd_cache_misses_ = 0;
    std::size_t count_ = 0, bytes_ = 0;
    std::uint64_t revision_ = 1;
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
                  const PendingEdit* edit = nullptr, bool remove = false);
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
    bool edit_sd(std::int32_t number, const char* text,
                 std::size_t length, bool remove);
    bool normalize(const char* input, char* output) const;
    void bump_revision();
    bool ensure_ram_store();
    bool migrate_ram_to_psram();
    void release_ram_store();
    bool ram_read_record(std::size_t index, ProgramLine& out) const;
    bool ram_write_record(std::size_t index, const ProgramLine& line);
    bool psram_read_index(
        std::size_t index,
        PsramProgramIndexEntry& entry
    ) const;
    bool psram_write_index(
        std::size_t index,
        const PsramProgramIndexEntry& entry
    );
    bool psram_read_text(
        const PsramProgramIndexEntry& entry,
        const char*& text
    ) const;
    bool psram_write_text(
        std::uint16_t slot,
        const char* text,
        std::size_t length
    );
    bool psram_set_line(
        std::int32_t number,
        const char* text,
        std::size_t length
    );
    bool psram_erase_line(std::int32_t number);
    std::uint32_t psram_text_base() const {
        return ram_psram_base_ + kPsramProgramIndexBytes;
    }
    bool ensure_sd_cache() const;
    void invalidate_sd_cache() const;
    bool sd_cache_lookup(
        std::size_t index,
        const SdProgramStore::Entry& entry,
        const char*& text,
        std::size_t& length
    ) const;
    void sd_cache_store(
        std::size_t index,
        const SdProgramStore::Entry& entry,
        const char* text,
        std::size_t length
    ) const;
    void protect();
};
} // namespace rmb
