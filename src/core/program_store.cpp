#include "program_store.hpp"
#include "program_file_guard.hpp"
#include "file_management.hpp"
#include "file_path.hpp"
#include "safe_file.hpp"
#include "editor_perf.hpp"
#include "storage.hpp"
#include "psram.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <new>
#include <memory>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace rmb {
SdProgramStore::~SdProgramStore() { std::free(lines); }
bool SdProgramStore::reserve(std::size_t count) {
    if(count<=capacity) return true;
    if(count>kMaxSdProgramLines) return false;
    const std::size_t next=(count+63)/64*64;
    void* memory=std::realloc(lines,next*sizeof(Entry));
    if(!memory) return false;
    lines=static_cast<Entry*>(memory); capacity=next; return true;
}
namespace {
// Allocate backend owners without
// pulling throwing operator new and its emergency exception pool into SRAM.
// Placement construction establishes lifetime; SD owns a bounded dynamic index.
template<class T> T* allocate_backend() {
    void* memory=std::malloc(sizeof(T));
    return memory ? new(memory) T{} : nullptr;
}
template<class T> void release_backend(T* value) {
    if(value) { value->~T(); std::free(value); }
}
template<class T> struct BackendDeleter {
    void operator()(T* value) const { release_backend(value); }
};
template<class T> using BackendPtr=std::unique_ptr<T,BackendDeleter<T>>;
struct Lease {
    bool locked;
    Lease() : locked(storage::init() && storage::try_lock()) {}
    ~Lease() { if (locked) storage::unlock(); }
};
std::uint32_t hash_line(
    std::int32_t number, const char* text, std::size_t length
) {
    std::uint32_t h=2166136261u ^ static_cast<std::uint32_t>(number);
    for(std::size_t i=0;i<length;++i)
        h=(h ^ static_cast<unsigned char>(text[i]))*16777619u;
    return h;
}
bool parse_text(
    char* text,std::int32_t& number,char*& body,std::size_t& length,
    bool canonical,std::size_t capacity
) {
    char* p=text; while (*p==' ' || *p=='\t') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    std::int64_t n=0;
    while (std::isdigit(static_cast<unsigned char>(*p))) {
        n=n*10+(*p++-'0'); if(n>INT32_MAX) return false;
    }
    if(canonical) { if(*p==' ') ++p; }
    else if (*p==' ' || *p=='\t') ++p;
    length=std::strlen(p);
    while(length && (p[length-1]=='\r' || p[length-1]=='\n'))
        p[--length]=0;
    if(length>=capacity) return false;
    number=static_cast<std::int32_t>(n);body=p;return true;
}
bool read_sd_entry(
    FILE* file,
    const SdProgramStore::Entry& entry,
    char* scratch,
    std::size_t capacity,
    std::int32_t& number,
    const char*& text,
    std::size_t& length,
    bool canonical,
    ProgramSourceMode mode = ProgramSourceMode::ClassicNumbered
) {
    if(!file || !scratch || capacity==0 ||
       std::fseek(file,entry.offset,SEEK_SET)!=0 ||
       !std::fgets(scratch,capacity,file)) return false;
    const std::size_t raw_length=std::strlen(scratch);
    if(raw_length==capacity-1 && scratch[raw_length-1]!='\n') return false;

    char* body=nullptr;
    std::int32_t parsed=0;
    std::size_t body_length=0;
    bool valid = false;
    if (mode == ProgramSourceMode::Structured) {
        body = scratch; parsed = entry.number; body_length = std::strlen(body);
        while (body_length && (body[body_length-1]=='\r' || body[body_length-1]=='\n'))
            body[--body_length]=0;
        valid = body_length < kMaxSdProgramLineLength;
    } else valid = parse_text(scratch,parsed,body,body_length,canonical,kMaxSdProgramLineLength);
    if(!valid ||
       parsed!=entry.number || body_length!=entry.length ||
       hash_line(parsed,body,body_length)!=entry.hash) return false;
    number=parsed;
    text=body;
    length=body_length;
    return true;
}
bool parse(char* text,ProgramLine& line,bool canonical=false) {
    char* body=nullptr;std::size_t length=0;std::int32_t number=0;
    if(!parse_text(text,number,body,length,canonical,kMaxProgramLineLength))
        return false;
    line.number=number;std::memcpy(line.text,body,length+1);return true;
}
std::size_t line_bytes(
    std::int32_t number,const char*,std::size_t length
) {
    char n[16];
    return static_cast<std::size_t>(
        std::snprintf(n,sizeof(n),"%ld ",static_cast<long>(number)))+
        length+1;
}
std::size_t line_bytes(const ProgramLine& line) {
    return line_bytes(line.number,line.text,std::strlen(line.text));
}
bool work_file_name(const char* name) {
    if(!name || std::strlen(name)!=12) return false;
    if(std::toupper(static_cast<unsigned char>(name[0]))!='R' ||
       std::toupper(static_cast<unsigned char>(name[1]))!='M' ||
       std::toupper(static_cast<unsigned char>(name[2]))!='B' ||
       std::toupper(static_cast<unsigned char>(name[3]))!='P') return false;
    for(int i=4;i<8;++i)
        if(!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    return name[8]=='.' &&
           std::toupper(static_cast<unsigned char>(name[9]))=='B' &&
           std::toupper(static_cast<unsigned char>(name[10]))=='A' &&
           std::toupper(static_cast<unsigned char>(name[11]))=='S';
}
void remove_work(const char* root,const char* name) {
    if(!work_file_name(name)) return;
    char path[192]; std::snprintf(path,sizeof(path),"%s%s",root,name); std::remove(path);
}
}
ProgramStore::~ProgramStore() {
    release_sd_cache();
    release_ram_store();
    release_backend(sd_);
    if(active_) program_files::set_active(nullptr);
}

std::size_t ProgramStore::line_capacity() const {
    if (backend_ == ProgramBackend::Sd) return kMaxSdProgramLines;
    return ram_psram_ ? kMaxPsramProgramLines : kMaxRamProgramLines;
}

std::size_t ProgramStore::line_length_capacity() const {
    if (backend_ == ProgramBackend::Sd)
        return kMaxSdProgramLineLength - 1;
    return ram_psram_
        ? kMaxPsramProgramLineLength - 1
        : kMaxProgramLineLength - 1;
}

bool ProgramStore::ensure_ram_store() {
    if (ram_psram_ || ram_) return true;

    if (psram::init()) {
        std::uint32_t base = 0;
        std::uint32_t allocated = 0;
        if (psram::claim(
                psram::Client::ProgramStore,
                kPsramProgramStoreBytes,
                base,
                allocated) &&
            allocated >= kPsramProgramStoreBytes) {
            char* scratch = static_cast<char*>(
                std::malloc(kMaxPsramProgramLineLength));
            if (scratch &&
                psram::fill(base, 0, kPsramProgramIndexBytes)) {
                ram_psram_ = true;
                ram_psram_base_ = base;
                ram_psram_bytes_ = allocated;
                ram_long_scratch_ = scratch;
                return true;
            }
            std::free(scratch);
            psram::release(psram::Client::ProgramStore);
        }
    }

    ram_ = allocate_backend<RamProgramStore>();
    return ram_ != nullptr;
}

bool ProgramStore::migrate_ram_to_psram() {
    if (ram_psram_) return true;
    if (!ram_) return ensure_ram_store();
    if (!psram::init() || count_ > kMaxPsramProgramLines) return false;

    std::uint32_t base = 0;
    std::uint32_t allocated = 0;
    if (!psram::claim(
            psram::Client::ProgramStore,
            kPsramProgramStoreBytes,
            base,
            allocated) ||
        allocated < kPsramProgramStoreBytes) {
        return false;
    }

    char* scratch = static_cast<char*>(
        std::malloc(kMaxPsramProgramLineLength));
    auto* entries = static_cast<PsramProgramIndexEntry*>(
        std::malloc(
            (count_ == 0 ? 1 : count_) *
            sizeof(PsramProgramIndexEntry)));
    if (!scratch || !entries ||
        !psram::fill(base, 0, kPsramProgramIndexBytes)) {
        std::free(scratch);
        std::free(entries);
        psram::release(psram::Client::ProgramStore);
        return false;
    }

    bool ok = true;
    const std::uint32_t text_base =
        base + kPsramProgramIndexBytes;
    for (std::size_t i = 0; i < count_; ++i) {
        const ProgramLine& line = ram_->lines[i];
        const std::size_t length = std::strlen(line.text);
        entries[i].number = line.number;
        entries[i].hash = hash_line(line.number, line.text, length);
        entries[i].slot = static_cast<std::uint16_t>(i);
        entries[i].length = static_cast<std::uint16_t>(length);
        if (!psram::write(
                text_base +
                    static_cast<std::uint32_t>(
                        i * kMaxPsramProgramLineLength),
                line.text,
                length + 1u)) {
            ok = false;
            break;
        }
    }
    if (ok && count_ != 0) {
        ok = psram::write(
            base,
            entries,
            count_ * sizeof(PsramProgramIndexEntry));
    }

    std::free(entries);
    if (!ok) {
        std::free(scratch);
        psram::release(psram::Client::ProgramStore);
        return false;
    }

    release_backend(ram_);
    ram_ = nullptr;
    ram_psram_ = true;
    ram_psram_base_ = base;
    ram_psram_bytes_ = allocated;
    ram_long_scratch_ = scratch;
    return true;
}

void ProgramStore::release_ram_store() {
    release_backend(ram_);
    ram_ = nullptr;
    if (ram_psram_) psram::release(psram::Client::ProgramStore);
    ram_psram_ = false;
    ram_psram_base_ = 0;
    ram_psram_bytes_ = 0;
    std::free(ram_long_scratch_);
    ram_long_scratch_ = nullptr;
}

bool ProgramStore::psram_read_index(
    std::size_t index,
    PsramProgramIndexEntry& entry
) const {
    if (!ram_psram_ || index >= kMaxPsramProgramLines) return false;
    return psram::read(
        ram_psram_base_ +
            static_cast<std::uint32_t>(
                index * sizeof(PsramProgramIndexEntry)),
        &entry,
        sizeof(entry));
}

bool ProgramStore::psram_write_index(
    std::size_t index,
    const PsramProgramIndexEntry& entry
) {
    if (!ram_psram_ || index >= kMaxPsramProgramLines) return false;
    return psram::write(
        ram_psram_base_ +
            static_cast<std::uint32_t>(
                index * sizeof(PsramProgramIndexEntry)),
        &entry,
        sizeof(entry));
}

bool ProgramStore::psram_read_text(
    const PsramProgramIndexEntry& entry,
    const char*& text
) const {
    text = nullptr;
    if (!ram_psram_ || !ram_long_scratch_ ||
        entry.slot >= kMaxPsramProgramLines ||
        entry.length >= kMaxPsramProgramLineLength) {
        return false;
    }
    if (!psram::read(
            psram_text_base() +
                static_cast<std::uint32_t>(
                    entry.slot * kMaxPsramProgramLineLength),
            ram_long_scratch_,
            static_cast<std::size_t>(entry.length) + 1u)) {
        return false;
    }
    if (ram_long_scratch_[entry.length] != '\0' ||
        hash_line(
            entry.number,
            ram_long_scratch_,
            entry.length) != entry.hash) {
        return false;
    }
    text = ram_long_scratch_;
    return true;
}

bool ProgramStore::psram_write_text(
    std::uint16_t slot,
    const char* text,
    std::size_t length
) {
    if (!ram_psram_ || !text ||
        slot >= kMaxPsramProgramLines ||
        length >= kMaxPsramProgramLineLength) {
        return false;
    }
    return psram::write(
        psram_text_base() +
            static_cast<std::uint32_t>(
                slot * kMaxPsramProgramLineLength),
        text,
        length + 1u);
}

bool ProgramStore::psram_set_line(
    std::int32_t number,
    const char* text,
    std::size_t length
) {
    if (!ram_psram_ || !text ||
        length >= kMaxPsramProgramLineLength) {
        return fail("LINE TOO LONG FOR INTERNAL PSRAM");
    }

    const std::size_t capacity =
        count_ < kMaxPsramProgramLines ? count_ + 1u : count_;
    auto* entries = static_cast<PsramProgramIndexEntry*>(
        std::malloc(
            (capacity == 0 ? 1 : capacity) *
            sizeof(PsramProgramIndexEntry)));
    if (!entries) return fail("OUT OF MEMORY");

    if (count_ != 0 &&
        !psram::read(
            ram_psram_base_,
            entries,
            count_ * sizeof(PsramProgramIndexEntry))) {
        std::free(entries);
        return fail("PSRAM READ ERROR");
    }

    std::size_t pos = 0;
    while (pos < count_ && entries[pos].number < number) ++pos;
    const bool exists =
        pos < count_ && entries[pos].number == number;

    if (!exists && count_ >= kMaxPsramProgramLines) {
        std::free(entries);
        return fail("PROGRAM TOO LARGE FOR INTERNAL MODE");
    }

    std::uint8_t used[
        (kMaxPsramProgramLines + 7u) / 8u] = {};
    for (std::size_t i = 0; i < count_; ++i) {
        const std::size_t slot = entries[i].slot;
        if (slot < kMaxPsramProgramLines) {
            used[slot >> 3] |= static_cast<std::uint8_t>(
                1u << (slot & 7u));
        }
    }

    std::uint16_t slot = exists
        ? entries[pos].slot
        : static_cast<std::uint16_t>(kMaxPsramProgramLines);
    for (std::size_t candidate = 0;
         candidate < kMaxPsramProgramLines;
         ++candidate) {
        if ((used[candidate >> 3] &
             static_cast<std::uint8_t>(
                 1u << (candidate & 7u))) == 0) {
            slot = static_cast<std::uint16_t>(candidate);
            break;
        }
    }
    if (slot >= kMaxPsramProgramLines) {
        if (!exists) {
            std::free(entries);
            return fail("PROGRAM TOO LARGE FOR INTERNAL MODE");
        }
        slot = entries[pos].slot;
    }

    if (!psram_write_text(slot, text, length)) {
        std::free(entries);
        return fail("PSRAM WRITE ERROR");
    }

    std::size_t new_count = count_;
    const std::size_t replaced_bytes = exists
        ? line_bytes(
              entries[pos].number,
              nullptr,
              entries[pos].length)
        : 0u;
    if (!exists) {
        std::memmove(
            entries + pos + 1,
            entries + pos,
            (count_ - pos) * sizeof(PsramProgramIndexEntry));
        ++new_count;
    }

    entries[pos].number = number;
    entries[pos].hash = hash_line(number, text, length);
    entries[pos].slot = slot;
    entries[pos].length = static_cast<std::uint16_t>(length);

    if (!psram::write(
            ram_psram_base_,
            entries,
            new_count * sizeof(PsramProgramIndexEntry))) {
        std::free(entries);
        return fail("PSRAM WRITE ERROR");
    }

    std::free(entries);
    count_ = new_count;
    bytes_ = bytes_ - replaced_bytes +
        line_bytes(number, text, length);
    dirty_ = true;
    bump_revision();
    error_ = "OK";
    return true;
}

bool ProgramStore::psram_erase_line(std::int32_t number) {
    if (!ram_psram_ || count_ == 0) return true;

    auto* entries = static_cast<PsramProgramIndexEntry*>(
        std::malloc(count_ * sizeof(PsramProgramIndexEntry)));
    if (!entries) return fail("OUT OF MEMORY");
    if (!psram::read(
            ram_psram_base_,
            entries,
            count_ * sizeof(PsramProgramIndexEntry))) {
        std::free(entries);
        return fail("PSRAM READ ERROR");
    }

    std::size_t pos = 0;
    while (pos < count_ && entries[pos].number < number) ++pos;
    if (pos == count_ || entries[pos].number != number) {
        std::free(entries);
        return true;
    }

    const std::size_t removed_bytes = line_bytes(
        entries[pos].number,
        nullptr,
        entries[pos].length);
    std::memmove(
        entries + pos,
        entries + pos + 1,
        (count_ - pos - 1u) * sizeof(PsramProgramIndexEntry));

    const std::size_t new_count = count_ - 1u;
    if (new_count != 0 &&
        !psram::write(
            ram_psram_base_,
            entries,
            new_count * sizeof(PsramProgramIndexEntry))) {
        std::free(entries);
        return fail("PSRAM WRITE ERROR");
    }

    std::free(entries);
    count_ = new_count;
    bytes_ -= removed_bytes;
    dirty_ = true;
    bump_revision();
    error_ = "OK";
    return true;
}

bool ProgramStore::ram_read_record(
    std::size_t index,
    ProgramLine& out
) const {
    if (index >= count_) return false;
    if (ram_psram_) {
        PsramProgramIndexEntry entry;
        const char* text = nullptr;
        if (!psram_read_index(index, entry) ||
            entry.length >= kMaxProgramLineLength ||
            !psram_read_text(entry, text)) {
            return false;
        }
        out.number = entry.number;
        std::memcpy(out.text, text, entry.length + 1u);
        return true;
    }
    if (!ram_ || index >= kMaxRamProgramLines) return false;
    out = ram_->lines[index];
    return true;
}

bool ProgramStore::ram_write_record(
    std::size_t index,
    const ProgramLine& line
) {
    if (ram_psram_) return false;
    if (!ram_ || index >= kMaxRamProgramLines) return false;
    ram_->lines[index] = line;
    return true;
}

void ProgramStore::release_sd_cache() {
    if(sd_cache_bytes_!=0)
        psram::release(psram::Client::SdCache);
    std::free(sd_cache_meta_);
    sd_cache_meta_=nullptr;
    sd_cache_slots_=0;
    sd_cache_next_=0;
    sd_cache_base_=0;
    sd_cache_bytes_=0;
    sd_cache_hits_=0;
    sd_cache_misses_=0;
}

bool ProgramStore::ensure_sd_cache() const {
    if(sd_cache_meta_&&sd_cache_slots_!=0&&sd_cache_bytes_!=0)
        return true;
    if(!sd_||!psram::init()) return false;

    constexpr std::uint32_t requested=64u*1024u;
    constexpr std::uint32_t slot_bytes=
        static_cast<std::uint32_t>(kMaxSdProgramLineLength);
    std::uint32_t base=0,allocated=0;
    if(!psram::claim(
           psram::Client::SdCache,requested,base,allocated) ||
       allocated<slot_bytes) return false;

    const std::size_t slots=allocated/slot_bytes;
    auto* meta=static_cast<SdCacheMeta*>(
        std::calloc(slots,sizeof(SdCacheMeta)));
    if(!meta) {
        psram::release(psram::Client::SdCache);
        return false;
    }

    sd_cache_meta_=meta;
    sd_cache_slots_=slots;
    sd_cache_next_=0;
    sd_cache_base_=base;
    sd_cache_bytes_=static_cast<std::uint32_t>(slots*slot_bytes);
    return true;
}

void ProgramStore::invalidate_sd_cache() const {
    if(sd_cache_meta_&&sd_cache_slots_)
        std::memset(
            sd_cache_meta_,0,
            sd_cache_slots_*sizeof(SdCacheMeta));
    sd_cache_next_=0;
    sd_cache_hits_=0;
    sd_cache_misses_=0;
}

bool ProgramStore::sd_cache_lookup(
    std::size_t index,
    const SdProgramStore::Entry& entry,
    const char*& text,
    std::size_t& length
) const {
    if(!ensure_sd_cache()) {
        ++sd_cache_misses_;
        return false;
    }
    constexpr std::uint32_t slot_bytes=
        static_cast<std::uint32_t>(kMaxSdProgramLineLength);
    for(std::size_t slot=0;slot<sd_cache_slots_;++slot) {
        auto& meta=sd_cache_meta_[slot];
        if(!meta.valid||meta.index!=index||
           meta.length!=entry.length||meta.hash!=entry.hash)
            continue;
        if(!psram::read(
               sd_cache_base_+
                   static_cast<std::uint32_t>(slot)*slot_bytes,
               sd_->scratch,
               static_cast<std::size_t>(entry.length)+1u) ||
           sd_->scratch[entry.length]!=0 ||
           hash_line(entry.number,sd_->scratch,entry.length)!=entry.hash) {
            meta.valid=false;
            break;
        }
        ++sd_cache_hits_;
        text=sd_->scratch;
        length=entry.length;
        return true;
    }
    ++sd_cache_misses_;
    return false;
}

void ProgramStore::sd_cache_store(
    std::size_t index,
    const SdProgramStore::Entry& entry,
    const char* text,
    std::size_t length
) const {
    if(!text||length!=entry.length||
       length>=kMaxSdProgramLineLength||
       !ensure_sd_cache()||sd_cache_slots_==0) return;

    constexpr std::uint32_t slot_bytes=
        static_cast<std::uint32_t>(kMaxSdProgramLineLength);
    const std::size_t slot=sd_cache_next_++%sd_cache_slots_;
    if(!psram::write(
           sd_cache_base_+
               static_cast<std::uint32_t>(slot)*slot_bytes,
           text,length+1u)) return;
    sd_cache_meta_[slot].hash=entry.hash;
    sd_cache_meta_[slot].index=static_cast<std::uint16_t>(index);
    sd_cache_meta_[slot].length=entry.length;
    sd_cache_meta_[slot].valid=true;
}

void ProgramStore::bump_revision() {
    ++revision_;
    if(revision_==0) revision_=1;
}

bool ProgramStore::fail(const char* error,bool suspend) const { error_=error; if(suspend) suspended_=true; return false; }
const char* ProgramStore::mode_name(ProgramStorageMode mode) {
    return mode==ProgramStorageMode::Auto ? "AUTO" : mode==ProgramStorageMode::SdCard ? "SD CARD" : "INTERNAL RAM";
}
void ProgramStore::protect() { if(active_) program_files::set_active(backend_==ProgramBackend::Sd ? filename_ : nullptr); }
bool ProgramStore::normalize(const char* input,char* output) const {
    if(!file_management::normalize_program_name(input,output,80) ||
       program_files::reserved(output)) return fail("BAD FILENAME");
    return true;
}
bool ProgramStore::ready() const {
    if(suspended_) return fail("PROGRAM STORAGE SUSPENDED - RESUME IN MENU");
    if(backend_==ProgramBackend::Ram) return true;
    if(!storage::available() || !storage::card_present()) return fail("SD CARD REMOVED - PROGRAM STORAGE SUSPENDED",true);
    return true;
}
bool ProgramStore::set_dirty(bool value) {
    if(dirty_==value)return true;
    if(backend_==ProgramBackend::Sd) {
        if(!ready()||!sd_||!work_file_name(sd_->work))
            return fail("BAD SESSION STATE");
        Lease lease;if(!lease.locked)return fail(storage::last_error());
        // Keep the recovery metadata consistent with the in-memory flag.
        // This is only written when the state changes, never per keystroke.
        if(!write_session(sd_->work,filename_,value,source_mode_))return false;
    }
    dirty_=value;error_="OK";return true;
}
bool ProgramStore::read_text_unlocked(
    std::size_t i,std::int32_t& number,const char*& text,
    std::size_t& length
) const {
    if(i>=count_) return fail("BAD LINE INDEX");
    if(backend_==ProgramBackend::Ram) {
        if(ram_psram_) {
            PsramProgramIndexEntry entry;
            if(!psram_read_index(i,entry) ||
               !psram_read_text(entry,text))
                return fail("PSRAM READ ERROR");
            number=entry.number;
            length=entry.length;
            return true;
        }
        if(!ram_) return fail("RAM PROGRAM STORAGE ERROR");
        number=ram_->lines[i].number;
        text=ram_->lines[i].text;
        length=std::strlen(text);
        return true;
    }
    const auto& entry=sd_->lines[i];
    if(sd_cache_lookup(i,entry,text,length)) {
        number=entry.number;
        return true;
    }
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,sd_->work);
    FILE* f=std::fopen(path,"rb");
    if(!f)return fail("SD READ ERROR - PROGRAM STORAGE SUSPENDED",true);
    editor_perf::sd_open();
    bool ok=read_sd_entry(
        f,entry,sd_->scratch,sizeof(sd_->scratch),
        number,text,length,program_files::reserved(sd_->work),sd_->source_mode);
    if(std::fclose(f)!=0)ok=false;
    if(ok) sd_cache_store(i,entry,text,length);
    if(!ok)
        return fail("SD SOURCE CHANGED - PROGRAM STORAGE SUSPENDED",true);
    return true;
}
bool ProgramStore::read_line_metadata(
    std::size_t i,
    std::int32_t& number,
    std::size_t& length
) const {
    if(!ready()) return false;
    if(i>=count_) return fail("BAD LINE INDEX");
    if(backend_==ProgramBackend::Ram) {
        if(ram_psram_) {
            PsramProgramIndexEntry entry;
            if(!psram_read_index(i,entry))
                return fail("PSRAM READ ERROR");
            number=entry.number;
            length=entry.length;
        } else {
            if(!ram_) return fail("RAM PROGRAM STORAGE ERROR");
            number=ram_->lines[i].number;
            length=std::strlen(ram_->lines[i].text);
        }
    } else {
        number=sd_->lines[i].number;
        length=sd_->lines[i].length;
    }
    error_="OK";
    return true;
}

