// Exercises the actual REPL -> compiler -> VM path with host hardware stubs.
#include <cassert>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#define private public
#include "repl.hpp"
#undef private
#include "line_editor.hpp"
#include "storage.hpp"

namespace {
std::string output;
bool interrupt_run = false;
unsigned pixels = 0;
int col = 0;
}
namespace rmb::platform {
void put_char(char c) { output += c; col = c == '\n' || c == '\r' ? 0 : col + 1; }
void put_string(const char* s) { while (*s) put_char(*s++); }
int cursor_column() { return col; }
int cursor_row() { return 3; }
void set_cursor_position(int c, int) { col = c; }
int text_rows() { return 40; }
int text_columns() { return 53; }
void scroll_text_rows(int) {}
void clear_to_eol() {}
void set_status_area_enabled(bool) {}
bool status_area_enabled() { return true; }
void set_function_key_bar_enabled(bool) {}
bool function_key_bar_enabled() { return true; }
bool shift_held() { return false; }
bool caps_lock_enabled() { return false; }
bool get_battery_status(int&, bool&) { return false; }
bool get_datetime(DateTime&) { return false; }
std::uint32_t system_clock_hz() { return 150000000; }
std::uint32_t monotonic_millis() { return 0; }
void draw_text_row(int, const char*, std::uint32_t, std::uint32_t) {}
void set_graphics_color(std::uint32_t) {}
std::uint32_t graphics_color() { return 0; }
void graphics_clear(std::uint32_t) {}
void graphics_pixel(int,int) { ++pixels; }
void graphics_line(int,int,int,int) {}
void graphics_line_to(int,int) {}
void graphics_circle(int,int,int) {}
void graphics_box(int,int,int,int,bool) {}
bool graphics_paint(int,int) { return true; }
void graphics_flush() {}
bool graphics_point_nonblack(int,int) { return false; }
bool break_requested() { return interrupt_run; }
}
namespace rmb::storage {
Owner owner() { return Owner::Firmware; }
bool available() { return true; }
bool init() { return true; }
bool try_lock() { return true; }
void unlock() {}
bool card_present() { return true; }
const char* last_error() { return "HOST STORAGE"; }
bool save_screenshot(const char*,int,int,int,int) { return true; }
}
namespace rmb::network { bool connected() { return false; } }
namespace rmb { std::size_t LineEditor::read(char* b, std::size_t) { b[0]=0; return 0; } }

