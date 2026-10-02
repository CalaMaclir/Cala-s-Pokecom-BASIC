#include "path_compaction.hpp"
#include <cassert>
#include <cstring>
#include <string>
#include <initializer_list>
#include <cstdio>
using namespace rmb::file_paths;
int main() {
    const char* paths[]={"testtesttest/program.bas","projects/games/demo.bas",
        "very/long/path/program.bas","demo.bas","verylongbasename0123456789.bas"};
    for(const char* path:paths) {
        const std::string original=path;
        for(std::size_t width=0;width<=80;++width) {
            char out[81];compact_path(path,width,CompactPathPolicy::ProgramName,out,sizeof(out));
            assert(std::strlen(out)<=width);assert(original==path);
            if(original.size()<=width)assert(out==original);
            const char* base=rmb::file_paths::basename(path);
            std::string stem=base;if(stem.size()>=4&&same(stem.c_str()+stem.size()-4,".bas"))stem.resize(stem.size()-4);
            if(width>=stem.size()&&original.size()>width)assert(std::string(out).find(stem)!=std::string::npos);
        }
    }
    char out[80];
    compact_path("projects/games/demo.bas",13,CompactPathPolicy::ProgramName,out,sizeof(out));
    assert(std::string(out)=="../games/demo");
    compact_path("very/long/path/program.bas",15,CompactPathPolicy::ProgramName,out,sizeof(out));
    assert(std::string(out)=="../path/program");
    compact_path("testtesttest/program.bas",12,CompactPathPolicy::ProgramName,out,sizeof(out));
    assert(std::string(out)=="te../program");
    compact_path("projects/games/demo.bas",20,CompactPathPolicy::BasenameOnly,out,sizeof(out));
    assert(std::string(out)=="demo.bas");
    char small[5];compact_path(paths[0],80,CompactPathPolicy::ProgramName,small,sizeof(small));
    assert(std::strlen(small)<=4);assert(std::strstr(small,".."));
    for(const std::string& path:std::initializer_list<std::string>{"","sample","sample/test","sample/test/program.bas",
                               "/sample/test/program.bas","//sample/test/program.bas",
                               std::string(79,'X')}) {
        const auto original=path;
        char full[display_capacity]={};
        format_root_path(path.c_str(),full,sizeof(full),true);
        assert(full[0]=='/'&&full[1]!='/');
        std::string expected=path;
        while(!expected.empty()&&expected[0]=='/')expected.erase(0,1);
        assert(std::string(full)=="/"+expected);
        for(std::size_t width=0;width<=81;++width) {
            struct Guard { char before='Q';char out[82];char after='Z'; } guarded;
            compact_root_path(path.c_str(),width,CompactPathPolicy::FullName,guarded.out,sizeof(guarded.out),true);
            assert(guarded.before=='Q'&&guarded.after=='Z');
            assert(std::strlen(guarded.out)<=width);
            if(width)assert(guarded.out[0]=='/');
            if(std::strlen(full)<=width)assert(std::string(guarded.out)==full);
            assert(path==original);
            char tiny[3]={'Q','Q','Q'};
            compact_root_path(path.c_str(),width,CompactPathPolicy::FullName,tiny,2,true);
            assert(tiny[2]=='Q'&&std::strlen(tiny)<=1);
        }
    }
    char root[81]={};format_root_path(std::string(79,'X').c_str(),root,sizeof(root));
    assert(std::strlen(root)==80&&root[79]=='X'&&root[80]==0);
    compact_root_path("",20,CompactPathPolicy::FullName,out,sizeof(out));assert(!*out);
    compact_root_path("",20,CompactPathPolicy::FullName,out,sizeof(out),true);assert(std::string(out)=="/");
    compact_root_path("sample",20,CompactPathPolicy::FullName,out,sizeof(out));assert(std::string(out)=="/sample");
    compact_path("sample/program.bas",20,CompactPathPolicy::BasenameOnly,out,sizeof(out));assert(std::string(out)=="program.bas");
    std::puts("Path compaction width contracts: PASS");
}
