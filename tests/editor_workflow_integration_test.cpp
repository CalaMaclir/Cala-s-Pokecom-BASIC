// Exercise production REPL, editor, compiler and VM, with host hardware stubs.
#define CPB_REAL_LINE_EDITOR 1
#include "benchmark_platform.hpp"
#include "input_hotkeys.hpp"
#include "firmware_version.hpp"
#include <cstdio>
#include <sstream>

namespace rmb::platform {
void begin_command_input() {}
void end_command_input() {}
bool terminal_console_enabled() { return false; }
void screen_put_char(char c) { output += c; }
void serial_put_char_raw(char) {}
void serial_put_string_raw(const char*) {}
}
namespace rmb::psram { void test_set_available(bool); }
std::string contents(const std::string& name) {
    std::ifstream file(name);return {(std::istreambuf_iterator<char>(file)),{}};
}
void keys(std::vector<int> input) {
    editor_keys=std::move(input);editor_key_index=0;editor_repeat_count=0;
    ui_before_key=nullptr;
}
int main() {
    using namespace rmb;
    using namespace rmb::input_hotkeys;
    char temp[]="/tmp/cpb-editor-workflow-XXXXXX";assert(mkdtemp(temp));
    const std::string root=std::string(temp)+"/";
    for (int backend=0;backend<3;++backend) {
        psram::test_set_available(backend!=0);
        Repl repl;repl.program_.set_root(root.c_str());
        assert(repl.program_.initialize(backend==2?ProgramStorageMode::SdCard:ProgramStorageMode::InternalRam));
        assert(repl.program_.new_program(ProgramSourceMode::Structured));
        const char* rows[]={"PRINT 123"};assert(repl.program_.replace_source_rows(0,0,rows,1));
        assert(repl.program_.save("EDITOR"));repl.set_current_filename("EDITOR.BAS",false);
        const auto file=contents(root+"EDITOR.BAS");
        char filename[80]="EDITOR.BAS";bool dirty=false;
        keys({0xd5,'4',Run});storage_save_allowed=false;
        FullScreenEditor editor(repl.program_,filename,sizeof(filename),dirty);
        const auto result=editor.run();assert(result.action==EditorExitAction::Run);
        assert(dirty&&repl.program_.is_dirty());assert(contents(root+"EDITOR.BAS")==file);
        std::int32_t number=0;std::size_t length=0;const char* text=nullptr;
        assert(repl.program_.read_line_text(0,number,text,length));assert(std::string(text)=="PRINT 1234");
        // Prompt Run uses the same run_program route as the RUN command.
        keys({Run});char input[224]={};output.clear();
        assert(LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==0);
        assert(repl.handle_prompt_special(LineEditor::last_special_key()));
        assert(output.find("1234\r\n")!=std::string::npos);
        char run[]="RUN";output.clear();repl.process_line(run);assert(output.find("1234\r\n")!=std::string::npos);
        repl.program_dirty_=true;
        // Actual REPL editor exit action is consumed only after menu cleanup.
        keys({Editor,0xd5,'5',Run});output.clear();
        assert(LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==0);
        assert(repl.handle_prompt_special(LineEditor::last_special_key()));
        assert(output.find("12345\r\n")!=std::string::npos);
        assert(repl.program_dirty_&&repl.program_.is_dirty());
        assert(contents(root+"EDITOR.BAS")==file);
        storage_save_allowed=true;

        // Real compile error and its existing revision/mode guarded location.
        assert(repl.program_.new_program(ProgramSourceMode::Structured));
        const char* bad[]={"PRINT 1","PRINT ("};assert(repl.program_.replace_source_rows(0,0,bad,2));
        repl.set_current_filename("UNTITLED",false);
        keys({Run});output.clear();char edit[]="EDIT";repl.process_line(edit);
        assert(repl.compile_error_location_==2);
        assert(repl.compile_error_revision_==repl.program_.revision());
        bool focused=false,header=false,help=false;
        keys({0xb1});ui_before_key=[&] {
            focused=focused||ui_rows[1].text.find("ROW:2 ")==0;
            header=header||ui_rows[0].text.find("CPB v" RMB_VERSION)==0;
            help=help||(ui_rows[2].text.size()<=53&&ui_rows[2].text.find("ALT+R RUN")!=std::string::npos);
        };
        assert(repl.handle_prompt_special(Editor));assert(focused&&header&&help);
        ui_before_key=nullptr;
        // Stale locations are discarded by the same EDIT path.
        assert(repl.program_.new_program(ProgramSourceMode::ClassicNumbered));
        assert(repl.program_.set_line(10,"PRINT 7"));repl.set_current_filename("UNTITLED",false);
        keys({Run});output.clear();assert(repl.handle_prompt_special(Editor));
        assert(repl.compile_error_location_==0&&output.find("7\r\n")!=std::string::npos);

        bool menu=false;keys({ControlCenter,0xb1});
        assert(LineEditor::read(input,sizeof(input),nullptr,nullptr,true)==0);
        ui_before_key=[&] {
            for (const auto& row : ui_rows)
                menu=menu||row.text.find("CONTROL CENTER")!=std::string::npos;
        };
        assert(repl.handle_prompt_special(LineEditor::last_special_key()));assert(menu);ui_before_key=nullptr;
        keys({0xb1});char command[]="MENU";repl.process_line(command);
        assert(!repl.handle_prompt_special(Match));
        assert(!repl.handle_prompt_special(Outdent));
        // Model-level Outdent/match also execute against each real backend.
        assert(repl.program_.new_program(ProgramSourceMode::Structured));
        const char* block[]={"    IF 1 THEN","    PRINT 1","END IF"};
        for (std::size_t i=0;i<3;++i) assert(repl.program_.replace_source_rows(i,0,block+i,1));
        ProgramStoreEditorDocument doc(repl.program_);EditorModel model(doc);
        assert(model.begin());assert(model.outdent());assert(model.matching_block());
        assert(model.current_index()==2);assert(model.matching_block()&&model.current_index()==0);

        // Page navigation runs the real System Information menu and preserves
        // source/dirty state. Every required diagnostic fits the LCD body.
        const auto revision = repl.program_.revision();
        const auto source_dirty = repl.program_.is_dirty();
        bool resources=false,hardware=false,uptime=false,owner=false,usb=false;
        bool msc=false,program_backend=false,keyboard_timing=false;
        keys({0xb7,0xb7,0xb7,0xb7,0xb4,13});
        ui_before_key=[&] {
            for (std::size_t row=0;row<ui_rows.size();++row) {
                const auto& text=ui_rows[row].text;
                resources=resources||text.find("SYSTEM INFORMATION 1/")!=std::string::npos;
                hardware=hardware||text.find("SYSTEM INFORMATION 2/")!=std::string::npos;
                uptime=uptime||text.find("Uptime")!=std::string::npos;
                owner=owner||text.find("SD owner")!=std::string::npos;
                usb=usb||text.find("USB device")!=std::string::npos;
                msc=msc||text.find("USB MSC")!=std::string::npos;
                program_backend=program_backend||text.find("Program Storage")!=std::string::npos;
                keyboard_timing=keyboard_timing||text.find("First key ACK")!=std::string::npos;
                if(text.find("First key ACK")!=std::string::npos) assert(row<38);
            }
        };
        repl.menu_system_info();ui_before_key=nullptr;
        assert(resources&&hardware&&uptime&&owner&&usb&&msc&&program_backend&&keyboard_timing);
        assert(repl.program_.revision()==revision&&repl.program_.is_dirty()==source_dirty);
    }
    // One portable hardware sample covers every accepted Stage 3 feature.
    Repl regression; regression.program_.set_root(root.c_str());
    assert(regression.program_.initialize(ProgramStorageMode::InternalRam));
    assert(regression.program_.new_program(ProgramSourceMode::Structured));
    std::istringstream sample(contents("examples/v093/hardware-regression.bas"));
    std::string row;
    while (std::getline(sample,row)) {
        const char* rows[]={row.c_str()};
        assert(regression.program_.replace_source_rows(regression.program_.size(),0,rows,1));
    }
    assert(regression.program_.size()>20);
    for (int run=0;run<2;++run) {
        output.clear();regression.run_program();
        assert(output.find("STAGE3 PASS\r\n")!=std::string::npos);
        assert(output.find("STAGE3 FAIL")==std::string::npos);
    }
    // Suspended storage reports Error, distinct from ESC Exit and Run.
    ProgramStore suspended;suspended.set_root(root.c_str());
    assert(suspended.initialize(ProgramStorageMode::SdCard));assert(suspended.set_dirty(false));
    assert(suspended.suspend_for_usb());char name[80]="UNTITLED";bool dirty=false;
    FullScreenEditor editor(suspended,name,sizeof(name),dirty);
    assert(editor.run().action==EditorExitAction::Error);
    ui_before_key=nullptr;std::filesystem::remove_all(root);
    std::puts("REPL shortcut/editor Run/dirty/no save/error location/backends: PASS");
}
