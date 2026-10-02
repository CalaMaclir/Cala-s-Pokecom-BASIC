#include "program_store.hpp"
#include "file_management.hpp"
#include "psram.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
namespace {bool available=true,locked=false;int write_failure=0,rename_failure=0;}
extern "C" {
std::size_t __real_fwrite(const void*,std::size_t,std::size_t,FILE*);
int __real_rename(const char*,const char*);
std::size_t __wrap_fwrite(const void* p,std::size_t s,std::size_t n,FILE* f) {
    if(write_failure>0&&--write_failure==0){errno=EIO;return 0;}return __real_fwrite(p,s,n,f);
}
int __wrap_rename(const char* a,const char* b){if(rename_failure>0&&--rename_failure==0){errno=EIO;return -1;}return __real_rename(a,b);}
}
namespace rmb::storage {
bool init(){return ::available&&!locked;}bool available(){return ::available;}
bool remount(){return init();}
bool card_present(){return ::available;}bool firmware_owns_card(){return true;}
bool try_lock(){if(locked)return false;locked=true;return true;}void unlock(){assert(locked);locked=false;}
const char* last_error(){return "INJECTED STORAGE BUSY";}
}
namespace rmb::psram {void test_set_available(bool);void test_fail_write_after(int);}
using namespace rmb;
std::string listing(ProgramStore& p){std::string s;for(std::size_t i=0;i<p.size();++i){std::int32_t n;const char* b;std::size_t l;assert(p.read_line_text(i,n,b,l));s+=std::to_string(n)+" "+std::string(b,l)+"\n";}return s;}
int main(){
    char dir[]="/tmp/cpb-stage1-pair-XXXXXX";assert(mkdtemp(dir));const std::string root=std::string(dir)+"/";
    for(int mode=0;mode<3;++mode){
        psram::test_set_available(mode!=0);
        ProgramStore p;p.set_root(root.c_str());assert(p.initialize(mode==2?ProgramStorageMode::SdCard:ProgramStorageMode::InternalRam));
        assert(p.set_line(100,"PRINT 1:PRINT 2"));assert(p.set_line(200,"END"));
        const auto initial=listing(p);auto rev=p.revision();
        assert(p.replace_line_pair(100,"PRINT 1:",150,"PRINT 2"));assert(p.revision()==rev+1);
        assert(listing(p)=="100 PRINT 1:\n150 PRINT 2\n200 END\n");
        assert(p.replace_line_pair(100,"PRINT 1:PRINT 2",150,nullptr));assert(listing(p)==initial);
        const std::string large(p.line_length_capacity()+1,'X');rev=p.revision();
        assert(!p.replace_line_pair(100,large.c_str(),200,nullptr));assert(listing(p)==initial&&p.revision()==rev);
        assert(!p.replace_line_pair(100,"BAD",100,nullptr));assert(listing(p)==initial);
        assert(p.replace_line_pair(100,"",200,""));assert(listing(p)=="100 \n200 \n");
        assert(p.replace_line_pair(100,"  PRINT 1",200,"END"));
        const auto indented=listing(p);
        std::filesystem::create_directories(root+"GAMES");assert(p.save("GAMES/A"));
        assert(std::string(p.filename())=="GAMES/A.BAS");assert(p.load("GAMES/A.BAS"));assert(listing(p)==indented);
        assert(file_management::rename_directory(root.c_str(),"GAMES","RENAMED",p.filename())==file_management::Result::Success);
        assert(p.note_source_renamed("GAMES","RENAMED"));assert(std::string(p.filename())=="RENAMED/A.BAS");
        assert(p.save(p.filename()));assert(std::filesystem::exists(root+"RENAMED/A.BAS"));
        std::filesystem::rename(root+"RENAMED",root+"GAMES");assert(p.note_source_renamed("RENAMED","GAMES"));
        if(mode==1){
            for(int fail=1;fail<=3;++fail){
                const auto prior=listing(p);rev=p.revision();psram::test_fail_write_after(fail);
                assert(!p.replace_line_pair(100,"CHANGED",150,"ADDED"));
                assert(p.revision()==rev&&listing(p)==prior&&p.ready());
            }
        }
        if(mode==2){
            for(int fail=1;fail<=4;++fail){
                const auto prior=listing(p);rev=p.revision();write_failure=fail;
                assert(!p.replace_line_pair(100,"CHANGED",150,"ADDED"));
                write_failure=0;assert(!locked&&p.revision()==rev&&listing(p)==prior);
            }
            for(int fail=1;fail<=3;++fail){
                const auto prior=listing(p);rev=p.revision();rename_failure=fail;
                assert(!p.replace_line_pair(100,"CHANGED",150,"ADDED"));
                rename_failure=0;assert(!locked&&p.revision()==rev&&listing(p)==prior);
            }
            assert(p.replace_line_pair(100,"SUCCESS",150,"TAIL"));
        }
        assert(p.clear());
    }
    // At exact capacity, join may free a slot; split cannot add one.
    psram::test_set_available(false);
    {
        ProgramStore p;
        assert(p.initialize(ProgramStorageMode::InternalRam));
        for(int i=0;i<256;++i) {
            assert(p.set_line(i*10,"X"));
        }
        const auto full=listing(p);
        const auto revision=p.revision();
        assert(!p.replace_line_pair(0,"A",5,"B"));
        assert(listing(p)==full&&p.revision()==revision);
        assert(p.replace_line_pair(0,"XX",10,nullptr)&&p.size()==255);
    }
    std::filesystem::remove_all(root);std::puts("Program two-line transaction: PASS");
}