bool ProgramStore::read_line_text(
    std::size_t i,std::int32_t& number,const char*& text,
    std::size_t& length
) const {
    if(!ready())return false;
    if(backend_==ProgramBackend::Ram)
        return read_text_unlocked(i,number,text,length);
    Lease lease;if(!lease.locked)return fail(storage::last_error());
    return read_text_unlocked(i,number,text,length);
}
bool ProgramStore::visit_line_range(
    std::size_t first,
    std::size_t requested,
    LineVisitor visitor,
    void* context
) const {
    if(!visitor) return fail("BAD LINE VISITOR");
    if(!ready()) return false;
    if(requested==0) return true;
    if(first>=count_) return fail("BAD LINE INDEX");
    const std::size_t available=count_-first;
    const std::size_t count=requested<available ? requested : available;

    if(backend_==ProgramBackend::Ram) {
        for(std::size_t offset=0;offset<count;++offset) {
            const std::size_t index=first+offset;
            std::int32_t number=0;
            const char* body=nullptr;
            std::size_t length=0;
            if(!read_text_unlocked(index,number,body,length))
                return false;
            if(!visitor(index,number,body,length,context))
                return fail("LINE VISITOR STOPPED");
        }
        error_="OK";
        return true;
    }

    Lease lease;
    if(!lease.locked) return fail(storage::last_error());
    char path[192];
    std::snprintf(path,sizeof(path),"%s%s",root_,sd_->work);
    FILE* file=std::fopen(path,"rb");
    if(!file)
        return fail("SD READ ERROR - PROGRAM STORAGE SUSPENDED",true);
    editor_perf::sd_open();

    bool ok=true;
    for(std::size_t offset=0;offset<count;++offset) {
        const std::size_t index=first+offset;
        std::int32_t number=0;
        const char* body=nullptr;
        std::size_t length=0;
        if(!read_sd_entry(
                file,sd_->lines[index],sd_->scratch,sizeof(sd_->scratch),
                number,body,length,program_files::reserved(sd_->work),sd_->source_mode)) {
            ok=false;
            break;
        }
        sd_cache_store(index,sd_->lines[index],body,length);
        if(!visitor(index,number,body,length,context)) {
            std::fclose(file);
            return fail("LINE VISITOR STOPPED");
        }
    }
    if(std::fclose(file)!=0) ok=false;
    if(!ok)
        return fail("SD SOURCE CHANGED - PROGRAM STORAGE SUSPENDED",true);
    error_="OK";
    return true;
}

