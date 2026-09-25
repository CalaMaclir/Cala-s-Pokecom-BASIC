#include "program_store.hpp"
#include "program_file_guard.hpp"
#include "transfer_file.hpp"
#include "basic_compiler.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include <unistd.h>
namespace {
bool card=true,mounted=true,locked=false;
int fail_write=0,fail_close=0,fail_rename=0,fail_remove=0;
}
extern "C" {
size_t __real_fwrite(const void*,size_t,size_t,FILE*);
int __real_fclose(FILE*);
int __real_rename(const char*,const char*);
int __real_remove(const char*);
int __wrap_remove(const char* path) {
    if(fail_remove>0 && --fail_remove==0) return -1;
    return __real_remove(path);
}
// Reproduce the pinned pico-vfs _ftello_r: reports the underlying descriptor
// position rather than the stdio logical position after buffered fgets.
long __wrap_ftell(FILE* f) { return lseek(fileno(f),0,SEEK_CUR); }
size_t __wrap_fwrite(const void* p,size_t s,size_t n,FILE* f) {
    if(fail_write>0 && --fail_write==0) return 0;
    return __real_fwrite(p,s,n,f);
}
int __wrap_fclose(FILE* f) {int r=__real_fclose(f);return fail_close>0 && --fail_close==0 ? EOF:r;}
int __wrap_rename(const char* a,const char* b) {
    if(fail_rename>0 && --fail_rename==0) return -1;
    return __real_rename(a,b);
}
}
namespace rmb::storage {
bool init(){return card&&mounted&&!locked;}
bool available(){return mounted;}
bool card_present(){return card;}
bool remount(){if(locked)return false;mounted=card;return mounted;}
bool firmware_owns_card(){return true;}
bool try_lock(){if(locked||!card||!mounted)return false;locked=true;return true;}
void unlock(){assert(locked);locked=false;}
const char* last_error(){return locked?"STORAGE BUSY":"SD NOT AVAILABLE";}
}
using namespace rmb;
std::string read(const std::string& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
void write(const std::string& path,const std::string& text){std::ofstream(path)<<text;}
std::string listing(const ProgramStore& p) {
    std::string out;
    for(size_t i=0;i<p.size();++i) {
        std::int32_t number=0;const char* text=nullptr;std::size_t length=0;
        assert(p.read_line_text(i,number,text,length));
        out+=std::to_string(number)+" ";
        out.append(text,length);out+="\n";
    }
    return out;
}
std::string make_body(std::size_t length,const std::string& tail={}) {
    assert(length>=tail.size());
    return std::string(length-tail.size(),'X')+tail;
}
void parity(const ProgramStore& a,const ProgramStore& b) {
    static CompiledProgram x,y;BasicCompiler c;
    auto xr=c.compile(a,x),yr=c.compile(b,y);assert(xr.ok==yr.ok);
    if(!xr.ok){assert(!std::strcmp(xr.message,yr.message));return;}
    assert(x.code_count==y.code_count&&x.string_used==y.string_used&&x.number_count==y.number_count);
    assert(!std::memcmp(x.code,y.code,x.code_count*sizeof(Op)));
    assert(!std::memcmp(x.string_pool,y.string_pool,x.string_used));
    assert(!std::memcmp(x.number_pool,y.number_pool,x.number_count*sizeof(BasicNumber)));
}
int main(int argc,char** argv) {
    char temp[]="/tmp/rmb075-XXXXXX";assert(mkdtemp(temp));std::string root=std::string(temp)+"/";
    // Current-name saves use the same transactional ProgramStore::save path
    // in every selectable mode and both AUTO backend outcomes.
    for(int scenario=0;scenario<4;++scenario) {
        card=mounted=scenario!=3;
        ProgramStorageMode mode=
            scenario==0 ? ProgramStorageMode::SdCard :
            scenario==1 ? ProgramStorageMode::InternalRam :
            ProgramStorageMode::Auto;
        ProgramStore current;current.set_root(root.c_str());
        assert(current.initialize(mode));
        assert(current.backend_type()==
            ((scenario==0||scenario==2)?ProgramBackend::Sd:ProgramBackend::Ram));
        // AUTO selected RAM while no card was present. A later card insertion
        // makes SAVE available without silently migrating the live backend.
        if(scenario==3) card=mounted=true;
        assert(current.set_line(10,"PRINT 1"));
        char name[24];std::snprintf(name,sizeof(name),"MODE%d",scenario);
        assert(current.save(name));
        assert(current.filename()[0] && !current.is_dirty());
        assert(current.set_line(10,"PRINT 2") && current.is_dirty());
        assert(current.save(current.filename()));
        assert(!current.is_dirty());
        assert(read(root+std::string(current.filename()))=="10 PRINT 2\n");
    }
    card=mounted=true;
    {
        ProgramStore limits;limits.set_root(root.c_str());
        assert(limits.initialize(ProgramStorageMode::InternalRam));
        assert(limits.set_line(10,std::string(191,'R').c_str()));
        const auto kept=listing(limits);
        assert(!limits.set_line(20,std::string(192,'R').c_str()));
        assert(!std::strcmp(limits.error(),"LINE TOO LONG"));
        assert(listing(limits)==kept);
        write(root+"RAM192.BAS","10 "+std::string(192,'R')+"\n");
        assert(!limits.load("RAM192"));
        assert(listing(limits)==kept);
    }
    {
        ProgramStore unavailable; unavailable.set_root(root.c_str());card=mounted=false;
        assert(!unavailable.initialize(ProgramStorageMode::SdCard));
        assert(unavailable.backend_type()==ProgramBackend::Sd && unavailable.suspended());
        card=mounted=true; assert(unavailable.resume()); assert(unavailable.clear());
    }
    ProgramStore p;p.set_root(root.c_str());card=mounted=false;
    assert(p.initialize(ProgramStorageMode::Auto));assert(p.backend_type()==ProgramBackend::Ram);
    assert(p.set_line(10,"PRINT 3"));assert(p.set_line(20,"GOTO 10"));
    auto original=listing(p);assert(!p.switch_mode(ProgramStorageMode::SdCard));assert(listing(p)==original);
    card=mounted=true;assert(p.backend_type()==ProgramBackend::Ram); // hotplug doesn't migrate
    assert(p.switch_mode(ProgramStorageMode::SdCard));assert(p.is_dirty());assert(listing(p)==original);
    assert(p.save("TEST"));auto saved=read(root+"TEST.BAS");assert(saved==original);
    assert(program_files::in_use("test.bas"));assert(!program_files::in_use("OTHER.BAS"));
    TransferFile transfer;assert(!transfer.open("test.bas",true,root.c_str()));assert(!std::strcmp(transfer.error(),"FILE IN USE"));
    assert(!transfer.open("RMBP0000.BAS",true,root.c_str()));
    assert(p.set_line(15,"PRINT \"EDIT\""));assert(p.erase_line(20));
    auto edited=listing(p);assert(read(root+"TEST.BAS")==saved);assert(p.is_dirty());
    locked=true;assert(!p.set_line(30,"END"));assert(!p.save("TEST"));locked=false;
    assert(listing(p)==edited&&!p.suspended());
    fail_write=1;assert(!p.set_line(15,"PRINT 999"));assert(listing(p)==edited);assert(read(root+"TEST.BAS")==saved);
    // Failed close / first rename / install rename preserve both live index and old source.
    for(int phase=0;phase<2;++phase) {
        if(phase==0) fail_close=static_cast<int>(p.size())+1; // source reads then writer close
        else fail_rename=phase;
        assert(!p.set_line(15,"PRINT 999"));assert(listing(p)==edited);assert(read(root+"TEST.BAS")==saved);
    }
    // Failed overwrite preserves the named BAS, in-memory index and dirty state.
    fail_rename=2; assert(!p.save("TEST")); assert(read(root+"TEST.BAS")==saved);
    assert(listing(p)==edited && p.is_dirty());
    // SAVE to the same named source never truncates its own input.
    assert(p.save("TEST.BAS"));assert(read(root+"TEST.BAS")==edited);
    assert(p.clear());assert(p.size()==0);assert(read(root+"TEST.BAS")==edited);assert(!program_files::in_use("TEST.BAS"));
    assert(p.load("TEST"));assert(listing(p)==edited);
    card=false;ProgramLine line;assert(!p.read_line(0,line));assert(p.suspended());assert(p.backend_type()==ProgramBackend::Sd);
    card=true;assert(!p.read_line(0,line));assert(p.resume());assert(listing(p)==edited);
    // Reinserted/replaced media must match the expected working source.
    std::string work;
    for(const auto& e:std::filesystem::directory_iterator(root)) {
        if(e.path().filename().string().rfind("RMBP",0)==0 && read(e.path().string())==edited) work=e.path().string();
    }
    assert(!work.empty());auto retained=read(work);write(work,"10 PRINT 999\n");
    assert(!p.resume()); assert(p.suspended());write(work,retained);assert(p.resume());
    assert(p.switch_mode(ProgramStorageMode::InternalRam));assert(listing(p)==edited);assert(!program_files::in_use("TEST.BAS"));
    assert(p.switch_mode(ProgramStorageMode::Auto));assert(p.backend_type()==ProgramBackend::Sd);
    card=false;assert(!p.clear());assert(p.suspended());assert(!p.switch_mode(ProgramStorageMode::InternalRam));
    assert(p.switch_mode(ProgramStorageMode::InternalRam,true));assert(p.size()==0);
    card=mounted=true;
    // Transactional imports preserve sorting, duplicate replacement and deletion semantics.
    write(root+"SORT.BAS","30 END\n10 PRINT 1\n20 PRINT 2\n10 PRINT 3\n20\n");
    assert(p.load("SORT"));assert(listing(p)=="10 PRINT 3\n30 END\n");
    assert(p.switch_mode(ProgramStorageMode::SdCard));assert(p.load("SORT"));assert(listing(p)=="10 PRINT 3\n30 END\n");
    auto before=listing(p);write(root+"BAD.BAS","10 PRINT 1\nBAD\n");assert(!p.load("BAD"));assert(listing(p)==before);
    // SD line boundaries are independent of ProgramLine/RAM capacity.
    write(root+"EMPTY.BAS","10 \n");assert(p.load("EMPTY"));assert(p.size()==0);
    for(const std::size_t length:{1u,190u,191u,192u,511u,1023u,2047u}) {
        const auto body=make_body(length);
        const auto name="BOUND"+std::to_string(length)+".BAS";
        write(root+name,"10 "+body+"\r\n");
        assert(p.load(name.c_str()));
        std::int32_t number=0;const char* text=nullptr;std::size_t actual=0;
        assert(p.read_line_text(0,number,text,actual));
        assert(number==10&&actual==length&&std::string(text,actual)==body);
    }
    before=listing(p);
    write(root+"LONG2048.BAS","10 "+make_body(2048)+"\n");
    assert(!p.load("LONG2048"));
    assert(std::strstr(p.error(),"LINE TOO LONG"));
    assert(listing(p)==before);
    write(root+"LONG2049.BAS","10 "+make_body(2049)+"\n");
    assert(!p.load("LONG2049"));
    assert(std::strstr(p.error(),"LINE TOO LONG"));
    assert(listing(p)==before);

    const std::string sentinel="END_SENTINEL";
    const std::string long_body=
        "REM "+make_body(2047-4-sentinel.size(),sentinel);
    const std::string long_program=
        "10 "+long_body+"\n20 PRINT 77\n";
    write(root+"LONGOK.BAS",long_program);
    assert(p.load("LONGOK"));
    assert(listing(p)==long_program);
    static CompiledProgram long_il;BasicCompiler long_compiler;
    assert(long_compiler.compile(p,long_il).ok);
    assert(p.save("LONGROUND"));
    assert(read(root+"LONGROUND.BAS")==long_program);
    assert(p.clear()&&p.load("LONGROUND"));
    assert(listing(p)==long_program);

    // Hash verification covers bytes well beyond the legacy 191-character tail.
    std::string long_work;
    for(const auto& e:std::filesystem::directory_iterator(root)) {
        const auto name=e.path().filename().string();
        if(name.rfind("RMBP",0)==0&&read(e.path().string())==long_program)
            long_work=e.path().string();
    }
    assert(!long_work.empty());
    std::fstream mutate(long_work,std::ios::in|std::ios::out|std::ios::binary);
    mutate.seekp(3+500);mutate.put('Z');mutate.close();
    std::int32_t hash_number=0;const char* hash_text=nullptr;
    std::size_t hash_length=0;
    assert(!p.read_line_text(0,hash_number,hash_text,hash_length));
    assert(std::strstr(p.error(),"SD SOURCE CHANGED"));
    write(long_work,long_program);assert(p.resume());
    assert(listing(p)==long_program);

    // A long SD source cannot be migrated into the fixed RAM ProgramLine store.
    const auto long_before=listing(p);
    assert(!p.switch_mode(ProgramStorageMode::InternalRam));
    assert(!std::strcmp(p.error(),"LINE TOO LONG FOR RAM"));
    assert(p.backend_type()==ProgramBackend::Sd&&listing(p)==long_before);

    // Syntax after character 191 proves the compiler consumes the full SD line.
    const std::string late_error=
        "PRINT 1"+std::string(220,' ')+": THIS IS NOT BASIC";
    write(root+"LATEERR.BAS","10 "+late_error+"\n");
    assert(p.load("LATEERR"));
    const auto late_result=long_compiler.compile(p,long_il);
    assert(!late_result.ok);

    assert(!p.load("../TEST.BAS"));assert(!p.save("RMBEDIT.TMP"));
    // Leftover staging from interrupted power is never overwritten automatically.
    write(root+"RMBEDIT.BAK","recover me");assert(!p.set_line(40,"END"));assert(read(root+"RMBEDIT.BAK")=="recover me");
    std::filesystem::remove(root+"RMBEDIT.BAK");
    assert(p.clear());for(int i=1;i<=256;++i)assert(p.set_line(i,"REM capacity"));
    before=listing(p);
    assert(p.switch_mode(ProgramStorageMode::InternalRam));assert(listing(p)==before);
    assert(!p.set_line(257,"END"));assert(listing(p)==before);
    assert(!p.load("BAD")); assert(listing(p)==before);
    fail_close=1; assert(!p.load("SORT")); assert(listing(p)==before);
    // Compile the same source through both backends: VM sees identical IL.
    ProgramStore ram;ram.set_root(root.c_str());
    for(int i=1;i<argc;++i) {
        std::filesystem::copy_file(argv[i],root+"BENCH.BAS",std::filesystem::copy_options::overwrite_existing);
        assert(ram.load("BENCH"));assert(p.switch_mode(ProgramStorageMode::SdCard));assert(p.load("BENCH"));
        assert(listing(ram)==listing(p));parity(ram,p);
        static CompiledProgram il;BasicCompiler c;
        auto t=std::chrono::steady_clock::now();assert(c.compile(ram,il).ok);auto u=std::chrono::steady_clock::now();assert(c.compile(p,il).ok);auto v=std::chrono::steady_clock::now();
        std::printf("host compile %s RAM=%lld us SD-file=%lld us (not device timing)\n",argv[i],(long long)std::chrono::duration_cast<std::chrono::microseconds>(u-t).count(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(v-u).count());
    }
    // Exact hardware route, CRLF input and a pico-vfs-style broken ftell.
    write(root+"A.BAS","10 PRINT 11\r\n20 END\r\n");
    write(root+"B.BAS","10 PRINT 22\r\n20 GOSUB 40\r\n30 END\r\n40 RETURN\r\n");
    write(root+"C.BAS","10 PRINT 33\n20 END\n");
    assert(p.switch_mode(ProgramStorageMode::InternalRam));assert(p.load("A"));
    assert(p.switch_mode(ProgramStorageMode::Auto));
    static CompiledProgram repeated; BasicCompiler compiler;
    for(int i=0;i<100;++i) {
        const char* name=i%3==0?"B":i%3==1?"A":"C";
        assert(p.load(name));assert(!p.suspended());
        auto contents=listing(p);assert(contents.find(name[0]=='B'?"22":name[0]=='A'?"11":"33")!=std::string::npos);
        assert(p.resume()); // independently re-scan and compare all entries
        assert(compiler.compile(p,repeated).ok);
    }
    before=listing(p);auto old_name=std::string(p.filename());auto old_mode=p.mode();
    // A failure during the final post-close verification must keep ALL old
    // state, not merely its source filename. The candidate is not published.
    fail_close=7; // B scan + four source reads + writer close + verification close
    assert(!p.load("B"));assert(listing(p)==before);assert(p.filename()==old_name);
    assert(p.mode()==old_mode && !p.suspended());assert(p.resume());
    fail_remove=1;assert(p.load("A")); // old-file cleanup is best effort
    assert(p.resume());assert(compiler.compile(p,repeated).ok);
    std::filesystem::copy_file("tests/fixtures/cpb_large_400.bas",root+"LARGE.BAS");
    assert(p.load("LARGE"));assert(p.size()==400);assert(p.resume());
    before=listing(p);assert(compiler.compile(p,repeated).ok);assert(repeated.line_count==400);
    assert(!p.switch_mode(ProgramStorageMode::InternalRam));assert(listing(p)==before);
    assert(p.backend_type()==ProgramBackend::Sd);
    assert(p.save("LARGE2"));assert(p.load("LARGE2"));assert(listing(p)==before);
    assert(p.set_line(100,"PRINT \"EDITED\""));assert(p.set_line(105,"REM extra"));
    assert(p.size()==401);assert(p.erase_line(105));assert(p.size()==400);assert(p.resume());
    // Capacity belongs to source backend, not the compiler's RAM line map.
    assert(p.clear());for(int i=1;i<=1024;++i)assert(p.set_line(i,"REM capacity"));
    assert(!p.set_line(1025,"END"));assert(p.size()==1024);assert(p.resume());
    assert(compiler.compile(p,repeated).ok);assert(repeated.line_count==1024);
    assert(p.switch_mode(ProgramStorageMode::InternalRam,true));
    assert(p.set_line(10,"PRINT 99"));before=listing(p);assert(!p.load("LARGE"));
    assert(!std::strcmp(p.error(),"PROGRAM TOO LARGE FOR RAM MODE"));assert(listing(p)==before);
    assert(p.set_line(10,"  PRINT 3"));before=listing(p);
    assert(p.switch_mode(ProgramStorageMode::Auto));assert(listing(p)==before);assert(p.resume());

    // USB handoff never publishes dirty SD source and never reuses the old
    // offset/hash index. The backing BAS is scanned into a fresh index after
    // the host returns the card.
    assert(p.save("USBTEST.BAS"));
    assert(p.set_line(20,"PRINT 4"));
    assert(!p.suspend_for_usb());
    assert(!std::strcmp(p.error(),"PROGRAM MODIFIED - SAVE OR DISCARD BEFORE USB STORAGE"));
    assert(p.save("USBTEST.BAS"));
    assert(p.suspend_for_usb() && p.suspended());
    mounted=false;
    write(root+"USBTEST.BAS","10 PRINT 80\n20 END\n");
    mounted=true;
    assert(p.resume_after_usb());
    assert(listing(p)=="10 PRINT 80\n20 END\n");
    assert(p.suspend_for_usb());mounted=false;
    std::filesystem::remove(root+"USBTEST.BAS");mounted=true;
    assert(!p.resume_after_usb() && p.suspended());
    write(root+"USBTEST.BAS","10 PRINT 81\n20 END\n");
    assert(p.resume_after_usb());
    assert(listing(p)=="10 PRINT 81\n20 END\n");

    // USB return rebuilds offsets, lengths and full-line hashes for long source.
    const std::string usb_long_body=
        "REM "+make_body(2047-4-sentinel.size(),sentinel);
    const std::string usb_long_program=
        "10 "+usb_long_body+"\n20 PRINT 82\n";
    write(root+"USBTEST.BAS",usb_long_program);
    assert(p.load("USBTEST.BAS"));
    assert(p.suspend_for_usb());mounted=false;
    std::string usb_changed=usb_long_program;
    usb_changed[3+1000]='Q';
    write(root+"USBTEST.BAS",usb_changed);mounted=true;
    assert(p.resume_after_usb());
    assert(listing(p)==usb_changed);

    std::filesystem::remove_all(temp);
    std::puts("Program storage transactions, backend parity and failures passed");
}
