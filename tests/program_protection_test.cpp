#include "language93_test_support.hpp"
#include "input_hotkeys.hpp"
#include <fstream>
#include "soak_support.hpp"

namespace rmb::platform {
void begin_command_input() {}
void end_command_input() {}
bool terminal_console_enabled() { return false; }
void screen_put_char(char c) { output += c; }
void serial_put_char_raw(char) {}
void serial_put_string_raw(const char*) {}
}
namespace rmb::storage { extern std::vector<DirectoryEntry> host_program_listing; }
namespace rmb::psram { void test_set_available(bool); }
static void keys(std::vector<int> input) {
    editor_keys=std::move(input);editor_key_index=0;editor_repeat_count=0;
    rmb::platform::host_key_repeats.clear();ui_before_key=nullptr;
}
static std::string contents(const std::string& name) {
    std::ifstream f(name); return {(std::istreambuf_iterator<char>(f)),{}};
}
static std::string source(rmb::Repl& repl) {
    std::string result;
    for(std::size_t i=0;i<repl.program_.size();++i) {
        std::int32_t number=0;std::size_t length=0;const char* text=nullptr;
        assert(repl.program_.read_line_text(i,number,text,length));result+=text;result+='\n';
    }
    return result;
}
static void command(rmb::Repl& repl,const char* text) {
    std::vector<char> input(text,text+std::strlen(text)+1);repl.process_line(input.data());
}
int main() {
    using namespace rmb;
    char temp[]="/tmp/cpb-program-protection-XXXXXX";assert(mkdtemp(temp));
    const auto resources = cpb_soak::resources();
    cpb_soak::record("program-protection", 0, resources);
    // Each cycle exercises all three backends in both source modes.
    for (unsigned cycle = 0; cycle < 50; ++cycle) {
    for(int backend=0;backend<3;++backend) for(bool structured:{false,true}) {
        psram::test_set_available(backend!=0);
        const auto root=std::string(temp)+"/"+std::to_string(backend)+(structured?"s/":"c/");
        std::filesystem::create_directories(root);
        Repl repl;repl.program_.set_root(root.c_str());
        assert(repl.program_.initialize(backend==2?ProgramStorageMode::SdCard:ProgramStorageMode::InternalRam));
        std::ofstream(root+"BASE.BAS")<<(structured?"PRINT 1\n":"10 PRINT 1\n");
        std::ofstream(root+"OTHER.BAS")<<(structured?"PRINT 9\n":"10 PRINT 9\n");
        const auto baseline=contents(root+"BASE.BAS");
        auto edit=[&]() {
            if(structured) {const char* rows[]={"PRINT 2"};assert(repl.program_.replace_source_rows(0,1,rows,1));}
            else assert(repl.program_.set_line(10,"PRINT 2"));
            assert(repl.program_.is_dirty());
            // Exercise the authoritative store even if the display mirror is stale.
            repl.program_dirty_=false;
        };
        auto prepare=[&]() {
            assert(repl.program_.load("BASE"));repl.set_current_filename(repl.program_.filename(),false);
            edit();assert(repl.program_modified());
        };
        prepare();const auto edited=source(repl);
        auto unchanged=[&](std::uint64_t revision) {
            assert(source(repl)==edited&&repl.program_.revision()==revision);
            assert(repl.program_modified()&&std::string(repl.current_filename_)=="BASE.BAS");
        };
        for(const char* danger:{"NEW","LOAD OTHER"}) {
            prepare();const auto revision=repl.program_.revision();
            keys({0xb1});command(repl,danger);unchanged(revision);
            assert(editor_key_index==1);
        }
        prepare();keys({'d'});command(repl,"LOAD MISSING");
        assert(source(repl)==edited&&repl.program_modified());
        assert(contents(root+"BASE.BAS")==baseline);
        prepare();storage_save_allowed=false;keys({'s',13});command(repl,"NEW");
        assert(source(repl)==edited&&repl.program_modified());
        assert(contents(root+"BASE.BAS")==baseline);
        storage_save_allowed=true;command(repl,"SAVE");
        assert(!repl.program_modified()&&contents(root+"BASE.BAS")!=baseline);
        std::ofstream(root+"BASE.BAS")<<baseline;
        for(const char* danger:{"NEW","LOAD OTHER"}) for(char choice:{'s','d'}) {
            prepare();keys({choice});command(repl,danger);
            assert(!repl.program_modified());
            assert(danger[0]=='N'?repl.program_.size()==0:source(repl)=="PRINT 9\n");
            assert((contents(root+"BASE.BAS")!=baseline)==(choice=='s'));
            std::ofstream(root+"BASE.BAS")<<baseline;
        }
        prepare();std::strcpy(repl.settings_.quick[0].filename,"OTHER.BAS");
        repl.settings_.quick[0].run=false;keys({0xb1});const auto before=repl.program_.revision();
        repl.handle_quick_key(0x81);unchanged(before);
        storage::host_program_listing.resize(1);
        std::strcpy(storage::host_program_listing[0].name,"OTHER.BAS");
        storage::host_program_listing[0].directory=false;
        prepare();keys({13,0xb1,0xb1});bool warning=false,returned=false;
        ui_before_key=[&] {
            for(const auto& row:ui_rows) {
                warning=warning||row.text=="UNSAVED CHANGES";
                if(editor_key_index==2) returned=returned||row.text.find("FILES [")==0;
            }
        };
        assert(!repl.menu_files());ui_before_key=nullptr;assert(warning&&returned);
        assert(source(repl)==edited&&repl.program_modified());
        for(char choice:{'s','d'}) {
            prepare();keys({13,choice});assert(repl.menu_files());
            assert(source(repl)=="PRINT 9\n"&&!repl.program_modified());
            assert((contents(root+"BASE.BAS")!=baseline)==(choice=='s'));
            std::ofstream(root+"BASE.BAS")<<baseline;
        }
        prepare();storage_save_allowed=false;keys({13,'s',13,0xb1});
        assert(!repl.menu_files());assert(source(repl)==edited&&repl.program_modified());
        storage_save_allowed=true;
        // Editor navigation alone is clean, and returning from RUN is not Save.
        assert(repl.program_.load("BASE"));repl.set_current_filename(repl.program_.filename(),false);
        keys({0xb3,0xb2,input_hotkeys::Run});
        FullScreenEditor navigation(repl.program_,repl.current_filename_,sizeof(repl.current_filename_),repl.program_dirty_);
        assert(navigation.run().action==EditorExitAction::Run&&!repl.program_modified());
        // Save -> edit -> Undo to the saved identity clears dirty; Redo restores it.
        keys({0xd5,'2',0x81,'3',0x84,input_hotkeys::Run});
        FullScreenEditor undo(repl.program_,repl.current_filename_,sizeof(repl.current_filename_),repl.program_dirty_);
        assert(undo.run().action==EditorExitAction::Run&&!repl.program_modified());
        keys({0xd5,'3',0x84,0x89,input_hotkeys::Run});
        FullScreenEditor redo(repl.program_,repl.current_filename_,sizeof(repl.current_filename_),repl.program_dirty_);
        assert(redo.run().action==EditorExitAction::Run&&repl.program_modified());
        const auto name=std::string(repl.current_filename_);
        const auto revision=repl.program_.revision();
        output.clear();command(repl,"LASTERROR");assert(output=="No error recorded.\r\n");
        repl.run_direct_line("A=1/0");assert(repl.last_error_.valid);
        const auto error=std::string(repl.last_error_.message);
        output.clear();command(repl,"LASTERROR");
        assert(output.find(error)!=std::string::npos&&output.find("Direct")!=std::string::npos);
        repl.run_direct_line("PRINT 7");output.clear();command(repl,"LASTERROR");
        assert(output.find(error)!=std::string::npos);
        SystemInformation snapshot;repl.collect_system_information(snapshot);
        host_audio_active=true;const auto stops=host_audio_stop_count;
        repl.collect_system_information(snapshot);output.clear();command(repl,"INFO");
        for(const char* label:{"Mode","Program","SD Card","Connection","Playback state","Audio power"})
            assert(snapshot.value(label)&&output.find(snapshot.value(label))!=std::string::npos);
        assert(output.find("N/A")!=std::string::npos);
        assert(!platform::host_keyboard_query&&host_audio_stop_count==stops);
        assert(repl.program_.revision()==revision&&repl.program_modified()&&name==repl.current_filename_);
        platform::audio_stop();
        // A repeated source line / missing delete cannot manufacture dirty.
        assert(repl.program_.new_program(ProgramSourceMode::ClassicNumbered));
        assert(repl.program_.set_line(10,"PRINT 1")&&repl.program_.save("NOOP"));
        repl.set_current_filename(repl.program_.filename(),false);
        const auto saved_revision=repl.program_.revision();
        command(repl,"10 PRINT 1");command(repl,"999");
        if(repl.program_modified()||repl.program_.revision()!=saved_revision)
            std::fprintf(stderr,"No-op failure backend=%d structured=%d dirty=%d revision=%llu -> %llu\n",
                backend,structured,repl.program_modified(),
                static_cast<unsigned long long>(saved_revision),static_cast<unsigned long long>(repl.program_.revision()));
        assert(!repl.program_modified()&&repl.program_.revision()==saved_revision);
        assert(repl.program_.set_line(20,"PRINT 2"));
        std::int32_t number=0;std::size_t length=0;const char* borrowed=nullptr;
        assert(repl.program_.read_line_text(1,number,borrowed,length));
        assert(repl.program_.set_line(10,borrowed));
        assert(source(repl)=="PRINT 2\nPRINT 2\n");
        // The same LASTERROR command works with saved Classic / Structured source.
        assert(repl.program_.new_program(structured?ProgramSourceMode::Structured:ProgramSourceMode::ClassicNumbered));
        if(structured) {const char* rows[]={"PRINT 1/0"};assert(repl.program_.replace_source_rows(0,0,rows,1));}
        else assert(repl.program_.set_line(10,"PRINT 1/0"));
        assert(repl.program_.save("ERROR"));repl.set_current_filename(repl.program_.filename(),false);
        repl.run_program();output.clear();command(repl,"LASTERROR");
        assert(output.find("DIVISION BY ZERO")!=std::string::npos);
        assert(output.find(structured?"Source Row":"BASIC Line")!=std::string::npos);
        assert(!repl.program_modified());
        // Unnamed source can be saved before NEW, or a cancelled Save As kept.
        assert(repl.program_.new_program(ProgramSourceMode::ClassicNumbered));
        repl.set_current_filename("UNTITLED",false);assert(repl.program_.set_line(10,"PRINT 3"));
        keys({'s',0xb1});command(repl,"NEW");assert(repl.program_modified()&&repl.program_.size()==1);
        keys({'s','N','A','M','E','D',13});command(repl,"NEW");
        assert(!repl.program_modified()&&repl.program_.size()==0);
        assert(contents(root+"NAMED.BAS")=="10 PRINT 3\n");
    }
        if ((cycle + 1) % 10 == 0) cpb_soak::record("program-protection", cycle + 1, resources);
    }
    ui_before_key=nullptr;storage::host_program_listing.clear();std::filesystem::remove_all(temp);
    std::puts("Program protection: NEW / LOAD / Files / quick / Save failure / Undo / INFO / LASTERROR PASS");
}