bool ProgramStore::read_line(std::size_t i,ProgramLine& out) const {
    std::int32_t number=0;const char* text=nullptr;std::size_t length=0;
    if(!read_line_text(i,number,text,length))return false;
    if(length>=kMaxProgramLineLength)return fail("LINE TOO LONG FOR RAM");
    out.number=number;std::memcpy(out.text,text,length+1);return true;
}
bool ProgramStore::new_work_name(char* name) const {
    for(unsigned i=0;i<10000;++i) {
        std::snprintf(name,80,"RMBP%04u.BAS",i);
        if(sd_ && program_files::equal(name,sd_->work)) continue;
        char path[192]; std::snprintf(path,sizeof(path),"%s%s",root_,name);
        struct stat st; if(stat(path,&st)!=0) { if(errno==ENOENT) return true; return fail("SD READ ERROR"); }
    }
    return fail("TOO MANY PROGRAM WORK FILES");
}
// Produces canonical text without a large stack line buffer. Index becomes
// live only after SafeFileWriter has flushed, closed and committed the file.
bool ProgramStore::snapshot(
    const char* target,const ProgramStore& source,SdProgramStore& index,
    std::size_t& count,std::size_t& bytes,const PendingEdit* edit,bool remove,
    const PendingEdit* second_edit, const RowEdit* row_edit
) {
    index.source_mode = source.source_mode_;
    SafeFileWriter writer;
    if(!writer.open(target,"RMBEDIT",root_))return fail(writer.error());
    count=0;bytes=0;
    const PendingEdit* edits[2] = {edit, second_edit};
    const std::size_t edit_count = second_edit ? 2u : edit ? 1u : 0u;
    if (edit_count == 2 && edits[0]->number > edits[1]->number)
        std::swap(edits[0], edits[1]);
    std::size_t edit_index = 0;
    auto removed = [&](const PendingEdit* item) {
        return item->text == nullptr || (item == edit && remove);
    };
    auto emit=[&](std::int32_t number,const char* text,std::size_t length) {
        if(count>=kMaxSdProgramLines)return fail("SD PROGRAM FULL");
        if(length>=kMaxSdProgramLineLength)return fail("SD LINE TOO LONG");
        if(!index.reserve(count+1))return fail("OUT OF MEMORY");
        char prefix[16];
        const int prefix_length=source.source_mode_ == ProgramSourceMode::Structured ? 0 : std::snprintf(
            prefix,sizeof(prefix),"%ld ",static_cast<long>(number));
        if(prefix_length<0||prefix_length>=static_cast<int>(sizeof(prefix)))
            return fail("BAD BASIC LINE");
        const std::uint8_t newline='\n';
        index.lines[count++]={
            number,static_cast<std::uint32_t>(bytes),
            hash_line(number,text,length),
            static_cast<std::uint16_t>(length)
        };
        if(!writer.write(
               reinterpret_cast<const std::uint8_t*>(prefix),
               static_cast<std::size_t>(prefix_length))||
           (length&&!writer.write(
               reinterpret_cast<const std::uint8_t*>(text),length))||
           !writer.write(&newline,1))
            return fail(writer.error());
        bytes+=static_cast<std::size_t>(prefix_length)+length+1;
        return true;
    };
    if (row_edit) {
        for (std::size_t i=0;i<=source.count_;++i) {
            if (i==row_edit->first)
                for(std::size_t j=0;j<row_edit->insert;++j)
                    if(!emit(static_cast<std::int32_t>(count+1),row_edit->rows[j],
                             std::strlen(row_edit->rows[j]))) return false;
            if(i==source.count_) break;
            if(i>=row_edit->first && i<row_edit->first+row_edit->remove) continue;
            std::int32_t id=0; const char* body=nullptr; std::size_t length=0;
            if(!source.read_text_unlocked(i,id,body,length) ||
               !emit(static_cast<std::int32_t>(count+1),body,length)) return false;
        }
    } else for(std::size_t i=0;i<source.count_;++i) {
        std::int32_t number=0;const char* text=nullptr;std::size_t length=0;
        if(!source.read_text_unlocked(i,number,text,length))
            return fail(source.error(),&source==this&&source.suspended());
        bool replaced = false;
        while (edit_index < edit_count && edits[edit_index]->number <= number) {
            const PendingEdit* item = edits[edit_index++];
            if (!removed(item) && !emit(item->number, item->text, item->length))
                return false;
            replaced = replaced || item->number == number;
        }
        if (replaced) continue;
        if(!emit(number,text,length))return false;
    }
    while (edit_index < edit_count) {
        const PendingEdit* item = edits[edit_index++];
        if (!removed(item) && !emit(item->number, item->text, item->length))
            return false;
    }
    if(!writer.commit())return fail(writer.error());
    std::snprintf(index.work,sizeof(index.work),"%s",target);
    if(program_files::reserved(target)&&!verify(index,count)) {
        remove_work(root_,target);return false;
    }
    return true;
}
// Re-read the complete canonical snapshot before making it live. No second
// full index or source copy is needed. Compare offsets, lengths, numbers and
// full-line hashes.
bool ProgramStore::verify(
    const SdProgramStore& index,std::size_t count
) const {
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,index.work);
    FILE* file=std::fopen(path,"rb");
    if(!file)return fail("SD WORK VERIFICATION FAILED");
    std::size_t n=0,offset=0;bool ok=true;
    while(std::fgets(index.scratch,sizeof(index.scratch),file)) {
        const auto raw_length=std::strlen(index.scratch);
        if(raw_length==sizeof(index.scratch)-1&&
           index.scratch[raw_length-1]!='\n'){ok=false;break;}
        char* body=nullptr;std::size_t length=0;std::int32_t number=0;
        bool parsed=false;
        if (index.source_mode==ProgramSourceMode::Structured) {
            body=index.scratch; number=n<count?index.lines[n].number:0;
            length=std::strlen(body);
            while(length && (body[length-1]=='\r'||body[length-1]=='\n'))body[--length]=0;
            parsed=length<kMaxSdProgramLineLength;
        } else parsed=parse_text(index.scratch,number,body,length,true,kMaxSdProgramLineLength);
        if(n>=count||!parsed||
           index.lines[n].offset!=offset||
           index.lines[n].number!=number||
           index.lines[n].length!=length||
           index.lines[n].hash!=hash_line(number,body,length)) {
            ok=false;break;
        }
        offset+=raw_length;++n;
    }
    if(std::ferror(file)||n!=count)ok=false;
    if(std::fclose(file)!=0)ok=false;
    return ok||fail("SD WORK VERIFICATION FAILED");
}
bool ProgramStore::read_session_file(
    const char* name, SessionMetadata& session
) const {
    session=SessionMetadata{};
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,name);
    FILE* file=std::fopen(path,"rb");if(!file)return false;
    char line[192];bool version=false,work=false,filename=false,dirty=false,source_mode=false,ok=true;
    int format=1;
    while(std::fgets(line,sizeof(line),file)) {
        const std::size_t length=std::strlen(line);
        if(length==sizeof(line)-1&&line[length-1]!='\n'){ok=false;break;}
        while(*line&&
              (line[std::strlen(line)-1]=='\r'||line[std::strlen(line)-1]=='\n'))
            line[std::strlen(line)-1]=0;
        char* equals=std::strchr(line,'=');
        if(!equals){ok=false;break;}
        *equals++=0;
        if(program_files::equal(line,"version")) {
            if(version||(std::strcmp(equals,"1")&&std::strcmp(equals,"2"))){ok=false;break;}
            format=*equals-'0'; version=true;
        } else if(program_files::equal(line,"work")) {
            if(work||!work_file_name(equals)){ok=false;break;}
            std::snprintf(session.work,sizeof(session.work),"%s",equals);work=true;
        } else if(program_files::equal(line,"file")) {
            if(filename||(*equals&&
               (!file_paths::valid_relative(equals)||
                program_files::reserved(equals)))){ok=false;break;}
            std::snprintf(session.filename,sizeof(session.filename),"%s",equals);
            filename=true;
        } else if(program_files::equal(line,"dirty")) {
            if(dirty||(std::strcmp(equals,"0")&&std::strcmp(equals,"1")))
                {ok=false;break;}
            session.dirty=*equals=='1';dirty=true;
        } else if(program_files::equal(line,"source_mode")) {
            if(source_mode||(std::strcmp(equals,"0")&&std::strcmp(equals,"1"))){ok=false;break;}
            session.source_mode=*equals=='1'?ProgramSourceMode::Structured:ProgramSourceMode::ClassicNumbered;
            source_mode=true;
        } else {ok=false;break;}
    }
    if(std::ferror(file))ok=false;
    if(std::fclose(file)!=0)ok=false;
    if(format==1)session.source_mode=ProgramSourceMode::ClassicNumbered;
    return ok&&version&&work&&filename&&dirty&&(format==1 || source_mode);
}
bool ProgramStore::write_session(
    const char* work,const char* filename,bool dirty,ProgramSourceMode mode
) {
    if(!work_file_name(work)||!filename)return fail("BAD SESSION STATE");
    char text[256];
    const int length=std::snprintf(
        text,sizeof(text),"version=2\nwork=%s\nfile=%s\ndirty=%d\nsource_mode=%d\n",
        work,filename,dirty?1:0,mode==ProgramSourceMode::Structured?1:0);
    if(length<=0||length>=static_cast<int>(sizeof(text)))
        return fail("BAD SESSION STATE");
    // A completed SafeFileWriter commit may leave its old backup when only
    // backup removal failed. If the installed target parses completely, that
    // staging state is stale and can be removed before the next transaction.
    SessionMetadata installed;
    if(read_session_file("RMBASIC.SES",installed)) {
        char stale[192];
        struct stat value;
        std::snprintf(stale,sizeof(stale),"%sRMBSES.TMP",root_);
        if(stat(stale,&value)==0) std::remove(stale);
        std::snprintf(stale,sizeof(stale),"%sRMBSES.BAK",root_);
        if(stat(stale,&value)==0) std::remove(stale);
    }
    SafeFileWriter writer;
    if(!writer.open("RMBASIC.SES","RMBSES",root_))
        return fail("SESSION WRITE ERROR");
    if(!writer.write(reinterpret_cast<const std::uint8_t*>(text),
                     static_cast<std::size_t>(length))||!writer.commit())
        return fail("SESSION WRITE ERROR");
    return true;
}
bool ProgramStore::preserve_unverified_work_locked() {
    // An unreadable/mismatched session must not erase its last editing source.
    // Retain every work file as an ordinary, loadable recovery BAS before cleanup.
    unsigned serial=0;
    for(;;) {
        DIR* directory=opendir(root_);
        if(!directory)return fail("SESSION PRESERVATION ERROR");
        char name[80]={};
        while(dirent* entry=readdir(directory)) {
            if(work_file_name(entry->d_name)) {
                std::snprintf(name,sizeof(name),"%s",entry->d_name);break;
            }
        }
        closedir(directory);
        if(!*name)return true;
        char source[192],target[192];
        std::snprintf(source,sizeof(source),"%s%s",root_,name);
        bool selected=false;
        for(;serial<10000;++serial) {
            std::snprintf(target,sizeof(target),"%sRECOVER%04u.BAS",root_,serial);
            struct stat value;
            if(stat(target,&value)!=0) {
                if(errno!=ENOENT)return fail("SESSION PRESERVATION ERROR");
                selected=true;++serial;break;
            }
        }
        if(!selected||std::rename(source,target)!=0)
            return fail("SESSION PRESERVATION ERROR");
    }
}
bool ProgramStore::cleanup_orphans_locked(
    const char* active_work,const char* session_work
) {
    DIR* directory=opendir(root_);
    if(!directory)return false;
    while(dirent* entry=readdir(directory)) {
        const char* name=entry->d_name;
        if(!work_file_name(name))continue;
        if((active_work&&program_files::equal(name,active_work))||
           (session_work&&program_files::equal(name,session_work)))continue;
        char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,name);
        std::remove(path); // best effort: never invalidate the protected source
    }
    closedir(directory);
    return true;
}
bool ProgramStore::cleanup_orphans() {
    if(!storage::firmware_owns_card()||!storage::available())
        return fail("SD CARD NOT AVAILABLE");
    Lease lease;if(!lease.locked)return fail(storage::last_error());
    SessionMetadata session;const char* protected_session=nullptr;
    if(read_session_file("RMBASIC.SES",session))
        protected_session=session.work;
    const char* active=sd_?sd_->work:nullptr;
    const bool ok=cleanup_orphans_locked(active,protected_session);
    if(ok)error_="OK";
    return ok;
}
bool ProgramStore::publish_sd(
    SdProgramStore* next,std::size_t count,std::size_t bytes,
    const char* filename,bool dirty
) {
    if(!next||!work_file_name(next->work))
        return fail("BAD SESSION STATE");
    // filename may alias filename_ (normal edit and RAM-to-SD migration).
    // Preserve it before publishing into the same object.
    char published_filename[80] = {};
    std::snprintf(published_filename,sizeof(published_filename),"%s",
                  filename?filename:"");
    // The candidate is complete and verified before the atomic metadata switch.
    // If power fails after RMBASIC.SES commits, the next boot can safely adopt
    // this snapshot even if the volatile pointer was not yet changed.
    if(!write_session(next->work,published_filename,dirty,next->source_mode)) {
        remove_work(root_,next->work);
        return false;
    }
    SdProgramStore* old=sd_;
    sd_=next;source_mode_=next->source_mode;count_=count;bytes_=bytes;backend_=ProgramBackend::Sd;
    invalidate_sd_cache();
    suspended_=false;dirty_=dirty;session_recovered_=false;
    std::snprintf(filename_,sizeof(filename_),"%s",published_filename);
    bump_revision();
    error_="OK";protect();
    if(old){if(!program_files::equal(old->work,sd_->work))
        remove_work(root_,old->work);release_backend(old);}
    cleanup_orphans_locked(sd_->work,sd_->work);
    error_="OK";
    return true;
}
bool ProgramStore::recover_session_locked(ProgramStorageMode mode) {
    auto candidate=[&](const char* metadata,SessionMetadata& session,
                       BackendPtr<SdProgramStore>& index,
                       std::size_t& count,std::size_t& bytes) {
        if(!read_session_file(metadata,session))return false;
        index.reset(allocate_backend<SdProgramStore>());
        if(!index)return false;
        if(!scan(session.work,*index,count,session.source_mode)||
           index->source_mode!=session.source_mode||!verify(*index,count))return false;
        char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,session.work);
        struct stat value;
        if(stat(path,&value)!=0||value.st_size<0)return false;
        bytes=static_cast<std::size_t>(value.st_size);
        return true;
    };
    SessionMetadata session;BackendPtr<SdProgramStore> next;
    std::size_t count=0,bytes=0;
    bool from_backup=false;
    bool valid=candidate("RMBASIC.SES",session,next,count,bytes);
    if(!valid) {
        next.reset();
        valid=candidate("RMBSES.BAK",session,next,count,bytes);
        from_backup=valid;
    }
    char target[192],backup[192],temp[192];
    std::snprintf(target,sizeof(target),"%sRMBASIC.SES",root_);
    std::snprintf(backup,sizeof(backup),"%sRMBSES.BAK",root_);
    std::snprintf(temp,sizeof(temp),"%sRMBSES.TMP",root_);
    if(!valid) {
        std::remove(temp);std::remove(backup);
        return fail("NO VALID SD SESSION");
    }
    if(from_backup) {
        std::remove(target);
        if(std::rename(backup,target)!=0)
            return fail("SESSION RECOVERY ERROR");
    } else {
        std::remove(backup);
    }
    std::remove(temp);
    SdProgramStore* old=sd_;
    sd_=next.release();source_mode_=sd_->source_mode;count_=count;bytes_=bytes;backend_=ProgramBackend::Sd;
    invalidate_sd_cache();
    mode_=mode;suspended_=false;dirty_=session.dirty;session_recovered_=true;
    std::snprintf(filename_,sizeof(filename_),"%s",session.filename);
    release_ram_store();
    if(old)release_backend(old);
    bump_revision();
    error_="OK";protect();
    cleanup_orphans_locked(sd_->work,sd_->work);
    return true;
}
bool ProgramStore::recover_session(
    ProgramStorageMode mode,bool discard_current
) {
    if(dirty_&&!discard_current)
        return fail("PROGRAM MODIFIED - KEEP OR DISCARD BEFORE RESTORE");
    if(!storage::available()||!storage::card_present())
        return fail("SD CARD NOT AVAILABLE");
    Lease lease;if(!lease.locked)return fail(storage::last_error());
    return recover_session_locked(mode);
}
bool ProgramStore::scan(
    const char* name,SdProgramStore& index,std::size_t& count,
    ProgramSourceMode empty_mode
) const {
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,name);
    FILE* f=std::fopen(path,"rb");if(!f)return fail("FILE NOT FOUND");
    bool numbered=false,plain=false,ok=true;
    while(std::fgets(index.scratch,sizeof(index.scratch),f)) {
        const std::size_t n=std::strlen(index.scratch);
        if(n==sizeof(index.scratch)-1 && index.scratch[n-1]!='\n') {
            ok=fail("SD LINE TOO LONG");break;
        }
        const char* p=index.scratch;
        while(*p && std::isspace(static_cast<unsigned char>(*p)))++p;
        if(!*p)continue;
        if(std::isdigit(static_cast<unsigned char>(*p)))numbered=true;
        else plain=true;
        if(numbered&&plain){ok=fail("MIXED SOURCE MODE");break;}
    }
    if(std::ferror(f))ok=fail("SD READ ERROR");
    index.source_mode=numbered?ProgramSourceMode::ClassicNumbered:
                      plain?ProgramSourceMode::Structured:empty_mode;
    std::rewind(f); count=0;std::uint32_t position=0;
    while(ok && std::fgets(index.scratch,sizeof(index.scratch),f)) {
        const auto raw_length=std::strlen(index.scratch);
        const auto offset=position;
        position+=static_cast<std::uint32_t>(raw_length);
        char* body=index.scratch;std::size_t length=raw_length;
        std::int32_t number=static_cast<std::int32_t>(count+1);
        if(index.source_mode==ProgramSourceMode::ClassicNumbered) {
            char* p=body;while(*p&&std::isspace(static_cast<unsigned char>(*p)))++p;
            if(!*p)continue;
            if(!parse_text(body,number,body,length,program_files::reserved(name),
                           kMaxSdProgramLineLength)){ok=fail("BAD BASIC FILE / SD LINE TOO LONG");break;}
        } else {
            while(length&&(body[length-1]=='\r'||body[length-1]=='\n'))body[--length]=0;
            if(length>=kMaxSdProgramLineLength){ok=fail("SD LINE TOO LONG");break;}
        }
        std::size_t pos=0;while(pos<count&&index.lines[pos].number<number)++pos;
        const bool exists=pos<count&&index.lines[pos].number==number;
        if(!exists) {
            if(count==kMaxSdProgramLines){ok=fail("SD PROGRAM FULL");break;}
            if(!index.reserve(count+1)){ok=fail("OUT OF MEMORY");break;}
            for(auto j=count;j>pos;--j)index.lines[j]=index.lines[j-1];
            ++count;
        }
        index.lines[pos]={number,static_cast<std::uint32_t>(offset),
            hash_line(number,body,length),static_cast<std::uint16_t>(length)};
    }
    if(std::ferror(f))ok=fail("SD READ ERROR");
    if(std::fclose(f)!=0)ok=fail("SD READ ERROR");
    if(ok)std::snprintf(index.work,sizeof(index.work),"%s",name);
    return ok;
}
bool ProgramStore::initialize(ProgramStorageMode mode) {
    active_=true;mode_=mode;
    const ProgramBackend target=
        mode==ProgramStorageMode::InternalRam?ProgramBackend::Ram:
        mode==ProgramStorageMode::SdCard?ProgramBackend::Sd:
        (storage::available()&&storage::card_present()
            ?ProgramBackend::Sd:ProgramBackend::Ram);
    if(target==ProgramBackend::Ram) {
        backend_=ProgramBackend::Ram;
        suspended_=false;
        error_="OK";
        if(!ensure_ram_store()) return fail("OUT OF MEMORY");
        return true;
    }
    if(!storage::available()||!storage::card_present()) {
        sd_=allocate_backend<SdProgramStore>();
        if(!sd_)return fail("OUT OF MEMORY");
        backend_=ProgramBackend::Sd;suspended_=true;
        return fail("SD CARD NOT AVAILABLE",true);
    }
    Lease lease;if(!lease.locked)return fail(storage::last_error());
    if(recover_session_locked(mode))return true;

    // Session recovery has been decided before cleanup. Invalid/missing
    // metadata never blocks boot; start an empty verified workspace.
    session_recovered_=false;error_="OK";
    if(!preserve_unverified_work_locked())return false;
    cleanup_orphans_locked(nullptr,nullptr);
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next)return fail("OUT OF MEMORY");
    char work[80];if(!new_work_name(work))return false;
    ProgramStore empty;std::size_t count=0,bytes=0;
    if(!snapshot(work,empty,*next,count,bytes))return false;
    if(!publish_sd(next.get(),count,bytes,"",false))return false;
    next.release();mode_=mode;return true;
}
bool ProgramStore::switch_mode(ProgramStorageMode mode,bool discard) {
    ProgramBackend target=mode==ProgramStorageMode::InternalRam ? ProgramBackend::Ram :
        mode==ProgramStorageMode::SdCard ? ProgramBackend::Sd :
        (storage::available() && storage::card_present() ? ProgramBackend::Sd : ProgramBackend::Ram);
    if(target==backend_ && !discard) {
        if(!ready()) return false;
        mode_=mode;
        return true;
    }
    if(target==backend_ && discard && target==ProgramBackend::Ram) {
        mode_=mode;
        return clear();
    }
    if(backend_==ProgramBackend::Sd && !discard && !ready()) return false;
    if(target==ProgramBackend::Ram) {
        ProgramStore next;
        next.backend_=ProgramBackend::Ram;
        next.mode_=mode;
        next.source_mode_=source_mode_;
        if(!next.ensure_ram_store()) return fail("OUT OF MEMORY");

        if(!discard) {
            for(std::size_t i=0;i<count_;++i) {
                std::int32_t number=0;
                const char* text=nullptr;
                std::size_t length=0;
                if(!read_line_text(i,number,text,length))
                    return false;
                if(!next.set_line(number,text))
                    return fail(next.error());
            }
        }

        // The SD session remains durable so an explicit later restore is possible.
        release_sd_cache();
        release_backend(sd_);sd_=nullptr;
        release_ram_store();
        ram_=next.ram_;next.ram_=nullptr;
        ram_psram_=next.ram_psram_;next.ram_psram_=false;
        ram_psram_base_=next.ram_psram_base_;next.ram_psram_base_=0;
        ram_psram_bytes_=next.ram_psram_bytes_;next.ram_psram_bytes_=0;
        ram_long_scratch_=next.ram_long_scratch_;
        next.ram_long_scratch_=nullptr;
        count_=discard ? 0 : next.count_;
        bytes_=discard ? 0 : next.bytes_;
        if(!ram_psram_) (void)migrate_ram_to_psram();
        if(discard) {filename_[0]=0;dirty_=false;}
        else dirty_=next.dirty_;
        backend_=target;mode_=mode;suspended_=false;
        session_recovered_=false;bump_revision();protect();return true;
    }
    if(!storage::available() || !storage::card_present()) return fail("SD CARD NOT AVAILABLE");
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    char work[80]; if(!new_work_name(work)) return false;
    ProgramStore empty; empty.source_mode_=source_mode_; std::size_t count,bytes;
    if(!snapshot(work,discard?empty:*this,*next,count,bytes)) return false;
    const char* next_filename=discard?"":filename_;
    const bool next_dirty=discard?false:dirty_;
    if(!publish_sd(next.get(),count,bytes,next_filename,next_dirty))return false;
    next.release();
    release_ram_store();
    mode_=mode;protect(); return true;
}
bool ProgramStore::resume() {
    if(backend_==ProgramBackend::Ram) return true;
    if(!storage::card_present()) return fail("SD CARD NOT AVAILABLE",true);
    if(!storage::remount()) return fail(storage::last_error());
    if (!*sd_->work) return switch_mode(mode_,true);
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> check(allocate_backend<SdProgramStore>());
    if(!check) return fail("OUT OF MEMORY");
    std::size_t count;
    if(!scan(sd_->work,*check,count,source_mode_)) return fail("SD WORK FILE MISSING - STORAGE SUSPENDED",true);
    bool changed=count!=count_;
    for(std::size_t i=0;!changed&&i<count;++i) {
        const auto& a=check->lines[i];
        const auto& b=sd_->lines[i];
        changed=a.number!=b.number||a.offset!=b.offset||
                a.length!=b.length||a.hash!=b.hash;
    }
    if(changed)
        return fail("SD SOURCE CHANGED - STORAGE SUSPENDED",true);
    invalidate_sd_cache();
    suspended_=false; error_="OK"; return true;
}
bool ProgramStore::suspend_for_usb() {
    if (backend_==ProgramBackend::Ram) return true;
    if (!ready()) return false;
    if (dirty_) return fail("PROGRAM MODIFIED - SAVE OR DISCARD BEFORE USB STORAGE");
    release_sd_cache();
    suspended_=true;
    error_="PROGRAM STORAGE SUSPENDED - USB HOST OWNS SD CARD";
    if(active_) program_files::set_active(nullptr);
    return true;
}
void ProgramStore::cancel_usb_suspend() {
    if (backend_!=ProgramBackend::Sd) return;
    suspended_=false; error_="OK"; protect();
}
bool ProgramStore::resume_after_usb() {
    if (backend_==ProgramBackend::Ram) return true;
    if (!storage::available() || !storage::card_present())
        return fail("SD CARD NOT AVAILABLE - PROGRAM STORAGE SUSPENDED",true);
    Lease lease; if(!lease.locked) return fail(storage::last_error(),true);
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY",true);
    std::size_t count=0,bytes=0;char work[80];
    if(!new_work_name(work))return false;
    if(*filename_) {
        ProgramStore input;input.root_=root_;input.backend_=ProgramBackend::Sd;
        input.sd_=allocate_backend<SdProgramStore>();
        if(!input.sd_)return fail("OUT OF MEMORY",true);
        if(!scan(filename_,*input.sd_,input.count_,source_mode_))
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
        input.source_mode_=input.sd_->source_mode;
        if(!snapshot(work,input,*next,count,bytes))
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
    } else {
        ProgramStore empty;
        if(!snapshot(work,empty,*next,count,bytes))return false;
    }
    if(!publish_sd(next.get(),count,bytes,filename_,false))return false;
    next.release();dirty_=false;suspended_=false;error_="OK";protect();return true;
}
bool ProgramStore::edit_sd(
    std::int32_t number,const char* text,std::size_t length,bool remove
) {
    if(!ready()) return false;
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    PendingEdit line{number,text,length};
    std::size_t count,bytes;
    char work[80]; if(!new_work_name(work)) return false;
    if(!snapshot(work,*this,*next,count,bytes,&line,remove)) return false;
    if(!publish_sd(next.get(),count,bytes,filename_,true))return false;
    next.release();return true;
}

