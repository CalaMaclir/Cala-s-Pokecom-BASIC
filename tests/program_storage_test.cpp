#include "program_store.hpp"
#include "program_file_guard.hpp"
#include "file_management.hpp"
#include "transfer_file.hpp"
#include "basic_compiler.hpp"
#include "psram.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
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
namespace rmb::psram { void test_set_available(bool value); }
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
struct VisitedLine {
    std::size_t index;
    std::int32_t number;
    std::string body;
};
bool capture_line(
    std::size_t index,
    std::int32_t number,
    const char* body,
    std::size_t length,
    void* context
) {
    auto* lines=static_cast<std::vector<VisitedLine>*>(context);
    lines->push_back({index,number,std::string(body,length)});
    return true;
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
        if(current.backend_type()==ProgramBackend::Ram) {
            const auto allocation=
                psram::allocation(psram::Client::ProgramStore);
            assert(allocation.active);
            assert(allocation.allocated_bytes>=kPsramProgramStoreBytes);
            assert(current.internal_psram_extended());
            assert(current.line_capacity()==kMaxPsramProgramLines);
            assert(current.line_length_capacity()==
                   kMaxPsramProgramLineLength-1);
        }
        std::int32_t metadata_number=0;
        std::size_t metadata_length=0;
        assert(current.read_line_metadata(
            0,metadata_number,metadata_length));
        assert(metadata_number==10&&metadata_length==7);
        assert(!current.read_line_metadata(
            1,metadata_number,metadata_length));
        std::vector<VisitedLine> visited;
        assert(current.visit_line_range(0,33,capture_line,&visited));
        assert(visited.size()==1&&visited[0].index==0&&
               visited[0].number==10&&visited[0].body=="PRINT 1");
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
        ProgramStore spaced;spaced.set_root(root.c_str());
        assert(spaced.initialize(ProgramStorageMode::InternalRam));
        assert(spaced.set_line(10,"PRINT \"SPACE\""));
        assert(spaced.save("MY PROGRAM"));
        assert(!std::strcmp(spaced.filename(),"MY PROGRAM.BAS"));
        assert(read(root+"MY PROGRAM.BAS")=="10 PRINT \"SPACE\"\n");
        assert(spaced.clear());
        assert(spaced.load("MY PROGRAM.BAS"));
        assert(listing(spaced)=="10 PRINT \"SPACE\"\n");
    }
    {
        write(root+"OLD SOURCE.BAS","10 PRINT 41\n");
        ProgramStore renamed;renamed.set_root(root.c_str());
        assert(renamed.initialize(ProgramStorageMode::SdCard));
        assert(renamed.load("OLD SOURCE.BAS"));
        assert(file_management::rename_file(
            root.c_str(),"OLD SOURCE.BAS","NEW SOURCE.BAS",
            renamed.filename())==file_management::Result::Success);
        assert(renamed.note_source_renamed(
            "OLD SOURCE.BAS","NEW SOURCE.BAS"));
        assert(!std::strcmp(renamed.filename(),"NEW SOURCE.BAS"));
        assert(!std::filesystem::exists(root+"OLD SOURCE.BAS"));
        assert(read(root+"NEW SOURCE.BAS")=="10 PRINT 41\n");
    }
    {
        write(root+"ROLL BACK.BAS","10 PRINT 42\n");
        ProgramStore rollback;rollback.set_root(root.c_str());
        assert(rollback.initialize(ProgramStorageMode::SdCard));
        assert(rollback.load("ROLL BACK.BAS"));
        assert(file_management::rename_file(
            root.c_str(),"ROLL BACK.BAS","ROLL FORWARD.BAS",
            rollback.filename())==file_management::Result::Success);
        locked=true;
        assert(!rollback.note_source_renamed(
            "ROLL BACK.BAS","ROLL FORWARD.BAS"));
        locked=false;
        assert(file_management::rename_file(
            root.c_str(),"ROLL FORWARD.BAS","ROLL BACK.BAS",
            rollback.filename(),file_management::Access::Allowed,true)==
            file_management::Result::Success);
        assert(!std::strcmp(rollback.filename(),"ROLL BACK.BAS"));
        assert(std::filesystem::exists(root+"ROLL BACK.BAS"));
        assert(!std::filesystem::exists(root+"ROLL FORWARD.BAS"));
        assert(read(root+"ROLL BACK.BAS")=="10 PRINT 42\n");
    }
    {
        ProgramStore limits;limits.set_root(root.c_str());
        assert(limits.initialize(ProgramStorageMode::InternalRam));
        assert(limits.internal_psram_extended());
        assert(limits.line_capacity()==1024);
        assert(limits.line_length_capacity()==2047);

        const std::string maximum=make_body(2047,"PSRAM_END");
        assert(limits.set_line(10,maximum.c_str()));
        std::int32_t maximum_number=0;
        std::size_t maximum_length=0;
        assert(limits.read_line_metadata(
            0,maximum_number,maximum_length));
        assert(maximum_number==10&&maximum_length==2047);
        std::int32_t borrowed_number=0;
        const char* borrowed_body=nullptr;
        std::size_t borrowed_length=0;
        assert(limits.read_line_text(
            0,borrowed_number,borrowed_body,borrowed_length));
        const std::string borrowed_copy(borrowed_body,borrowed_length);
        assert(limits.read_line_metadata(
            0,maximum_number,maximum_length));
        assert(std::string(
            borrowed_body,borrowed_length)==borrowed_copy);

        const auto kept=listing(limits);
        assert(!limits.set_line(
            20,std::string(2048,'R').c_str()));
        assert(!std::strcmp(
            limits.error(), "LINE TOO LONG FOR INTERNAL PSRAM"));
        assert(listing(limits)==kept);

        write(root+"RAM2047.BAS",
              "10 "+std::string(2047,'R')+"\n");
        assert(limits.load("RAM2047"));
        assert(limits.size()==1);
        assert(limits.read_line_metadata(
            0,maximum_number,maximum_length));
        assert(maximum_length==2047);

        const auto loaded=listing(limits);
        write(root+"RAM2048.BAS",
              "10 "+std::string(2048,'R')+"\n");
        assert(!limits.load("RAM2048"));
        assert(listing(limits)==loaded);
    }

    // Explicitly disable PSRAM and verify the compatibility fallback remains
    // 256 source lines x 191 body characters.
    psram::test_set_available(false);
    {
        ProgramStore fallback;fallback.set_root(root.c_str());
        assert(fallback.initialize(ProgramStorageMode::InternalRam));
        assert(!fallback.internal_psram_extended());
        assert(fallback.line_capacity()==256);
        assert(fallback.line_length_capacity()==191);
        assert(fallback.set_line(
            10,std::string(191,'F').c_str()));
        assert(!fallback.set_line(
            20,std::string(192,'F').c_str()));
        assert(!std::strcmp(
            fallback.error(), "LINE TOO LONG FOR SRAM FALLBACK"));
        assert(fallback.clear());
        for(int i=1;i<=256;++i)
            assert(fallback.set_line(i,"REM fallback"));
        assert(!fallback.set_line(257,"END"));
        assert(!std::strcmp(
            fallback.error(), "PROGRAM TOO LARGE FOR SRAM FALLBACK"));
    }
    psram::test_set_available(true);
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
    // SD body reads populate the read-only PSRAM cache. Re-reading the same
    // line must hit it without changing source semantics.
    {
        std::int32_t cached_number=0;
        const char* cached_text=nullptr;
        std::size_t cached_length=0;
        const auto hits_before=p.sd_cache_hits();
        assert(p.read_line_text(0,cached_number,cached_text,cached_length));
        assert(p.sd_cache_bytes()>=kMaxSdProgramLineLength);
        assert(p.read_line_text(0,cached_number,cached_text,cached_length));
        assert(p.sd_cache_hits()>hits_before);
    }
    locked=true;assert(!p.set_line(30,"END"));assert(!p.save("TEST"));locked=false;
    assert(listing(p)==edited&&!p.suspended());
    fail_write=1;assert(!p.set_line(15,"PRINT 999"));assert(listing(p)==edited);assert(read(root+"TEST.BAS")==saved);
    // Failed close / first rename / install rename preserve both live index and old source.
    for(int phase=0;phase<2;++phase) {
        // PSRAM SD cache removes per-line source fopen/fclose from the hot
        // path. Fail the transactional writer close directly.
        if(phase==0) fail_close=1;
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
    // Transactional imports preserve sorting, duplicate replacement and
    // persisted empty source lines. Direct-mode deletion is a REPL concern.
    write(root+"SORT.BAS","30 END\n10 PRINT 1\n20 PRINT 2\n10 PRINT 3\n20\n");
    assert(p.load("SORT"));assert(listing(p)=="10 PRINT 3\n20 \n30 END\n");
    assert(p.switch_mode(ProgramStorageMode::SdCard));assert(p.load("SORT"));assert(listing(p)=="10 PRINT 3\n20 \n30 END\n");

    std::string range_program;
    for(int i=1;i<=40;++i)
        range_program+=std::to_string(i*10)+" PRINT "+std::to_string(i)+"\n";
    write(root+"RANGE.BAS",range_program);
    assert(p.load("RANGE"));
    for(const std::size_t index:{0u,19u,39u}) {
        std::int32_t number=0;
        std::size_t length=0;
        assert(p.read_line_metadata(index,number,length));
        assert(number==static_cast<std::int32_t>((index+1)*10));
        assert(length==std::string(
            "PRINT "+std::to_string(index+1)).size());
    }
    std::int32_t borrowed_number=0;
    const char* borrowed_body=nullptr;
    std::size_t borrowed_length=0;
    assert(p.read_line_text(
        19,borrowed_number,borrowed_body,borrowed_length));
    const std::string borrowed_copy(borrowed_body,borrowed_length);
    std::int32_t metadata_number=0;
    std::size_t metadata_length=0;
    assert(p.read_line_metadata(
        39,metadata_number,metadata_length));
    assert(std::string(borrowed_body,borrowed_length)==borrowed_copy);
    std::vector<VisitedLine> range_visited;
    assert(p.visit_line_range(0,33,capture_line,&range_visited));
    assert(range_visited.size()==33&&
           range_visited.front().number==10&&
           range_visited.back().number==330);
    range_visited.clear();
    assert(p.visit_line_range(35,33,capture_line,&range_visited));
    assert(range_visited.size()==5&&
           range_visited.front().number==360&&
           range_visited.back().number==400);
    locked=true;
    range_visited.clear();
    assert(!p.visit_line_range(
        0,33,capture_line,&range_visited));
    locked=false;
    assert(!p.suspended());
    card=false;
    assert(!p.read_line_metadata(
        0,metadata_number,metadata_length));
    assert(p.suspended());
    card=true;
    assert(p.resume());

    auto before=listing(p);write(root+"BAD.BAS","10 PRINT 1\nBAD\n");assert(!p.load("BAD"));assert(listing(p)==before);
    // SD line boundaries are independent of ProgramLine/RAM capacity.
    write(root+"EMPTY.BAS","10 \n");assert(p.load("EMPTY"));
    assert(p.size()==1&&listing(p)=="10 \n");
    std::vector<VisitedLine> empty_visited;
    assert(p.visit_line_range(0,33,capture_line,&empty_visited));
    assert(empty_visited.size()==1&&empty_visited[0].number==10&&
           empty_visited[0].body.empty());
    std::int32_t empty_metadata_number=0;
    std::size_t empty_metadata_length=99;
    assert(p.read_line_metadata(
        0,empty_metadata_number,empty_metadata_length));
    assert(empty_metadata_number==10&&empty_metadata_length==0);
    assert(!p.read_line_metadata(
        1,empty_metadata_number,empty_metadata_length));
    assert(p.save("EMPTYROUND"));assert(p.clear()&&p.load("EMPTYROUND"));
    assert(p.size()==1&&listing(p)=="10 \n");
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
    std::vector<VisitedLine> long_visited;
    assert(p.visit_line_range(0,33,capture_line,&long_visited));
    assert(long_visited.size()==2&&long_visited[0].number==10&&
           long_visited[0].body==long_body&&
           long_visited[1].number==20&&
           long_visited[1].body=="PRINT 77");
    // Stage 2 edits the complete SD line without routing it through the
    // fixed 191-character ProgramLine representation.
    std::string edited_long="REM "+std::string(2043,'X');edited_long[2000]='Z';
    assert(p.set_line(10,edited_long.c_str()));
    std::int32_t edit_number=0;const char* edit_text=nullptr;
    std::size_t edit_length=0;
    assert(p.read_line_text(0,edit_number,edit_text,edit_length));
    assert(edit_number==10&&edit_length==2047&&
           std::string(edit_text,edit_length)==edited_long);
    assert(!p.set_line(10,(edited_long+"X").c_str()));
    assert(!std::strcmp(p.error(),"SD LINE TOO LONG"));
    assert(p.read_line_text(0,edit_number,edit_text,edit_length));
    assert(std::string(edit_text,edit_length)==edited_long);
    assert(p.save("LONGEDIT"));
    assert(p.clear()&&p.load("LONGEDIT"));
    assert(p.read_line_text(0,edit_number,edit_text,edit_length));
    assert(edit_length==2047&&std::string(edit_text,edit_length)==edited_long);
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
    std::vector<VisitedLine> changed_visited;
    assert(!p.visit_line_range(
        0,2,capture_line,&changed_visited));
    assert(std::strstr(p.error(),"SD SOURCE CHANGED"));
    write(long_work,long_program);assert(p.resume());
    assert(listing(p)==long_program);

    // Enhanced INTERNAL PSRAM has the same 1024 x 2047 source limits as SD.
    const auto long_before=listing(p);
    assert(p.switch_mode(ProgramStorageMode::InternalRam));
    assert(p.internal_psram_extended());
    assert(p.line_capacity()==1024);
    assert(p.line_length_capacity()==2047);
    assert(listing(p)==long_before);
    assert(p.switch_mode(ProgramStorageMode::SdCard));
    assert(listing(p)==long_before);

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
    {
        ProgramStore internal_capacity;
        internal_capacity.set_root(root.c_str());
        assert(internal_capacity.initialize(
            ProgramStorageMode::InternalRam));
        assert(internal_capacity.internal_psram_extended());
        for(int i=1;i<=1024;++i)
            assert(internal_capacity.set_line(i,"REM capacity"));
        const auto full=listing(internal_capacity);
        assert(!internal_capacity.set_line(1025,"END"));
        assert(listing(internal_capacity)==full);
    }
    before=listing(p);
    assert(!p.load("BAD")); assert(listing(p)==before);
    fail_close=1; assert(!p.load("SORT")); assert(listing(p)==before);
    // Compile the same source through both backends: VM sees identical IL.
    {
    ProgramStore ram;ram.set_root(root.c_str());
    assert(ram.initialize(ProgramStorageMode::InternalRam));
    for(int i=1;i<argc;++i) {
        std::filesystem::copy_file(argv[i],root+"BENCH.BAS",std::filesystem::copy_options::overwrite_existing);
        assert(ram.load("BENCH"));assert(p.switch_mode(ProgramStorageMode::SdCard));assert(p.load("BENCH"));
        assert(listing(ram)==listing(p));parity(ram,p);
        static CompiledProgram il;BasicCompiler c;
        auto t=std::chrono::steady_clock::now();assert(c.compile(ram,il).ok);auto u=std::chrono::steady_clock::now();assert(c.compile(p,il).ok);auto v=std::chrono::steady_clock::now();
        std::printf("host compile %s RAM=%lld us SD-file=%lld us (not device timing)\n",argv[i],(long long)std::chrono::duration_cast<std::chrono::microseconds>(u-t).count(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(v-u).count());
    }
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
    assert(p.switch_mode(ProgramStorageMode::InternalRam));
    assert(p.internal_psram_extended());
    assert(listing(p)==before);
    assert(compiler.compile(p,repeated).ok);
    assert(repeated.line_count==400);
    assert(p.switch_mode(ProgramStorageMode::SdCard));
    assert(listing(p)==before);
    assert(p.save("LARGE2"));assert(p.load("LARGE2"));assert(listing(p)==before);
    assert(p.set_line(100,"PRINT \"EDITED\""));assert(p.set_line(105,"REM extra"));
    assert(p.size()==401);assert(p.erase_line(105));assert(p.size()==400);assert(p.resume());
    // Capacity belongs to source backend, not the compiler's RAM line map.
    assert(p.clear());for(int i=1;i<=1024;++i)assert(p.set_line(i,"REM capacity"));
    assert(!p.set_line(1025,"END"));assert(p.size()==1024);assert(p.resume());
    assert(compiler.compile(p,repeated).ok);assert(repeated.line_count==1024);
    assert(p.switch_mode(ProgramStorageMode::InternalRam,true));
    assert(p.internal_psram_extended());
    assert(p.set_line(10,"PRINT 99"));
    assert(p.load("LARGE"));
    assert(p.size()==400);
    assert(compiler.compile(p,repeated).ok);
    assert(repeated.line_count==400);
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
    ProgramLine suspended_line;
    assert(!p.read_line(0,suspended_line));
    assert(!p.set_line(10,"PRINT 999"));
    assert(!p.save("USBTEST.BAS"));
    assert(!p.load("USBTEST.BAS"));
    assert(p.suspended());
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
