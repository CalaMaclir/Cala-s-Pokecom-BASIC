#include "file_path.hpp"
#include "file_management.hpp"
#include "safe_file.hpp"
#include "program_file_guard.hpp"
#include "transfer_file.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include <unistd.h>
using namespace rmb;
std::string content(const std::string& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main() {
    namespace p=file_paths;namespace f=file_management;using f::Result;
    char out[80]={};
    for(const char* bad : {"../A","A/../B","A//B","A/./B","A/","A\\B","/A", " A/B","A /B","A/.B","A/B.","A/B ","A:\\B","A/%2e%2e","A/\nB"})
        assert(!p::valid_relative(bad));
    assert(p::valid_relative("A B/C D.BAS"));assert(p::valid_relative("",true));
    assert(p::normalize("/GAMES/DEMO.BAS",out,sizeof(out))&&std::string(out)=="GAMES/DEMO.BAS");
    assert(!p::normalize("//GAMES",out,sizeof(out)));
    assert(p::join("GAMES","DEMO.BAS",out,sizeof(out))&&std::string(out)=="GAMES/DEMO.BAS");
    assert(p::join("GAMES","/DEMO.BAS",out,sizeof(out))&&std::string(out)=="DEMO.BAS");
    assert(p::parent("A/B/C",out,sizeof(out))&&std::string(out)=="A/B");
    assert(p::parent(out,out,sizeof(out))&&std::string(out)=="A");
    assert(p::parent(out,out,sizeof(out))&&!*out);
    assert(p::same_or_child("games/demo.bas","GAMES"));assert(!p::same_or_child("GAMES2/X","GAMES"));
    assert(p::relocate("GAMES/A.BAS","GAMES","B",out,sizeof(out))&&std::string(out)=="B/A.BAS");
    assert(p::valid_relative(std::string(79,'A').c_str()));assert(!p::valid_relative(std::string(80,'A').c_str()));
    assert(!p::join(std::string(77,'A').c_str(),"BC",out,sizeof(out)));
    assert(f::normalize_program_name("GAMES/MY DEMO",out,sizeof(out))&&std::string(out)=="GAMES/MY DEMO.BAS");
    assert(!f::normalize_program_name(std::string(76,'A').c_str(),out,sizeof(out)));
    assert(f::protected_name("GAMES/RMBP0001.BAS"));assert(f::protected_name("RMBASIC.CFG/X.BAS"));
    assert(!SafeFileWriter::valid_root_name("GAMES/A.BAS")); // remote trust boundary unchanged
    char temp[]="/tmp/cpb-stage1-path-XXXXXX";assert(mkdtemp(temp));const std::string root=std::string(temp)+"/";
    assert(f::create_directory(root.c_str(),"GAMES")==Result::Success);
    assert(f::create_directory(root.c_str(),"GAMES/SUB")==Result::Success);
    assert(f::create_directory(root.c_str(),"GAMES")==Result::AlreadyExists);
    assert(f::create_directory(root.c_str(),"../BAD")==Result::InvalidName);
    assert(f::create_directory(root.c_str(),"BLOCKED",f::Access::UsbHost)==Result::UsbHost);
    const std::string body="10   PRINT 1\n";
    {
        SafeFileWriter w;assert(w.open("GAMES/A.BAS","RMBEDIT",root.c_str()));
        assert(std::filesystem::exists(root+"GAMES/RMBEDIT.TMP"));
        assert(!std::filesystem::exists(root+"RMBEDIT.TMP"));
        assert(w.write(reinterpret_cast<const std::uint8_t*>(body.data()),body.size()));assert(w.commit());
    }
    assert(content(root+"GAMES/A.BAS")==body);
    program_files::set_active("GAMES/A.BAS");
    assert(f::rename_directory(root.c_str(),"GAMES","DEMOS","GAMES/A.BAS")==Result::Success);
    assert(content(root+"DEMOS/A.BAS")==body);
    program_files::set_active("DEMOS/A.BAS");
    assert(f::delete_directory(root.c_str(),"DEMOS","DEMOS/A.BAS")==Result::Protected);
    assert(f::rename_directory(root.c_str(),"DEMOS","DEMOS/SUB2",nullptr)==Result::Protected);
    program_files::set_active(nullptr);
    assert(f::rename_directory(root.c_str(),"DEMOS","DEMOS/SUB2",nullptr)==Result::InvalidName);
    assert(f::delete_directory(root.c_str(),"DEMOS",nullptr)==Result::DirectoryNotEmpty);
    assert(f::delete_directory(root.c_str(),"DEMOS/SUB",nullptr)==Result::Success);
    assert(f::rename_file(root.c_str(),"DEMOS/A.BAS","DEMOS/B.BAS",nullptr)==Result::Success);
    {
        TransferFile file;assert(file.open("DEMOS/B.BAS",false,root.c_str()));
        std::uint8_t buffer[64]={};assert(file.read(buffer,sizeof(buffer))==static_cast<int>(body.size()));
        assert(std::memcmp(buffer,body.data(),body.size())==0);
    }
    {TransferFile file;assert(!file.open("DEMOS/B.BAS",true,root.c_str()));}
    assert(f::delete_file(root.c_str(),"DEMOS/B.BAS",nullptr)==Result::Success);
    assert(f::delete_directory(root.c_str(),"DEMOS",nullptr)==Result::Success);
    assert(rmdir(temp)==0);
    std::puts("Directory paths and transactional files: PASS");
}