bool ProgramStore::replace_line_pair(
    std::int32_t first, const char* first_text,
    std::int32_t second, const char* second_text
) {
    if (first < 0 || second < 0 || first == second) return fail("BAD LINE PAIR");
    if (!ready()) return false;
    if (backend_ == ProgramBackend::Ram && !ensure_ram_store())
        return fail("OUT OF MEMORY");
    const char* supplied[2] = {first_text, second_text};
    const std::int32_t numbers[2] = {first, second};
    std::size_t lengths[2] = {};
    for (int j = 0; j < 2; ++j) {
        if (!supplied[j]) continue;
        lengths[j] = std::strlen(supplied[j]);
        if (lengths[j] > line_length_capacity() ||
            std::strpbrk(supplied[j], "\r\n")) return fail("LINE TOO LONG OR INVALID");
    }
    // Copy caller text before any borrowed ProgramStore scratch is reused.
    struct Texts {
        char next[2][kMaxSdProgramLineLength] = {};
        char old[2][kMaxSdProgramLineLength] = {};
    };
    BackendPtr<Texts> text(allocate_backend<Texts>());
    if (!text) return fail("OUT OF MEMORY");
    for (int j = 0; j < 2; ++j)
        if (supplied[j]) std::memcpy(text->next[j], supplied[j], lengths[j] + 1u);

    bool exists[2] = {};
    std::size_t next_count = count_;
    for (std::size_t i = 0; i < count_; ++i) {
        std::int32_t n = 0;
        std::size_t length = 0;
        if (!read_line_metadata(i, n, length)) return false;
        for (int j = 0; j < 2; ++j) if (n == numbers[j]) {
            exists[j] = true;
            --next_count;
        }
    }
    for (int j = 0; j < 2; ++j) if (supplied[j]) ++next_count;
    if (next_count > line_capacity()) return fail("PROGRAM FULL - EDIT NOT APPLIED");

    if (backend_ == ProgramBackend::Sd) {
        Lease lease;
        if (!lease.locked) return fail(storage::last_error());
        BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
        if (!next) return fail("OUT OF MEMORY");
        PendingEdit a{first, supplied[0] ? text->next[0] : nullptr, lengths[0]};
        PendingEdit b{second, supplied[1] ? text->next[1] : nullptr, lengths[1]};
        std::size_t count = 0, bytes = 0;
        char work[80];
        if (!new_work_name(work) || !snapshot(work, *this, *next, count, bytes,
                                             &a, false, &b)) return false;
        if (!publish_sd(next.get(), count, bytes, filename_, true)) return false;
        next.release();
        return true;
    }

    if (!ram_psram_) {
        // No allocation or I/O after preflight: compact in place, then insert.
        std::size_t kept = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (ram_->lines[i].number != first && ram_->lines[i].number != second)
                ram_->lines[kept++] = ram_->lines[i];
        }
        for (int j = 0; j < 2; ++j) if (supplied[j]) {
            std::size_t at = 0;
            while (at < kept && ram_->lines[at].number < numbers[j]) ++at;
            std::memmove(ram_->lines + at + 1, ram_->lines + at,
                         (kept - at) * sizeof(ProgramLine));
            ram_->lines[at].number = numbers[j];
            std::memcpy(ram_->lines[at].text, text->next[j], lengths[j] + 1u);
            ++kept;
        }
        count_ = kept;
        bytes_ = 0;
        for (std::size_t i = 0; i < count_; ++i) bytes_ += line_bytes(ram_->lines[i]);
    } else {
        // The source remains compact: copy only indexes and the two edited
        // bodies, never the 2 MiB program. Retain before-images for rollback.
        const std::size_t capacity = std::max<std::size_t>(1, std::max(count_, next_count));
        auto* old = static_cast<PsramProgramIndexEntry*>(
            std::malloc(capacity * 2u * sizeof(PsramProgramIndexEntry)));
        if (!old) return fail("OUT OF MEMORY");
        auto* next = old + capacity;
        if (count_ && !psram::read(ram_psram_base_, old,
                                 count_ * sizeof(PsramProgramIndexEntry))) {
            std::free(old);
            return fail("PSRAM READ ERROR");
        }
        std::uint16_t old_slot[2] = {};
        std::size_t old_length[2] = {};
        bool used[kMaxPsramProgramLines] = {};
        std::size_t kept = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& entry = old[i];
            if (entry.slot >= kMaxPsramProgramLines ||
                entry.length >= kMaxPsramProgramLineLength) {
                std::free(old);
                return fail("PSRAM INDEX ERROR");
            }
            int changed = entry.number == first ? 0 : entry.number == second ? 1 : -1;
            if (changed >= 0) {
                const char* original = nullptr;
                if (!psram_read_text(entry, original)) {
                    std::free(old);
                    return fail("PSRAM READ ERROR");
                }
                std::memcpy(text->old[changed], original, entry.length + 1u);
                old_slot[changed] = entry.slot;
                old_length[changed] = entry.length;
            } else {
                used[entry.slot] = true;
                next[kept++] = entry;
            }
        }
        bool ok = true;
        for (int j = 0; j < 2 && ok; ++j) if (supplied[j]) {
            std::size_t slot = 0;
            while (slot < kMaxPsramProgramLines && used[slot]) ++slot;
            if (slot == kMaxPsramProgramLines) { ok = false; break; }
            used[slot] = true;
            ok = psram_write_text(static_cast<std::uint16_t>(slot),
                                  text->next[j], lengths[j]);
            next[kept++] = {numbers[j], hash_line(numbers[j], text->next[j], lengths[j]),
                static_cast<std::uint16_t>(slot), static_cast<std::uint16_t>(lengths[j])};
        }
        if (ok) {
            std::sort(next, next + kept, [](const auto& a, const auto& b) {
                return a.number < b.number;
            });
            if (kept) ok = psram::write(ram_psram_base_, next,
                                       kept * sizeof(PsramProgramIndexEntry));
        }
        if (!ok) {
            bool restored = true;
            for (int j = 0; j < 2; ++j) if (exists[j]) {
                const bool written = psram_write_text(old_slot[j], text->old[j], old_length[j]);
                restored = written && restored;
            }
            if (count_) {
                const bool written = psram::write(ram_psram_base_, old,
                                                  count_ * sizeof(PsramProgramIndexEntry));
                restored = written && restored;
            }
            std::free(old);
            return fail(restored ? "PSRAM EDIT FAILED - ORIGINAL RESTORED"
                                 : "PSRAM RECOVERY REQUIRED", !restored);
        }
        bytes_ = 0;
        for (std::size_t i = 0; i < kept; ++i)
            bytes_ += line_bytes(next[i].number, nullptr, next[i].length);
        count_ = kept;
        std::free(old);
    }
    dirty_ = true;
    bump_revision();
    error_ = "OK";
    return true;
}