static rmb::Repl repl;
void workspace_empty() {
    assert(!repl.workspace_busy_);
    assert(repl.compiled_.code_count == 0);
    assert(repl.compiled_.symbol_count == 0);
    assert(repl.compiled_.number_count == 0);
    assert(repl.compiled_.line_count == 0);
    assert(repl.compiled_.string_used == 1);
}
void direct(const char* s, const char* expected) {
    output.clear();
    repl.run_direct_line(s);
    assert(output == expected);
    workspace_empty();
}
void source(std::initializer_list<const char*> lines) {
    assert(repl.program_.clear());
    int number = 10;
    for (auto l : lines) { assert(repl.program_.set_line(number,l)); number += 10; }
}
void run(const char* prefix) {
    output.clear();
    repl.run_program();
    assert(output.rfind(prefix,0) == 0);
    workspace_empty();
}
int main(int argc, char** argv) {
    char temporary[]="/tmp/rmb-runtime-XXXXXX";
    assert(mkdtemp(temporary));
    std::string root=std::string(temporary)+"/";
    repl.program_.set_root(root.c_str());
    for (int backend=0; backend<2; ++backend) {
    assert(repl.program_.initialize(backend ? rmb::ProgramStorageMode::SdCard : rmb::ProgramStorageMode::InternalRam));
    repl.vm_.clear_direct_state();
    source({"PRINT 42"});
    direct("PRINT 1+2", "3\r\n");
    direct("A=123", "");
    direct("PRINT A", "123\r\n");
    direct("S$=\"retained\"", "");
    run("42\r\n[RUN]");
    direct("PRINT A;S$", "123retained\r\n");
    assert(repl.program_.size() == 1);
    rmb::ProgramLine stored; assert(repl.program_.read_line(0,stored));
    assert(!std::strcmp(stored.text,"PRINT 42"));
    direct("PRINT (", "?EXPECTED EXPRESSION\r\n");
    run("42\r\n[RUN]");
    direct("PRINT A", "123\r\n");
    direct("DIM N(2):N(1)=7:PRINT N(1)", "7\r\n");
    output.clear(); repl.run_direct_line("PRINT N(1)"); assert(output[0]=='?');
    direct("DIM T$(2):T$(1)=\"abc\":PRINT T$(1)", "abc\r\n");
    output.clear(); repl.run_direct_line("PRINT T$(1)"); assert(output[0]=='?');
    // Errors do not commit direct scalar changes; HALT alone saves them.
    output.clear(); repl.run_direct_line("A=9:PRINT 1/0"); assert(output[0]=='?');
    direct("PRINT A", "123\r\n");
    source({"A=9", "PRINT 1/0"}); run("?");
    direct("PRINT A", "123\r\n");
    source({"FOR I=1 TO 3", "GOSUB 50", "NEXT I", "END", "PRINT I", "RETURN"});
    run("1\r\n2\r\n3\r\n[RUN]");
    source({"ON 2 GOTO 30,40", "END", "PRINT 1:END", "ON 1 GOSUB 60,70", "END", "PRINT 6:RETURN", "PRINT 7:RETURN"});
    run("6\r\n[RUN]");
    source({"GOTO 10"}); interrupt_run=true; run("?BREAK"); interrupt_run=false;
    direct("PRINT A", "123\r\n");
    source({"PRINT 42"}); run("42\r\n[RUN]");
    source({"DIM N(4096)"}); run("?");
    direct("PRINT 3", "3\r\n");
    source({"DIM T$(512)"}); run("?");
    direct("PRINT 3", "3\r\n");
    direct("SCREEN 320,320:CLS:PAINT 10,10", "");
    // A failed/partial compiler result must not poison a later RUN.
    output.clear(); repl.run_direct_line("THIS IS NOT BASIC");
    assert(output[0]=='?'); workspace_empty();
    source({"FOR I=1 TO 3"}); run("?");
    direct("PRINT A", "123\r\n");
    // Compiler capacity failure after many emitted operations.
    assert(repl.program_.clear());
    for(int i=1;i<=256;++i) assert(repl.program_.set_line(i,"PRINT 1:PRINT 2:PRINT 3:PRINT 4"));
    run("?PROGRAM TOO COMPLEX"); direct("PRINT A", "123\r\n");
    // Nested entry must not reset or overwrite an active workspace.
    repl.workspace_busy_=true; repl.compiled_.code_count=7;
    output.clear(); repl.run_direct_line("A=0");
    assert(output=="?BASIC BUSY\r\n" && repl.compiled_.code_count==7);
    output.clear(); repl.run_program();
    assert(output=="?BASIC BUSY\r\n" && repl.compiled_.code_count==7);
    repl.workspace_busy_=false; repl.compiled_.reset();
    direct("PRINT A", "123\r\n");
    // Direct input retains the original truncation and synthetic line-10 rules.
    static rmb::ProgramStore legacy_direct;
    static rmb::CompiledProgram legacy_il, direct_il;
    rmb::BasicCompiler compiler;
    for (const std::string s : {std::string("PRINT 12"), std::string("GOTO 10"),
                               std::string("REM ")+std::string(220,'X')}) {
        legacy_direct.clear(); legacy_direct.set_line(10,s.c_str());
        assert(compiler.compile(legacy_direct,legacy_il).ok);
        assert(compiler.compile_direct(s.c_str(),direct_il).ok);
        assert(legacy_il.code_count==direct_il.code_count);
        assert(!std::memcmp(legacy_il.code,direct_il.code,direct_il.code_count*sizeof(rmb::Op)));
        assert(legacy_il.number_count==direct_il.number_count);
        assert(!std::memcmp(legacy_il.number_pool,direct_il.number_pool,direct_il.number_count*sizeof(rmb::BasicNumber)));
        assert(direct_il.lines[0].line==10);
    }
    // Stored source capacity remains 256 x 191 characters.
    assert(repl.program_.clear());
    for (int i=1;i<=256;++i) assert(repl.program_.set_line(i,"REM capacity"));
    assert(repl.program_.set_line(257,"REM overflow")==static_cast<bool>(backend)); run("[RUN]");
    for (int arg=1;arg<argc;++arg) {
        std::ifstream file(argv[arg]); assert(file.good());
        assert(repl.program_.clear()); std::string line;
        while(std::getline(file,line)) {
            char* end=nullptr; int n=std::strtol(line.c_str(),&end,10);
            if(n>0) { while(*end==' ') ++end; assert(repl.program_.set_line(n,end)); }
        }
        output.clear(); pixels=0; repl.run_program();
        assert(output.find('?')==std::string::npos);
        workspace_empty();
        direct("PRINT A", "123\r\n");
    }
    std::puts(backend ? "SD RUN/direct workspace regressions passed" : "RAM RUN/direct workspace regressions passed");
    }
    std::filesystem::copy_file("tests/fixtures/cpb_large_400.bas",root+"LARGE.BAS");
    assert(repl.program_.load("LARGE")); assert(repl.program_.size()==400);
    rmb::BasicCompiler large_compiler; static rmb::CompiledProgram large_il;
    assert(large_compiler.compile(repl.program_,large_il).ok);
    std::printf("400-line fixture: ops=%zu numbers=%zu strings=%zu line-map=%zu\n",
        large_il.code_count,large_il.number_count,large_il.string_used,large_il.line_count);
    output.clear(); repl.run_program();
    assert(output.find('?')==std::string::npos);
    assert(output.find("END OF 400 LINE TEST\r\n[RUN]")!=std::string::npos);
    workspace_empty(); assert(repl.compiled_.extra_lines==nullptr);
    assert(repl.program_.save("LARGE2")); assert(repl.program_.load("LARGE2"));
    output.clear(); repl.run_program(); assert(output.find("END OF 400 LINE TEST")!=std::string::npos);
    assert(!repl.program_.switch_mode(rmb::ProgramStorageMode::InternalRam));
    assert(repl.program_.backend_type()==rmb::ProgramBackend::Sd);
    assert(repl.program_.set_line(100,"PRINT \"EDITED\""));
    assert(repl.program_.set_line(105,"A=10"));assert(repl.program_.erase_line(105));
    assert(repl.program_.save("LARGE2"));assert(repl.program_.load("LARGE2"));
    output.clear();repl.run_program();assert(output.find("EDITED")!=std::string::npos);
    assert(output.find('?')==std::string::npos);workspace_empty();
    // Error reporting and recovery must use extended line-map entries too.
    assert(repl.program_.set_line(3880,"PRINT 1/0"));
    output.clear();repl.run_program();
    assert(output.find('?')!=std::string::npos && output.find("3880")!=std::string::npos);
    workspace_empty();direct("PRINT 3","3\r\n");
    assert(repl.program_.load("LARGE"));
    interrupt_run=true;run("?BREAK");interrupt_run=false;
    direct("PRINT 3","3\r\n");output.clear();repl.run_program();
    assert(output.find("END OF 400 LINE TEST")!=std::string::npos);workspace_empty();
    std::filesystem::remove_all(temporary);
}
