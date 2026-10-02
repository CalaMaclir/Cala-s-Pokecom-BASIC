// Exercise the real Files and shared picker routes with a drawing/SD stub.
#include "benchmark_platform.hpp"
#include <algorithm>
namespace {
std::vector<rmb::storage::DirectoryEntry> listing;
std::vector<std::array<UiRow,40>> frames;
std::vector<std::size_t> skips;
bool nested=false,shrink=false;
rmb::storage::DirectoryEntry entry(const char* name,bool directory=false,std::uint32_t size=123) {
    rmb::storage::DirectoryEntry e{};std::snprintf(e.name,sizeof(e.name),"%s",name);e.directory=directory;e.size=size;return e;
}
}
namespace rmb::storage {
std::size_t collect_directory_entries(const char* path,bool,DirectoryEntry* out,std::size_t max,std::size_t skip,bool* more) {
    skips.push_back(skip);
    auto entries=listing;
    if(nested) {
        if(!*path)entries={entry("sample",true)};
        else if(std::string(path)=="sample")entries={entry("test",true)};
        else {assert(std::string(path)=="sample/test");entries={entry("program.bas")};}
    }
    if(shrink)entries.resize(std::min<std::size_t>(1,entries.size()));
    const auto n=skip<entries.size()?std::min(max,entries.size()-skip):0;
    std::copy_n(entries.begin()+std::min(skip,entries.size()),n,out);
    if(more)*more=skip+n<entries.size();return n;
}
bool create_directory(const char*){return true;}
bool delete_directory(const char*,const char*){return true;}
bool rename_directory(const char*,const char*,const char*){return true;}
bool root_entry_info(const char*,DirectoryEntry& out){out=entry("program.bas");return true;}
bool rename_root_file(const char*,const char*,const char*,bool){return true;}
bool delete_root_file(const char*,const char*){return true;}
}
int main() {
    rmb::Repl repl;
    auto keys=[&](std::vector<int> k) {
        editor_keys=std::move(k);editor_key_index=0;frames.clear();
        ui_before_key=[&](){frames.push_back(ui_rows);if(editor_key_index==5)shrink=true;};
        repl.draw_menu_header(nullptr,nullptr);
    };
    listing={entry("games",true),entry("music",true),entry("demo.bas"),
             entry("a_very_long_file_name_that_must_be_compacted_with_its_basename.bas",false,4294967295u)};
    for(int theme=0;theme<3;++theme) {
        repl.settings_.theme=theme;shrink=false;
        keys({0xd2,0xb6,0xb6,0xb7,0xb4,'f',0xb1});assert(!repl.menu_files());
        bool directory=false,file=false,selected=false,restored=false,blank=false;
        for(const auto& screen:frames) {
            for(int row=7;row<29;++row) {
                const auto& v=screen[row];assert(v.text.size()==53);
                if(v.text.rfind("[DIR] ",0)==0) {
                    assert(v.bg!=0);directory=true;
                    if(v.fg==0xffffff)selected=true;else restored=true;
                } else if(v.text.substr(6,8)=="demo.bas") {file=true;assert(v.bg==0||v.fg==0xffffff);}
                if(v.text==std::string(53,' ')){assert(v.bg==0);blank=true;}
            }
        }
        assert(directory&&file&&selected&&restored&&blank);
        assert(frames.back()[8].text==std::string(53,' ')); // reduced list clears stale rows
    }
    nested=true;shrink=false;ui_before_key=[&](){frames.push_back(ui_rows);};
    keys({0x0a,0x0a,0x0a});ui_before_key=[&](){frames.push_back(ui_rows);};
    char chosen[80]={};assert(repl.pick_program_file(chosen,sizeof(chosen)));
    assert(std::string(chosen)=="sample/test/program.bas");
    assert(frames.back()[6].text=="/sample/test");
    assert(frames.back()[7].text.rfind("      program.bas",0)==0);
    keys({0x0a,0x0a,0x0a});ui_before_key=[&](){frames.push_back(ui_rows);};
    assert(repl.pick_transfer_file(chosen,sizeof(chosen),"SEND FILE"));
    assert(std::string(chosen)=="sample/test/program.bas");
    nested=false;listing.clear();skips.clear();
    for(int i=0;i<130;++i){char name[40];std::snprintf(name,sizeof(name),"F%03d.bas",i);listing.push_back(entry(name));}
    std::vector<int> sequence(129,0xb6);sequence.push_back(0xb1);keys(sequence);
    ui_before_key=[&](){frames.push_back(ui_rows);};
    assert(!repl.pick_transfer_file(chosen,sizeof(chosen),"SEND FILE"));
    assert(std::find(skips.begin(),skips.end(),128)!=skips.end());
    assert(frames.back()[7].text.substr(6,8)=="F128.bas");
    listing.clear();keys({0xb1});ui_before_key=[&](){frames.push_back(ui_rows);};
    assert(!repl.pick_program_file(chosen,sizeof(chosen)));
    assert(frames.back()[7].text.rfind("(EMPTY DIRECTORY)",0)==0);
    for(int r=8;r<32;++r)assert(frames.back()[r].text==std::string(53,' '));
    ui_before_key=nullptr;
    // The bottom F-key stays basename-only; the console action reports a path.
    std::strcpy(repl.settings_.quick[0].filename,"sample/test/program.bas");
    repl.settings_.quick[0].run=false;
    repl.render_function_keys();
    assert(ui_rows[39].text.find("sample")==std::string::npos);
    assert(ui_rows[39].text.find('/')==std::string::npos);
    output.clear();repl.handle_quick_key(0x81);
    assert(output.find("F1: LOAD /sample/test/program.bas\r\n")!=std::string::npos);
    assert(std::strcmp(repl.settings_.quick[0].filename,"sample/test/program.bas")==0);
    std::puts("Actual Files/picker drawing, themes, scrolling and batching: PASS");
}