bool ProgramStore::set_line(std::int32_t number,const char* text) {
    if(!text)return fail("BAD LINE");
    const std::size_t length=std::strlen(text);
    if(backend_==ProgramBackend::Sd) {
        if(length>=kMaxSdProgramLineLength)return fail("SD LINE TOO LONG");
        return edit_sd(number,text,length,false);
    }

    if(!ensure_ram_store()) return fail("OUT OF MEMORY");
    if(ram_psram_) return psram_set_line(number,text,length);

    if(length>=kMaxProgramLineLength)
        return fail("LINE TOO LONG FOR SRAM FALLBACK");

    ProgramLine current;
    std::size_t pos=0;
    while(pos<count_) {
        if(!ram_read_record(pos,current))
            return fail("RAM PROGRAM STORAGE ERROR");
        if(current.number>=number) break;
        ++pos;
    }
    const bool exists=pos<count_&&current.number==number;
    if(!exists) {
        if(count_==kMaxRamProgramLines)
            return fail("PROGRAM TOO LARGE FOR SRAM FALLBACK");
        for(std::size_t j=count_;j>pos;--j) {
            ProgramLine moved;
            if(!ram_read_record(j-1,moved) ||
               !ram_write_record(j,moved))
                return fail("RAM PROGRAM STORAGE ERROR");
        }
        ++count_;
    } else {
        bytes_-=line_bytes(current);
    }

    ProgramLine line;
    line.number=number;
    std::strncpy(line.text,text,sizeof(line.text)-1);
    line.text[sizeof(line.text)-1]=0;
    if(!ram_write_record(pos,line))
        return fail("RAM PROGRAM STORAGE ERROR");
    bytes_+=line_bytes(line);
    dirty_=true;
    bump_revision();
    error_="OK";
    return true;
}

