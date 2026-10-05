// Exercise the real Files and shared picker routes with a drawing/SD stub.
#include "benchmark_platform.hpp"
#include <algorithm>
#include "soak_support.hpp"
#include "usb_device.hpp"
#include "usb_msc.hpp"
namespace {
std::vector<rmb::storage::DirectoryEntry> listing;
std::vector<std::array<UiRow,40>> frames;
std::vector<std::size_t> skips;
bool nested=false,preview_nested=false,shrink=false;
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
    if(preview_nested) {
        if(!*path)entries={entry("music",true)};
        else {assert(std::string(path)=="music");entries={entry("song.wav")};}
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
namespace rmb::platform {
bool usb_cdc_ready(){return true;}
bool serial_transfer_active(){return false;}
DiagnosticSerialResult begin_usb_diagnostic(){return DiagnosticSerialResult::Ready;}
bool write_usb_diagnostic(const char* text){put_string(text);return true;}
void end_usb_diagnostic(){}
const SerialTransferPerformance& serial_transfer_performance(){static SerialTransferPerformance perf;return perf;}
const char* uart_rx_mode_name(UartRxMode){return "IRQ";}
}
namespace rmb::network {
bool get_ip(char*,std::size_t){return false;}
const char* current_ssid(){return "";}
}
namespace rmb::storage {
bool busy(){return host_audio_active;}
const char* owner_name(){return "Firmware";}
}
namespace rmb::usb_device {bool usb_connected(){return true;}}
namespace rmb::usb_msc {bool active(){return false;}bool host_ejected(){return false;}bool media_present(){return true;}}
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
    // Preview input owns stop/pause presses. The following independent press
    // retains normal Files meaning, while hardware repeats remain consumed.
    listing={entry("song.wav"),entry("second.mp3")};
    const std::vector<int> stop_keys={0xb1,0x1b,'p','P',13,10,8,127,3,0xb4,0xb7,0xd2,0xd4,
        'r','R','e','E','n','N','m','M','f','F',0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x90};
    for(const int stop:stop_keys) {
        nested=false;shrink=false;skips.clear();
        keys({0xb7,'p',stop,stop,0xb1});
        rmb::platform::host_key_repeats={false,false,false,true,false};
        const int stops_before=host_audio_stop_count;
        int stopped_frames=0;std::size_t stopped_at_key=0;
        ui_before_key=[&] {
            frames.push_back(ui_rows);
            if(editor_key_index==2) {
                assert(host_audio_active);
                assert(ui_rows[36].text.find("Playing: /second.mp3")!=std::string::npos);
            }
            if(editor_key_index==3 || editor_key_index==4) {
                assert(!host_audio_active);++stopped_frames;stopped_at_key=editor_key_index;
                assert(ui_rows[3].text=="FILES [DIRECTORY]");
                assert(ui_rows[6].text=="/");
            }
        };
        assert(!repl.menu_files());
        assert(editor_key_index==5&&stopped_frames==2&&stopped_at_key==4);
        assert(host_audio_stop_count==stops_before+1);
        assert(skips.size()==2); // Initial view + mode switch, no stop rescan.
        rmb::platform::host_key_repeats.clear();
    }
    keys({0xb7,'p',' ',' ',0xb6,'i','p',0xb1});
    rmb::platform::host_key_repeats={false,false,false,true,false,false,false,false};
    ui_before_key=[&] {
        if(editor_key_index==3||editor_key_index==4)assert(host_audio_paused);
        if(editor_key_index==5||editor_key_index==6)assert(host_audio_active);
        if(editor_key_index==6)assert(ui_rows[36].text.find("Paused:")!=std::string::npos);
    };
    assert(!repl.menu_files());rmb::platform::host_key_repeats.clear();ui_before_key=nullptr;
    skips.clear();keys({0xb7,'p','s','s',0xb1,'p',0xb1});output.clear();
    ui_before_key=[&]{if(editor_key_index>=3&&editor_key_index<=5)assert(host_audio_active);};
    assert(!repl.menu_files());assert(skips.size()==2);
    assert(output.find("second.mp3")!=std::string::npos);
    ui_before_key=nullptr;
    preview_nested=true;skips.clear();
    keys({0xb7,13,'p',0xb1,0xb1,0xb1,0xb1});
    rmb::platform::host_key_repeats={false,false,false,false,true,false,false};
    ui_before_key=[&] {
        if(editor_key_index==3)assert(host_audio_active);
        if(editor_key_index==4||editor_key_index==5){assert(!host_audio_active);assert(ui_rows[6].text=="/music");}
        if(editor_key_index==6)assert(ui_rows[6].text=="/");
    };
    assert(!repl.menu_files());assert(editor_key_index==7&&skips.size()==4);
    preview_nested=false;rmb::platform::host_key_repeats.clear();
    listing={entry("song.wav")};skips.clear();
    keys({0xb7,'p',-1,0xb1});
    ui_before_key=[&] {
        if(editor_key_index==2)host_audio_active=false; // Natural EOF while waiting for a key.
        if(editor_key_index==3)assert(ui_rows[36].text.empty());
    };
    assert(!repl.menu_files());assert(skips.size()==2);
    ui_before_key=nullptr;
    const auto resources = cpb_soak::resources();
    preview_nested=true;
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) {
        skips.clear(); output.clear();
        keys({0xb7,13,'p',0xb1,0xb1,0xb1,0xb1});
        rmb::platform::host_key_repeats={false,false,false,false,true,false,false};
        ui_before_key=[&] {
            if (editor_key_index==4 || editor_key_index==5) {
                assert(!host_audio_active && ui_rows[6].text=="/music");
            }
        };
        const auto stops = host_audio_stop_count;
        assert(!repl.menu_files() && editor_key_index==7 && host_audio_stop_count==stops+1);
        assert(!host_audio_active);
    }
    preview_nested=false;ui_before_key=nullptr;rmb::platform::host_key_repeats.clear();
    cpb_soak::record("files-audio-esc", cpb_soak::iterations(), resources);
    std::puts("Actual Files/picker drawing, themes, scrolling and batching: PASS");
}
