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
std::uint32_t hash_line(const ProgramLine& line) {
    return hash_line(line.number,line.text,std::strlen(line.text));
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
    else while (*p==' ' || *p=='\t') ++p;
    length=std::strlen(p);
    while(length && (p[length-1]=='\r' || p[length-1]=='\n'))
        p[--length]=0;
    if(length>=capacity) return false;
    number=static_cast<std::int32_t>(n);body=p;return true;
}
bool parse(char* text,ProgramLine& line,bool canonical=false) {
    char* body=nullptr;std::size_t length=0;std::int32_t number=0;
    if(!parse_text(text,number,body,length,canonical,kMaxProgramLineLength))
        return false;
    line.number=number;std::memcpy(line.text,body,length+1);return true;
}
std::size_t line_bytes(
    std::int32_t number,const char* text,std::size_t length
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
bool ProgramStore::read_text_unlocked(
    std::size_t i,std::int32_t& number,const char*& text,
    std::size_t& length
) const {
    if(i>=count_) return fail("BAD LINE INDEX");
    if(backend_==ProgramBackend::Ram) {
        number=ram_->lines[i].number;
        text=ram_->lines[i].text;
        length=std::strlen(text);
        return true;
    }
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,sd_->work);
    FILE* f=std::fopen(path,"rb");
    if(!f)return fail("SD READ ERROR - PROGRAM STORAGE SUSPENDED",true);
    auto& entry=sd_->lines[i];
    char* raw=sd_->scratch;
    bool ok=std::fseek(f,entry.offset,SEEK_SET)==0 &&
        std::fgets(raw,sizeof(sd_->scratch),f);
    char* body=nullptr;std::size_t body_length=0;std::int32_t parsed=0;
    if(ok) {
        const std::size_t raw_length=std::strlen(raw);
        if(raw_length==sizeof(sd_->scratch)-1 &&
           raw[raw_length-1]!='\n') {
            ok=false;
        } else {
            ok=parse_text(
                raw,parsed,body,body_length,
                program_files::reserved(sd_->work),
                kMaxSdProgramLineLength);
        }
    }
    if(std::fclose(f)!=0)ok=false;
    if(!ok||parsed!=entry.number||body_length!=entry.length||
       hash_line(parsed,body,body_length)!=entry.hash)
        return fail("SD SOURCE CHANGED - PROGRAM STORAGE SUSPENDED",true);
    number=parsed;text=body;length=body_length;return true;
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
    std::size_t& count,std::size_t& bytes,const ProgramLine* edit,bool remove
) {
    SafeFileWriter writer;
    if(!writer.open(target,"RMBEDIT",root_))return fail(writer.error());
    count=0;bytes=0;bool inserted=false;
    auto emit=[&](std::int32_t number,const char* text,std::size_t length) {
        if(count>=kMaxSdProgramLines)return fail("SD PROGRAM FULL");
        if(length>=kMaxSdProgramLineLength)return fail("SD LINE TOO LONG");
        if(!index.reserve(count+1))return fail("OUT OF MEMORY");
        char prefix[16];
        const int prefix_length=std::snprintf(
            prefix,sizeof(prefix),"%ld ",static_cast<long>(number));
        if(prefix_length<=0||prefix_length>=static_cast<int>(sizeof(prefix)))
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
    for(std::size_t i=0;i<source.count_;++i) {
        std::int32_t number=0;const char* text=nullptr;std::size_t length=0;
        if(!source.read_text_unlocked(i,number,text,length))
            return fail(source.error(),&source==this&&source.suspended());
        if(edit&&!inserted&&edit->number<=number) {
            if(!remove&&!emit(
                   edit->number,edit->text,std::strlen(edit->text)))
                return false;
            inserted=true;
        }
        if(edit&&edit->number==number)continue;
        if(!emit(number,text,length))return false;
    }
    if(edit&&!inserted&&!remove&&
       !emit(edit->number,edit->text,std::strlen(edit->text)))
        return false;
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
        if(n>=count||
           !parse_text(index.scratch,number,body,length,true,
                       kMaxSdProgramLineLength)||
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
    char line[192];bool version=false,work=false,filename=false,dirty=false,ok=true;
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
            if(version||std::strcmp(equals,"1")!=0){ok=false;break;}
            version=true;
        } else if(program_files::equal(line,"work")) {
            if(work||!work_file_name(equals)){ok=false;break;}
            std::snprintf(session.work,sizeof(session.work),"%s",equals);work=true;
        } else if(program_files::equal(line,"file")) {
            if(filename||(*equals&&
               (!SafeFileWriter::valid_root_name(equals)||
                program_files::reserved(equals)))){ok=false;break;}
            std::snprintf(session.filename,sizeof(session.filename),"%s",equals);
            filename=true;
        } else if(program_files::equal(line,"dirty")) {
            if(dirty||(std::strcmp(equals,"0")&&std::strcmp(equals,"1")))
                {ok=false;break;}
            session.dirty=*equals=='1';dirty=true;
        } else {ok=false;break;}
    }
    if(std::ferror(file)||std::fclose(file)!=0)ok=false;
    return ok&&version&&work&&filename&&dirty;
}
bool ProgramStore::write_session(
    const char* work,const char* filename,bool dirty
) {
    if(!work_file_name(work)||!filename)return fail("BAD SESSION STATE");
    char text[256];
    const int length=std::snprintf(
        text,sizeof(text),"version=1\nwork=%s\nfile=%s\ndirty=%d\n",
        work,filename,dirty?1:0);
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
    if(!write_session(next->work,published_filename,dirty)) {
        remove_work(root_,next->work);
        return false;
    }
    SdProgramStore* old=sd_;
    sd_=next;count_=count;bytes_=bytes;backend_=ProgramBackend::Sd;
    suspended_=false;dirty_=dirty;session_recovered_=false;
    std::snprintf(filename_,sizeof(filename_),"%s",published_filename);
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
        if(!scan(session.work,*index,count)||!verify(*index,count))return false;
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
    sd_=next.release();count_=count;bytes_=bytes;backend_=ProgramBackend::Sd;
    mode_=mode;suspended_=false;dirty_=session.dirty;session_recovered_=true;
    std::snprintf(filename_,sizeof(filename_),"%s",session.filename);
    release_backend(ram_);ram_=nullptr;
    if(old)release_backend(old);
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
    const char* name,SdProgramStore& index,std::size_t& count
) const {
    char path[192];std::snprintf(path,sizeof(path),"%s%s",root_,name);
    FILE* f=std::fopen(path,"rb");if(!f)return fail("FILE NOT FOUND");
    count=0;bool ok=true;std::uint32_t position=0;
    while(true) {
        const auto offset=position;
        if(!std::fgets(index.scratch,sizeof(index.scratch),f))break;
        const auto raw_length=std::strlen(index.scratch);
        position+=static_cast<std::uint32_t>(raw_length);
        if(raw_length==sizeof(index.scratch)-1&&
           index.scratch[raw_length-1]!='\n') {
            ok=fail("SD LINE TOO LONG");break;
        }
        char* p=index.scratch;
        while(*p&&std::isspace(static_cast<unsigned char>(*p)))++p;
        if(!*p)continue;
        char* body=nullptr;std::size_t length=0;std::int32_t number=0;
        if(!parse_text(
               p,number,body,length,program_files::reserved(name),
               kMaxSdProgramLineLength)) {
            ok=fail("BAD BASIC FILE / SD LINE TOO LONG");break;
        }
        std::size_t pos=0;
        while(pos<count&&index.lines[pos].number<number)++pos;
        const bool exists=pos<count&&index.lines[pos].number==number;
        if(!*body) {
            if(exists) {
                for(auto j=pos;j+1<count;++j)index.lines[j]=index.lines[j+1];
                --count;
            }
        } else {
            if(!exists) {
                if(count==kMaxSdProgramLines) {
                    ok=fail("SD PROGRAM FULL");break;
                }
                if(!index.reserve(count+1)) {
                    ok=fail("OUT OF MEMORY");break;
                }
                for(auto j=count;j>pos;--j)index.lines[j]=index.lines[j-1];
                ++count;
            }
            index.lines[pos]={
                number,static_cast<std::uint32_t>(offset),
                hash_line(number,body,length),
                static_cast<std::uint16_t>(length)
            };
        }
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
        backend_=ProgramBackend::Ram;suspended_=false;error_="OK";
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
        // The SD session remains durable so an explicit later restore is possible.
        release_backend(sd_); sd_=nullptr; release_backend(ram_); ram_=next.release();
        if(discard) {count_=0;filename_[0]=0;dirty_=false;}
        bytes_=bytes; backend_=target; mode_=mode; suspended_=false;
        session_recovered_=false;protect(); return true;
    }
    if(!storage::available() || !storage::card_present()) return fail("SD CARD NOT AVAILABLE");
    Lease lease; if(!lease.locked) return fail(storage::last_error());
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    char work[80]; if(!new_work_name(work)) return false;
    ProgramStore empty; std::size_t count,bytes;
    if(!snapshot(work,discard?empty:*this,*next,count,bytes)) return false;
    const char* next_filename=discard?"":filename_;
    const bool next_dirty=discard?false:dirty_;
    if(!publish_sd(next.get(),count,bytes,next_filename,next_dirty))return false;
    next.release();
    release_backend(ram_); ram_=nullptr;
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
    if(!scan(sd_->work,*check,count)) return fail("SD WORK FILE MISSING - STORAGE SUSPENDED",true);
    bool changed=count!=count_;
    for(std::size_t i=0;!changed&&i<count;++i) {
        const auto& a=check->lines[i];
        const auto& b=sd_->lines[i];
        changed=a.number!=b.number||a.offset!=b.offset||
                a.length!=b.length||a.hash!=b.hash;
    }
    if(changed)
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
    std::size_t count=0,bytes=0;char work[80];
    if(!new_work_name(work))return false;
    if(*filename_) {
        ProgramStore input;input.root_=root_;input.backend_=ProgramBackend::Sd;
        input.sd_=allocate_backend<SdProgramStore>();
        if(!input.sd_)return fail("OUT OF MEMORY",true);
        if(!scan(filename_,*input.sd_,input.count_))
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
        if(!snapshot(work,input,*next,count,bytes))
            return fail("PROGRAM FILE CHANGED OR MISSING - STORAGE SUSPENDED",true);
    } else {
        ProgramStore empty;
        if(!snapshot(work,empty,*next,count,bytes))return false;
    }
    if(!publish_sd(next.get(),count,bytes,filename_,false))return false;
    next.release();dirty_=false;suspended_=false;error_="OK";protect();return true;
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
    if(!publish_sd(next.get(),count,bytes,filename_,true))return false;
    next.release();return true;
}
bool ProgramStore::set_line(std::int32_t number,const char* text) {
    if(!text)return fail("BAD LINE");
    if(std::strlen(text)>=kMaxProgramLineLength)
        return fail("LINE TOO LONG");
    if(backend_==ProgramBackend::Sd)return edit_sd(number,text,false);
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
        if(!publish_sd(next.get(),count,bytes,"",false))return false;
        next.release();
    } else {
        release_backend(ram_);ram_=nullptr;count_=bytes_=0;
        filename_[0]=0;dirty_=false;session_recovered_=false;
    }
    protect();return true;
}
bool ProgramStore::load(const char* name) {
    if(!ready()) return false;
    char filename[80];if(!normalize(name,filename)) return false;
    Lease lease;if(!lease.locked) return fail(storage::last_error());
    std::size_t bytes=0;
    if(backend_==ProgramBackend::Ram) {
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
        release_backend(ram_);ram_=next.ram_;next.ram_=nullptr;
        count_=next.count_;bytes_=next.bytes_;
        std::snprintf(filename_,sizeof(filename_),"%s",filename);
        dirty_=false;session_recovered_=false;protect();return true;
    }
    ProgramStore input; input.root_=root_;input.backend_=ProgramBackend::Sd;
    input.sd_=allocate_backend<SdProgramStore>();if(!input.sd_) return fail("OUT OF MEMORY");
    if(!scan(filename,*input.sd_,input.count_)) return false;
    char work[80];if(!new_work_name(work)) return false;
    BackendPtr<SdProgramStore> next(allocate_backend<SdProgramStore>());
    if(!next) return fail("OUT OF MEMORY");
    std::size_t count;
    if(!snapshot(work,input,*next,count,bytes)) return false;
    if(!publish_sd(next.get(),count,bytes,filename,false))return false;
    next.release();protect();return true;
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
       !write_session(sd_->work,filename,false))return false;
    std::snprintf(filename_,sizeof(filename_),"%s",filename);
    dirty_=false;session_recovered_=false;protect();return true;
}
} // namespace rmb
