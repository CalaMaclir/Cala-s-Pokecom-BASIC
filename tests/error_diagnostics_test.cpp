#define CPB_IMAGE_IO_TEST
#include "language93_test_support.hpp"
#include "soak_support.hpp"
namespace rmb { int LineEditor::last_special_key() {return 0;} }
using namespace rmb;
static void structured(Repl& repl,const std::string& text) {
    assert(repl.program_.new_program(ProgramSourceMode::Structured));
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {const char* rows[]={line.c_str()};assert(repl.program_.replace_source_rows(repl.program_.size(),0,rows,1));}
}
static std::string report(Repl& repl) {
    SystemInformation info;repl.collect_system_information(info);assert(!info.overflow);
    std::string result;
    assert(info.render_serial([](const char* s,void* p){*static_cast<std::string*>(p)+=s;return true;},&result));
    return result;
}
static bool editor_at(Repl& repl,const char* expected) {
    editor_text_rows.clear();capture_editor_text=true;
    editor_keys={0xb1,0xb5,0x0a};editor_key_index=0;editor_repeat_count=0;
    repl.open_full_screen_editor();capture_editor_text=false;
    assert(!repl.workspace_busy_&&!repl.active_compiled_&&!repl.full_screen_editor_active_);
    for(const auto& row:editor_text_rows) if(row.rfind(expected,0)==0)return true;
    std::fprintf(stderr,"Missing editor marker: %s; source: %s; saved present: %d\n",expected,repl.program_.filename(),repl.program_.saved_source_present());
    for (const auto& row:editor_text_rows) if(row.rfind("Ln:",0)==0 || row.rfind("ROW:",0)==0 || row.rfind("RUNTIME",0)==0)std::fprintf(stderr,"%s\n",row.c_str());
    return false;
}
int main() {
    char path[]="/tmp/cpb-diagnostics-XXXXXX";assert(mkdtemp(path));const std::string root=std::string(path)+"/";
    Repl repl;repl.program_.set_root(root.c_str());assert(repl.program_.initialize(ProgramStorageMode::InternalRam));
    assert(!repl.last_error_.valid&&report(repl).find("Last Error        : None")!=std::string::npos);
    for(bool optimize:{false,true}) {
        optimizer_test_disabled=!optimize;
        structured(repl,"PRINT Calc(1,0)\nFUNCTION Calc(A,B)\n    RETURN Divide(A,B)\nEND FUNCTION\nFUNCTION Divide(A,B)\n    RETURN A/B\nEND FUNCTION\n");
        output.clear();host_clock_ms=123456;repl.run_program();
        assert(!repl.active_compiled_&&!repl.workspace_busy_);
        const auto& e=repl.last_error_;
        assert(e.valid&&!e.direct&&e.phase==ErrorPhase::Runtime&&e.mode==ProgramSourceMode::Structured);
        assert(e.location==6&&std::string(e.source)=="    RETURN A/B"&&std::string(e.function)=="DIVIDE");
        assert(e.uptime_ms==123456&&e.call_depth==2&&e.trace_count==2&&!e.trace_truncated);
        assert(std::string(e.trace[0].name)=="DIVIDE"&&e.trace[0].caller_row==3);
        assert(std::string(e.trace[1].name)=="CALC"&&e.trace[1].caller_row==1);
        assert(output.find("Runtime Error - Row 6")!=std::string::npos&&output.find("RETURN A/B")!=std::string::npos);
        assert(editor_at(repl,"ROW:6 Col:1")&&editor_at(repl,"RUNTIME ERROR AT ROW 6"));
        auto serial=report(repl);assert(serial.find("DIVIDE")!=std::string::npos&&serial.find("Runtime Error")!=std::string::npos);
        assert(serial.find("RETURN A/B")==std::string::npos);
        // BREAK is a normal stop and success retains Last Error.
        interrupt_run=true;repl.run_program();interrupt_run=false;assert(repl.last_error_.location==6);
        const char* changed[]={"REM changed"};assert(repl.program_.replace_source_rows(0,1,changed,1));
        assert(!repl.last_error_.can_edit(repl.program_,repl.current_filename_)&&editor_at(repl,"ROW:1 Col:1"));
    }
    optimizer_test_disabled=false;
    assert(repl.program_.new_program(ProgramSourceMode::ClassicNumbered));
    assert(repl.program_.set_line(10,"PRINT \"TEST\"")&&repl.program_.set_line(120,"A=1/0"));
    output.clear();repl.run_program();assert(repl.last_error_.location==120&&repl.last_error_.mode==ProgramSourceMode::ClassicNumbered);
    assert(output.find("IN 120")!=std::string::npos&&output.find("Line 120")!=std::string::npos);
    assert(editor_at(repl,"Ln:120 Col:1"));
    assert(repl.program_.save("SAVED_ERROR"));repl.set_current_filename("SAVED_ERROR.BAS",false);repl.run_program();
    assert(editor_at(repl,"Ln:120 Col:1"));
    std::filesystem::remove(root+"SAVED_ERROR.BAS");assert(editor_at(repl,"Ln:10 Col:1"));
    assert(repl.program_.set_line(120,"PRINT 1"));repl.run_program();assert(repl.last_error_.location==120);
    // Compile, Direct, invalid source, and mode change never reuse a stale row.
    structured(repl,"PRINT 1\nPRINT (\n");repl.run_program();
    assert(repl.last_error_.phase==ErrorPhase::Compile&&repl.last_error_.location==2&&editor_at(repl,"ROW:2 Col:1"));
    repl.run_direct_line("A=1/0");assert(repl.last_error_.direct&&repl.last_error_.location==0&&editor_at(repl,"ROW:1 Col:1"));
    repl.run_direct_line("PRINT (");assert(repl.last_error_.phase==ErrorPhase::Compile&&repl.last_error_.direct);
    const auto revision=repl.program_.revision();repl.run_direct_line("PRINT 7");assert(repl.program_.revision()==revision);
    // Snapshot/Serial export after failure does not stop concurrent audio or alter source.
    structured(repl,"A=1/0\n");repl.run_program();host_audio_active=true;host_audio_paused=true;
    const auto stops=host_audio_stop_count;const auto rev=repl.program_.revision();
    std::strcpy(repl.settings_.wifi_profiles[0].password,"NEVER_EXPORT_THIS_PASSWORD");
    output.clear();editor_keys={'s',0xb1};editor_key_index=0;repl.menu_system_info(true);
    assert(output.find("Runtime Error")!=std::string::npos&&output.find("NEVER_EXPORT_THIS_PASSWORD")==std::string::npos);
    assert(host_audio_stop_count==stops&&host_audio_active&&host_audio_paused&&repl.program_.revision()==rev);
    assert(!platform::host_keyboard_query&&!platform::serial_transfer_active());platform::audio_stop();
    // Bounded nested trace and repeat failures replace the same record.
    structured(repl,"PRINT F(8)\nFUNCTION F(N)\nIF N=0 THEN\nRETURN 1/0\nEND IF\nRETURN F(N-1)\nEND FUNCTION\n");
    repl.run_program();assert(repl.last_error_.call_depth==9&&repl.last_error_.trace_count==4&&repl.last_error_.trace_truncated);
    for(unsigned i=0;i<cpb_soak::iterations();++i) {output.clear();repl.run_program();assert(!repl.active_compiled_&&!repl.workspace_busy_&&repl.last_error_.location==4);}
    // Compiled-cache restores and unverified execution preserve precise positions.
    repl.compiled_cache_.invalidate();
    Language93Session session;session.require_compile("A=1/0\n");
    CompiledProgramCache cache;auto restored=std::make_unique<CompiledProgram>();assert(cache.store(session.source.revision(),*session.code));
    assert(cache.restore(session.source.revision(),*restored,ProgramSourceMode::Structured));
    for(bool generic:{false,true})for(bool unverified:{false,true}) {
        vm_test_generic=generic;vm_test_unverified=unverified;const auto result=session.vm->run(*restored);
        assert(!result.ok&&result.source_row==1&&*result.detail);
    }
    vm_test_generic=vm_test_unverified=false;
    // Image I/O errors flow through the same VM/REPL context. The production
    // codec and corrupted-file handling are separately exercised by image-io-test.
    storage::image_load=[](const char*,int,int){return false;};
    const auto resources = cpb_soak::resources();
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) {
        const char* cause = i % 2 ? "TRUNCATED BMP" : "IMAGE FILE NOT FOUND";
        storage::host_storage_error=cause;
        structured(repl,"PRINT 1\nLOADIMAGE \"NONE.BMP\",0,0\n");
        repl.run_program();assert(repl.last_error_.location==2&&std::string(repl.last_error_.message)==cause);
        assert(editor_at(repl,"ROW:2 Col:1"));
        assert(report(repl).find(cause)!=std::string::npos);
        repl.run_direct_line("PRINT 42");assert(!repl.active_compiled_&&!repl.workspace_busy_);
        assert(platform::audio_beep(440,10));platform::audio_stop();
    }
    cpb_soak::record("error-editor-report", cpb_soak::iterations(), resources);
    storage::host_storage_error="OK";
    // Shipped error samples compile and fail safely at the documented location.
    for(const auto& item:{std::make_pair("examples/v094/error-classic.bas",120),std::make_pair("examples/v094/error-function.bas",6)}) {
        session.source.set_root("./");assert(session.source.load(item.first));
        assert(session.compiler.compile(session.source,*session.code).ok);const auto result=session.vm->run(*session.code);
        assert(!result.ok&&(session.source.source_mode()==ProgramSourceMode::Structured?result.source_row:result.source_line)==item.second);
    }
    std::filesystem::remove_all(path);
    std::puts("Error Context / call trace / REPL / Editor / read-only Diagnostics / Serial: PASS");
}
