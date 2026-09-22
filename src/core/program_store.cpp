#include "program_store.hpp"
#include "program_file_guard.hpp"
#include "safe_file.hpp"
#include "storage.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <new>
#include <memory>
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
std::uint32_t hash_line(const ProgramLine& line) {
    std::uint32_t h=2166136261u ^ static_cast<std::uint32_t>(line.number);
    for (const unsigned char* p=reinterpret_cast<const unsigned char*>(line.text); *p; ++p) h=(h ^ *p)*16777619u;
    return h;
}
bool parse(char* text, ProgramLine& line,bool canonical=false) {
    char* p=text; while (*p==' ' || *p=='\t') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    std::int64_t n=0;
    while (std::isdigit(static_cast<unsigned char>(*p))) { n=n*10+(*p++-'0'); if(n>INT32_MAX) return false; }
    if(canonical) { if(*p==' ') ++p; }
    else while (*p==' ' || *p=='\t') ++p;
    std::size_t len=std::strlen(p);
    while(len && (p[len-1]=='\r' || p[len-1]=='\n')) p[--len]=0;
    if(len>=kMaxProgramLineLength) return false;
    line.number=static_cast<std::int32_t>(n); std::strcpy(line.text,p); return true;
}
std::size_t line_bytes(const ProgramLine& line) {
    char n[16]; return std::snprintf(n,sizeof(n),"%ld ",static_cast<long>(line.number))+std::strlen(line.text)+1;
}
void remove_work(const char* root,const char* name) {
    if(!*name || !program_files::reserved(name)) return;
    char path[192]; std::snprintf(path,sizeof(path),"%s%s",root,name); std::remove(path);
}
}
ProgramStore::~ProgramStore() { release_backend(ram_); release_backend(sd_); if(active_) program_files::set_active(nullptr); }
bool ProgramStore::fail(const char* error,bool suspend) const { error_=error; if(suspend) suspended_=true; return false; }
const char* ProgramStore::mode_name(ProgramStorageMode mode) {
    return mode==ProgramStorageMode::Auto ? "AUTO" : mode==ProgramStorageMode::SdCard ? "SD CARD" : "INTERNAL RAM";
}
void ProgramStore::protect() { if(active_) program_files::set_active(backend_==ProgramBackend::Sd ? filename_ : nullptr); }
bool ProgramStore::normalize(const char* input,char* output) const {
    if(!SafeFileWriter::valid_root_name(input) || program_files::reserved(input)) return fail("BAD FILENAME");
    std::size_t len=std::strlen(input);
    bool bas=len>=4 && program_files::equal(input+len-4,".BAS");
    if(len+(bas?0:4)>=80) return fail("FILENAME TOO LONG");
    std::snprintf(output,80,"%s%s",input,bas?"":".BAS"); return true;
}
bool ProgramStore::ready() const {
    if(backend_==ProgramBackend::Ram) return true;
    if(suspended_) return fail("PROGRAM STORAGE SUSPENDED - RESUME IN MENU");
    if(!storage::available() || !storage::card_present()) return fail("SD CARD REMOVED - PROGRAM STORAGE SUSPENDED",true);
    return true;
}
bool ProgramStore::read_unlocked(std::size_t i,ProgramLine& out) const {
    if(i>=count_) return fail("BAD LINE INDEX");
    if(backend_==ProgramBackend::Ram) { out=ram_->lines[i]; return true; }
    char path[192],text[256]; std::snprintf(path,sizeof(path),"%s%s",root_,sd_->work);
    FILE* f=std::fopen(path,"rb"); if(!f) return fail("SD READ ERROR - PROGRAM STORAGE SUSPENDED",true);
    bool ok=std::fseek(f,sd_->lines[i].offset,SEEK_SET)==0 && std::fgets(text,sizeof(text),f) &&
        parse(text,out,program_files::reserved(sd_->work));
    if(std::fclose(f)!=0) ok=false;
    if(!ok || out.number!=sd_->lines[i].number || hash_line(out)!=sd_->lines[i].hash)
        return fail("SD SOURCE CHANGED - PROGRAM STORAGE SUSPENDED",true);
    return true;
}
bool ProgramStore::read_line(std::size_t i,ProgramLine& out) const {
    if(!ready()) return false;
    if(backend_==ProgramBackend::Ram) return read_unlocked(i,out);
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    return read_unlocked(i,out);
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
// Produces canonical text with a small line buffer. Index becomes live only
// after SafeFileWriter has flushed, closed and committed the entire file.
bool ProgramStore::snapshot(const char* target,const ProgramStore& source,SdProgramStore& index,
                           std::size_t& count,std::size_t& bytes,const ProgramLine* edit,bool remove) {
    SafeFileWriter writer;
    if(!writer.open(target,"RMBEDIT",root_)) return fail(writer.error());
    count=0; bytes=0; bool inserted=false;
    auto emit=[&](const ProgramLine& line) {
        if(count>=kMaxSdProgramLines) return fail("SD PROGRAM FULL");
        if(!index.reserve(count+1)) return fail("OUT OF MEMORY");
        char text[208]; int n=std::snprintf(text,sizeof(text),"%ld %s\n",static_cast<long>(line.number),line.text);
        index.lines[count++]={line.number,static_cast<std::uint32_t>(bytes),hash_line(line)};
        if(!writer.write(reinterpret_cast<const std::uint8_t*>(text),n)) return fail(writer.error());
        bytes+=n; return true;
    };
    for(std::size_t i=0;i<source.count_;++i) {
        ProgramLine line;
        if(!source.read_unlocked(i,line)) return fail(source.error(),&source==this && source.suspended());
        if(edit && !inserted && edit->number<=line.number) { if(!remove && !emit(*edit)) return false; inserted=true; }
        if(edit && edit->number==line.number) continue;
        if(!emit(line)) return false;
    }
    if(edit && !inserted && !remove && !emit(*edit)) return false;
    if(!writer.commit()) return fail(writer.error());
    std::snprintf(index.work,sizeof(index.work),"%s",target);
    if(program_files::reserved(target) && !verify(index,count)) {
        remove_work(root_,target);
        return false;
    }
    return true;
}
// Re-read the complete canonical snapshot before making it live. No second
// full index or source copy is needed. Compare offsets, numbers and hashes.
bool ProgramStore::verify(const SdProgramStore& index,std::size_t count) const {
    char path[192],text[256]; std::snprintf(path,sizeof(path),"%s%s",root_,index.work);
    FILE* file=std::fopen(path,"rb"); if(!file) return fail("SD WORK VERIFICATION FAILED");
    std::size_t n=0,offset=0; bool ok=true;
    while(std::fgets(text,sizeof(text),file)) {
        const auto length=std::strlen(text); ProgramLine line;
        if(n>=count || !parse(text,line,true) || index.lines[n].offset!=offset ||
           index.lines[n].number!=line.number || index.lines[n].hash!=hash_line(line)) {ok=false;break;}
        offset+=length; ++n;
    }
    if(std::ferror(file) || n!=count) ok=false;
    if(std::fclose(file)!=0) ok=false;
    return ok || fail("SD WORK VERIFICATION FAILED");
}
void ProgramStore::commit_sd(SdProgramStore* next,std::size_t count,std::size_t bytes) {
    SdProgramStore* old=sd_;
    sd_=next; count_=count; bytes_=bytes; suspended_=false; error_="OK";
    // Old file lifetime ends only AFTER new source and index are both live.
    // Cleanup failure must not invalidate the new, already verified program.
    if(old) {remove_work(root_,old->work);release_backend(old);}
}
bool ProgramStore::scan(const char* name,SdProgramStore& index,std::size_t& count) const {
    char path[192],text[256]; std::snprintf(path,sizeof(path),"%s%s",root_,name);
    FILE* f=std::fopen(path,"rb"); if(!f) return fail("FILE NOT FOUND");
    count=0; bool ok=true; std::uint32_t position=0;
    while(true) {
        // pico-vfs overrides _ftello_r with the underlying device position,
        // ignoring unread stdio lookahead. Count consumed binary bytes instead.
        const auto offset=position;
        if(!std::fgets(text,sizeof(text),f)) break;
        auto len=std::strlen(text);
        position+=len;
        if(len==sizeof(text)-1 && text[len-1]!='\n') {ok=fail("SOURCE LINE TOO LONG");break;}
        char* p=text; while(*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if(!*p) continue;
        ProgramLine line;
        if(!parse(p,line,program_files::reserved(name))) {ok=fail("BAD BASIC FILE / LINE TOO LONG");break;}
        std::size_t pos=0; while(pos<count && index.lines[pos].number<line.number) ++pos;
        bool exists=pos<count && index.lines[pos].number==line.number;
        if(!*line.text) {
            if(exists) {for(auto j=pos;j+1<count;++j) index.lines[j]=index.lines[j+1];--count;}
        } else {
            if(!exists) {
                if(count==kMaxSdProgramLines) {ok=fail("SD PROGRAM FULL");break;}
                if(!index.reserve(count+1)) {ok=fail("OUT OF MEMORY");break;}
                for(auto j=count;j>pos;--j) index.lines[j]=index.lines[j-1]; ++count;
            }
            index.lines[pos]={line.number,static_cast<std::uint32_t>(offset),hash_line(line)};
        }
    }
    if(std::ferror(f)) ok=fail("SD READ ERROR");
    if(std::fclose(f)!=0) ok=fail("SD READ ERROR");
    if(ok) std::snprintf(index.work,sizeof(index.work),"%s",name);
    return ok;
}
bool ProgramStore::initialize(ProgramStorageMode mode) {
    active_=true;
    // AUTO resolves once here; hotplug never migrates source.
    if (switch_mode(mode)) return true;
    if (mode==ProgramStorageMode::SdCard ||
        (mode==ProgramStorageMode::Auto && storage::available() && storage::card_present())) {
        sd_=allocate_backend<SdProgramStore>();
        if (!sd_) return fail("OUT OF MEMORY");
        backend_=ProgramBackend::Sd; mode_=mode; suspended_=true;
    }
    return false;
}
bool ProgramStore::switch_mode(ProgramStorageMode mode,bool discard) {
    ProgramBackend target=mode==ProgramStorageMode::InternalRam ? ProgramBackend::Ram :
        mode==ProgramStorageMode::SdCard ? ProgramBackend::Sd :
        (storage::available() && storage::card_present() ? ProgramBackend::Sd : ProgramBackend::Ram);
    if(target==backend_ && !discard) { if(!ready()) return false; mode_=mode; return true; }
    if(backend_==ProgramBackend::Sd && !discard && !ready()) return false;
    if(target==ProgramBackend::Ram) {
        if(!discard && count_>kMaxRamProgramLines) return fail("PROGRAM TOO LARGE FOR RAM MODE");
        BackendPtr<RamProgramStore> next(allocate_backend<RamProgramStore>());
        if(!next) return fail("OUT OF MEMORY");
        std::size_t bytes=0;
        if(!discard) {
            for(std::size_t i=0;i<count_;++i) {
                if(!read_line(i,next->lines[i])) return false;
                bytes+=line_bytes(next->lines[i]);
            }
        }
        // Keep old SD workspace recoverable on explicit discard/removal.
        release_backend(sd_); sd_=nullptr; release_backend(ram_); ram_=next.release();
        if(discard) {count_=0;filename_[0]=0;dirty_=false;}
        bytes_=bytes; backend_=target; mode_=mode; suspended_=false; protect(); return true;
    }
    if(!storage::available() || !storage::card_present()) return fail("SD CARD NOT AVAILABLE");
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    char work[80]; if(!new_work_name(work)) return false;
    ProgramStore empty; std::size_t count,bytes;
    if(!snapshot(work,discard?empty:*this,*next,count,bytes)) return false;
    commit_sd(next.release(),count,bytes);
    release_backend(ram_); ram_=nullptr;
    backend_=target; mode_=mode;
    if(discard) {filename_[0]=0;dirty_=false;}
    protect(); return true;
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
    if(!scan(sd_->work,*check,count)) return fail("SD WORK FILE MISSING - STORAGE SUSPENDED",true);
    if(count!=count_ || (count && std::memcmp(check->lines,sd_->lines,count*sizeof(SdProgramStore::Entry))))
        return fail("SD SOURCE CHANGED - STORAGE SUSPENDED",true);
    suspended_=false; error_="OK"; return true;
}
bool ProgramStore::suspend_for_usb() {
    if (backend_==ProgramBackend::Ram) return true;
    if (!ready()) return false;
    if (dirty_) return fail("PROGRAM MODIFIED - SAVE OR DISCARD BEFORE USB STORAGE");
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
    std::size_t count=0,bytes=0;
    if(*filename_) {
        if(!scan(filename_,*next,count))
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
        char path[192]; std::snprintf(path,sizeof(path),"%s%s",root_,filename_);
        struct stat value;
        if(stat(path,&value)!=0 || value.st_size<0)
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
        bytes=static_cast<std::size_t>(value.st_size);
    } else {
        char work[80]; if(!new_work_name(work)) return false;
        ProgramStore empty;
        if(!snapshot(work,empty,*next,count,bytes)) return false;
    }
    // The pre-USB index is never re-enabled. Install only the new scan after
    // its complete parse succeeds, then retire the old internal work file.
    commit_sd(next.release(),count,bytes);
    dirty_=false; suspended_=false; error_="OK"; protect(); return true;
}
bool ProgramStore::edit_sd(std::int32_t number,const char* text,bool remove) {
    if(!ready()) return false;
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    ProgramLine line; line.number=number; if(text) std::strncpy(line.text,text,sizeof(line.text)-1);
    std::size_t count,bytes;
    char work[80]; if(!new_work_name(work)) return false;
    if(!snapshot(work,*this,*next,count,bytes,&line,remove)) return false;
    commit_sd(next.release(),count,bytes); dirty_=true;return true;
}
bool ProgramStore::set_line(std::int32_t number,const char* text) {
    if(!text) return fail("BAD LINE");
    if(backend_==ProgramBackend::Sd) return edit_sd(number,text,false);
    if(!ram_) {ram_=allocate_backend<RamProgramStore>(); if(!ram_) return fail("OUT OF MEMORY");}
    std::size_t pos=0;while(pos<count_ && ram_->lines[pos].number<number) ++pos;
    bool exists=pos<count_ && ram_->lines[pos].number==number;
    if(!exists) {
        if(count_==kMaxRamProgramLines) return fail("PROGRAM TOO LARGE FOR RAM MODE");
        for(auto j=count_;j>pos;--j) ram_->lines[j]=ram_->lines[j-1]; ++count_;
    } else bytes_-=line_bytes(ram_->lines[pos]);
    auto& line=ram_->lines[pos]; line.number=number;std::strncpy(line.text,text,sizeof(line.text)-1);line.text[sizeof(line.text)-1]=0;
    bytes_+=line_bytes(line);dirty_=true;return true;
}
bool ProgramStore::erase_line(std::int32_t number) {
    if(backend_==ProgramBackend::Sd) return edit_sd(number,nullptr,true);
    std::size_t pos=0;while(pos<count_ && ram_->lines[pos].number<number) ++pos;
    if(pos==count_ || ram_->lines[pos].number!=number) return true;
    bytes_-=line_bytes(ram_->lines[pos]);
    for(auto j=pos;j+1<count_;++j) ram_->lines[j]=ram_->lines[j+1];--count_;dirty_=true;return true;
}
bool ProgramStore::clear() {
    if(!ready()) return false;
    if(backend_==ProgramBackend::Sd) {
        Lease lease; if(!lease.locked) return fail(storage::last_error());
        char work[80]; if(!new_work_name(work)) return false;
        ProgramStore empty; std::size_t count,bytes;
        BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
        if(!next) return fail("OUT OF MEMORY");
        if(!snapshot(work,empty,*next,count,bytes)) return false;
        commit_sd(next.release(),count,bytes);
    } else {release_backend(ram_);ram_=nullptr;}
    count_=bytes_=0;filename_[0]=0;dirty_=false;protect();return true;
}
bool ProgramStore::load(const char* name) {
    if(!ready()) return false;
    char filename[80];if(!normalize(name,filename)) return false;
    Lease lease;if(!lease.locked) return fail(storage::last_error());
    std::size_t bytes=0;
    if(backend_==ProgramBackend::Ram) {
        // RAM rollback needs one temporary RAM store, as in v0.7, but no SD
        // index or full-file cache alongside it. Parse sequentially once.
        ProgramStore next;
        char path[192],text[256]; std::snprintf(path,sizeof(path),"%s%s",root_,filename);
        FILE* file=std::fopen(path,"rb"); if(!file) return fail("FILE NOT FOUND");
        bool ok=true;
        while(std::fgets(text,sizeof(text),file)) {
            auto length=std::strlen(text);
            if(length==sizeof(text)-1 && text[length-1]!='\n') {ok=fail("SOURCE LINE TOO LONG");break;}
            char* body=text; while(*body && std::isspace(static_cast<unsigned char>(*body))) ++body;
            if(!*body) continue;
            ProgramLine line;
            if(!parse(body,line)) {ok=fail("BAD BASIC FILE / LINE TOO LONG");break;}
            if(!(*line.text ? next.set_line(line.number,line.text) : next.erase_line(line.number))) {
                ok=fail(next.error());break;
            }
        }
        if(std::ferror(file)) ok=fail("SD READ ERROR");
        if(std::fclose(file)!=0) ok=fail("SD READ ERROR");
        if(!ok) return false;
        release_backend(ram_);ram_=next.ram_;next.ram_=nullptr;count_=next.count_;bytes=next.bytes_;
    } else {
        ProgramStore input; input.root_=root_;input.backend_=ProgramBackend::Sd;
        input.sd_=allocate_backend<SdProgramStore>();if(!input.sd_) return fail("OUT OF MEMORY");
        if(!scan(filename,*input.sd_,input.count_)) return false;
        char work[80];if(!new_work_name(work)) return false;
        BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
        if(!next) return fail("OUT OF MEMORY");
        std::size_t count;
        if(!snapshot(work,input,*next,count,bytes)) return false;
        commit_sd(next.release(),count,bytes);
    }
    bytes_=bytes;std::strcpy(filename_,filename);dirty_=false;protect();return true;
}
bool ProgramStore::save(const char* name) {
    if(!ready()) return false;
    char filename[80];if(!normalize(name,filename)) return false;
    Lease lease;if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> index(allocate_backend<SdProgramStore>());
    if(!index) return fail("OUT OF MEMORY");
    std::size_t count,bytes;
    if(!snapshot(filename,*this,*index,count,bytes)) return false;
    std::strcpy(filename_,filename);dirty_=false;protect();return true;
}
} // namespace rmb