bool ProgramStore::erase_line(std::int32_t number) {
    if(backend_==ProgramBackend::Sd) return edit_sd(number,nullptr,0,true);
    if(count_==0) return true;
    if(!ensure_ram_store()) return fail("OUT OF MEMORY");
    if(ram_psram_) return psram_erase_line(number);

    ProgramLine current;
    std::size_t pos=0;
    while(pos<count_) {
        if(!ram_read_record(pos,current))
            return fail("RAM PROGRAM STORAGE ERROR");
        if(current.number>=number) break;
        ++pos;
    }
    if(pos==count_||current.number!=number) return true;
    bytes_-=line_bytes(current);
    for(std::size_t j=pos;j+1<count_;++j) {
        ProgramLine moved;
        if(!ram_read_record(j+1,moved) ||
           !ram_write_record(j,moved))
            return fail("RAM PROGRAM STORAGE ERROR");
    }
    --count_;
    dirty_=true;
    bump_revision();
    error_="OK";
    return true;
}
bool ProgramStore::clear() {
    if(!ready()) return false;
    if(backend_==ProgramBackend::Sd) {
        Lease lease; if(!lease.locked) return fail(storage::last_error());
        char work[80]; if(!new_work_name(work)) return false;
        ProgramStore empty; empty.source_mode_=source_mode_; std::size_t count,bytes;
        BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
        if(!next) return fail("OUT OF MEMORY");
        if(!snapshot(work,empty,*next,count,bytes)) return false;
        if(!publish_sd(next.get(),count,bytes,"",false))return false;
        next.release();
    } else {
        // Keep the selected INTERNAL backend allocated. On PSRAM systems this
        // preserves the 1024 x 2047 capability after NEW/CLEAR without
        // re-probing or briefly falling back to the legacy SRAM limits.
        count_=bytes_=0;
        filename_[0]=0;dirty_=false;session_recovered_=false;
        if(ram_psram_)
            (void)psram::fill(
                ram_psram_base_,0,kPsramProgramIndexBytes);
        else if(ram_)
            std::memset(ram_,0,sizeof(RamProgramStore));
        bump_revision();
    }
    protect();return true;
}
bool ProgramStore::load(const char* name) {
    if(!ready()) return false;
    char filename[80];if(!normalize(name,filename)) return false;
    Lease lease;if(!lease.locked) return fail(storage::last_error());
    std::size_t bytes=0;
    if(backend_==ProgramBackend::Ram) {
        // If PSRAM became available since the fallback backend was selected,
        // upgrade before evaluating the incoming source limits.
        if(!ram_psram_) (void)migrate_ram_to_psram();

        BackendPtr<SdProgramStore> source(
            allocate_backend<SdProgramStore>());
        if(!source) return fail("OUT OF MEMORY");
        std::size_t source_count=0;
        if(!scan(filename,*source,source_count,source_mode_)) return false;

        if(!ram_psram_) {
            if(source_count>kMaxRamProgramLines)
                return fail("PROGRAM TOO LARGE FOR SRAM FALLBACK");
            for(std::size_t i=0;i<source_count;++i) {
                if(source->lines[i].length>=kMaxProgramLineLength)
                    return fail("LINE TOO LONG FOR SRAM FALLBACK");
            }
        }

        char path[192];
        std::snprintf(path,sizeof(path),"%s%s",root_,filename);
        FILE* file=std::fopen(path,"rb");
        if(!file) return fail("FILE NOT FOUND");

        char* scratch=static_cast<char*>(
            std::malloc(kSdProgramSourceBufferLength));
        PsramProgramIndexEntry* entries=nullptr;
        if(ram_psram_ && source_count!=0) {
            entries=static_cast<PsramProgramIndexEntry*>(
                std::malloc(
                    source_count*sizeof(PsramProgramIndexEntry)));
        }
        if(!scratch || (ram_psram_ && source_count!=0 && !entries)) {
            std::free(scratch);
            std::free(entries);
            std::fclose(file);
            return fail("OUT OF MEMORY");
        }

        bool ok=true;
        std::size_t loaded_bytes=0;
        if(!ram_psram_ && ram_)
            std::memset(ram_,0,sizeof(RamProgramStore));

        for(std::size_t i=0;i<source_count;++i) {
            std::int32_t number=0;
            const char* body=nullptr;
            std::size_t length=0;
            if(!read_sd_entry(
                    file,
                    source->lines[i],
                    scratch,
                    kSdProgramSourceBufferLength,
                    number,
                    body,
                    length,
                    false,source->source_mode)) {
                ok=false;
                break;
            }

            if(ram_psram_) {
                if(!psram::write(
                        psram_text_base()+
                            static_cast<std::uint32_t>(
                                i*kMaxPsramProgramLineLength),
                        body,
                        length+1u)) {
                    ok=false;
                    break;
                }
                entries[i].number=number;
                entries[i].hash=hash_line(number,body,length);
                entries[i].slot=static_cast<std::uint16_t>(i);
                entries[i].length=static_cast<std::uint16_t>(length);
            } else {
                ram_->lines[i].number=number;
                std::memcpy(
                    ram_->lines[i].text,
                    body,
                    length+1u);
            }
            loaded_bytes+=line_bytes(number,body,length);
        }

        if(std::ferror(file)) ok=false;
        if(std::fclose(file)!=0) ok=false;

        if(ok && ram_psram_) {
            if(source_count==0) {
                ok=psram::fill(
                    ram_psram_base_,0,kPsramProgramIndexBytes);
            } else {
                ok=psram::write(
                    ram_psram_base_,
                    entries,
                    source_count*sizeof(PsramProgramIndexEntry));
            }
        }

        std::free(entries);
        std::free(scratch);
        if(!ok) return fail(
            ram_psram_ ? "PSRAM LOAD ERROR" : "SD READ ERROR");

        source_mode_=source->source_mode;
        count_=source_count;
        bytes_=loaded_bytes;
        std::snprintf(filename_,sizeof(filename_),"%s",filename);
        dirty_=false;
        session_recovered_=false;
        bump_revision();
        protect();
        error_="OK";
        return true;
    }
    ProgramStore input; input.root_=root_;input.backend_=ProgramBackend::Sd;
    input.sd_=allocate_backend<SdProgramStore>();if(!input.sd_) return fail("OUT OF MEMORY");
    if(!scan(filename,*input.sd_,input.count_,source_mode_)) return false;
    input.source_mode_=input.sd_->source_mode;
    char work[80];if(!new_work_name(work)) return false;
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    std::size_t count;
    if(!snapshot(work,input,*next,count,bytes)) return false;
    if(!publish_sd(next.get(),count,bytes,filename,false))return false;
    next.release();protect();return true;
}
bool ProgramStore::note_source_renamed(const char* old_name, const char* new_name) {
    if (!old_name || !new_name || !*filename_ ||
        !file_paths::same_or_child(filename_, old_name)) return true;
    char normalized[80] = {};
    if (!file_paths::relocate(filename_, old_name, new_name, normalized, sizeof(normalized)))
        return fail("RENAMED SOURCE PATH TOO LONG");
    if (backend_ == ProgramBackend::Sd) {
        if (!ready() || !sd_ || !work_file_name(sd_->work)) return fail("BAD SESSION STATE");
        Lease lease;
        if (!lease.locked) return fail(storage::last_error());
        if (!write_session(sd_->work, normalized, dirty_,source_mode_)) return false;
    }
    std::memcpy(filename_, normalized, std::strlen(normalized) + 1u);
    protect();
    error_ = "OK";
    return true;
}

bool ProgramStore::save(const char* name) {
    if(!ready()) return false;
    char filename[80];if(!normalize(name,filename)) return false;
    Lease lease;if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> index(allocate_backend<SdProgramStore>());
    if(!index) return fail("OUT OF MEMORY");
    std::size_t count,bytes;
    if(!snapshot(filename,*this,*index,count,bytes)) return false;
    if(backend_==ProgramBackend::Sd &&
       !write_session(sd_->work,filename,false,source_mode_))return false;
    std::snprintf(filename_,sizeof(filename_),"%s",filename);
    dirty_=false;session_recovered_=false;protect();return true;
}
#include "program_source_rows.inc"
} // namespace rmb
