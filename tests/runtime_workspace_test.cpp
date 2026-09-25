// Exercises the actual REPL -> compiler -> VM path with host hardware stubs.
#include <cassert>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <vector>
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
std::vector<rmb::platform::RuntimeKeyResult> runtime_keys;
std::size_t runtime_key_index = 0;
std::uint32_t host_clock_ms = 0;
int sleep_calls = 0;
int break_after_sleep_calls = -1;
int runtime_reset_count = 0;
bool storage_save_allowed = true;
std::string host_config;
std::string host_config_backup;
rmb::audio::KeyClickMode host_key_click = rmb::audio::KeyClickMode::Classic;
rmb::platform::I2cResult host_i2c_result = rmb::platform::I2cResult::Ok;
std::uint8_t host_i2c_value = 0x42;
int graphics_text_x = -1;
int graphics_text_y = -1;
std::string graphics_text_output;
int graphics_define_calls = 0;
int graphics_palette_calls = 0;
std::vector<std::uint8_t> host_i2c_scan_addresses = {0x51};
bool host_audio_active = false;
bool host_audio_paused = false;
bool host_audio_wav_allowed = true;
int host_audio_volume = 70;
int host_audio_service_countdown = -1;
int host_audio_voice_count = 0;
int host_audio_stop_count = 0;
char host_audio_error[48] = "OK";
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
void set_rtc_source(RtcSource) {}
bool set_rtc_address(std::uint8_t) { return true; }
std::uint8_t rtc_address() { return 0x51; }
const char* rtc_source_name() { return "AUTO"; }
bool set_lcd_backlight(std::uint8_t) { return true; }
bool set_cpu_clock_mhz(std::uint32_t) { return true; }
void set_text_color(std::uint32_t,std::uint32_t) {}
void set_console_mode(ConsoleMode) {}
bool status_area_enabled() { return true; }
void set_function_key_bar_enabled(bool) {}
bool function_key_bar_enabled() { return true; }
bool shift_held() { return false; }
bool caps_lock_enabled() { return false; }
bool get_battery_status(int&, bool&) { return false; }
bool get_datetime(DateTime&) { return false; }
std::uint32_t system_clock_hz() { return 150000000; }
std::uint32_t monotonic_millis() { return host_clock_ms; }
void sleep_millis(std::uint32_t ms) { host_clock_ms += ms; ++sleep_calls; }
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
void graphics_text_locate(int x, int y) { graphics_text_x=x; graphics_text_y=y; }
void graphics_text_print(const char* text) { graphics_text_output += text ? text : ""; }
bool graphics_define(char, const char*) { ++graphics_define_calls; return true; }
void graphics_define_clear() { ++graphics_define_calls; }
bool graphics_palette_rgb(int, int, int, int) { ++graphics_palette_calls; return true; }
bool graphics_palette_rgb24(int, std::uint32_t) { ++graphics_palette_calls; return true; }
void graphics_palette_reset() { ++graphics_palette_calls; }
RuntimeKeyResult poll_runtime_key() {
    if (runtime_key_index < runtime_keys.size()) {
        return runtime_keys[runtime_key_index++];
    }
    return {};
}
RuntimeKeyResult wait_runtime_key() {
    for (;;) {
        RuntimeKeyResult result = poll_runtime_key();
        if (result.type != RuntimeKeyType::None) return result;
        sleep_millis(10);
    }
}
void reset_runtime_input() { ++runtime_reset_count; }
I2cResult external_i2c_read(std::uint8_t address, std::uint8_t reg, std::uint8_t& value) { (void)address; (void)reg; value=host_i2c_value; return host_i2c_result; }
I2cResult external_i2c_write(std::uint8_t address, std::uint8_t reg, std::uint8_t value) { (void)address; (void)reg; (void)value; return host_i2c_result; }
int external_i2c_scan(std::uint8_t* addresses, int capacity) { const int count=static_cast<int>(host_i2c_scan_addresses.size()); for(int i=0;i<count&&i<capacity;++i) addresses[i]=host_i2c_scan_addresses[static_cast<std::size_t>(i)]; return count; }
const char* i2c_result_text(I2cResult result) { return result==I2cResult::Nack?"I2C NACK":result==I2cResult::Timeout?"I2C TIMEOUT":"I2C ERROR"; }
void audio_init() {}
void audio_reconfigure_clock() {}
void audio_service() {
    if (host_audio_service_countdown > 0 &&
        --host_audio_service_countdown == 0) {
        host_audio_active = false;
    }
}
void audio_stop() { ++host_audio_stop_count; host_audio_active=false; host_audio_paused=false; }
void audio_pause() { if(host_audio_active) host_audio_paused=true; }
void audio_resume() { if(host_audio_active) host_audio_paused=false; }
bool audio_playing() { return host_audio_active; }
bool audio_beep(int frequency,int duration) {
    if(frequency<20||frequency>20000) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"BAD BEEP FREQUENCY");
        return false;
    }
    if(duration<1||duration>60000) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"BAD BEEP DURATION");
        return false;
    }
    host_audio_active=true; host_audio_paused=false; return true;
}
bool audio_play_mml(const char* const* voices,int count) {
    if(count<1||count>3||!voices) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"BAD MML");
        return false;
    }
    for(int i=0;i<count;++i) if(!voices[i]||!*voices[i]) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"BAD MML");
        return false;
    }
    host_audio_voice_count=count; host_audio_active=true;
    host_audio_paused=false; return true;
}
bool audio_wavplay(const char* filename) {
    if(!host_audio_wav_allowed) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"SD CARD NOT AVAILABLE");
        return false;
    }
    if(!filename||!*filename) {
        std::snprintf(host_audio_error,sizeof(host_audio_error),"BAD WAV FILENAME");
        return false;
    }
    host_audio_active=true; host_audio_paused=false; return true;
}
void audio_set_volume(int percent) { host_audio_volume=percent; }
void audio_set_key_click(rmb::audio::KeyClickMode mode) { host_key_click=mode; }
void audio_key_click() {}
int audio_volume() { return host_audio_volume; }
const char* audio_last_error() { return host_audio_error; }
bool break_requested() {
    return interrupt_run ||
        (break_after_sleep_calls >= 0 &&
         sleep_calls >= break_after_sleep_calls);
}
}
namespace rmb::storage {
Owner owner() { return Owner::Firmware; }
bool available() { return true; }
bool init() { return true; }
bool try_lock() { return true; }
void unlock() {}
bool card_present() { return true; }
const char* last_error() { return "HOST STORAGE"; }
bool save_program(const char* name, ProgramStore& program) {
    return storage_save_allowed && program.save(name);
}
bool load_program(const char* name, ProgramStore& program) {
    return program.load(name);
}
bool save_screenshot(const char*,int,int,int,int) { return true; }
bool read_root_text(const char* name,char* output,std::size_t capacity) {
    const std::string& value=std::strcmp(name,"RMBASIC.CFG")==0 ? host_config : host_config_backup;
    if(value.empty() || value.size()+1>capacity) return false;
    std::memcpy(output,value.c_str(),value.size()+1); return true;
}
bool write_root_text(const char* name,const char* text) {
    (std::strcmp(name,"RMBASIC.CFG")==0 ? host_config : host_config_backup)=text;
    return true;
}
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
    repl.settings_.key_click=rmb::audio::KeyClickMode::Sharp;
    repl.save_settings();
    assert(host_config.find("key_click=SHARP\n")!=std::string::npos);
    repl.settings_.key_click=rmb::audio::KeyClickMode::Off;
    repl.load_settings(); repl.apply_settings();
    assert(repl.settings_.key_click==rmb::audio::KeyClickMode::Sharp);
    assert(host_key_click==rmb::audio::KeyClickMode::Sharp);
    host_config="[ui]\nstatus=on\n"; // Previous versions did not store Key Click.
    repl.load_settings(); repl.apply_settings();
    assert(repl.settings_.key_click==rmb::audio::KeyClickMode::Classic);

    for (int backend=0; backend<2; ++backend) {
    assert(repl.program_.initialize(backend ? rmb::ProgramStorageMode::SdCard : rmb::ProgramStorageMode::InternalRam));
    repl.vm_.clear_direct_state();

    // Version 0.81 current-file SAVE semantics are identical for RAM and SD
    // ProgramStore backends. ProgramStore supplies the canonical .BAS name.
    {
        std::ofstream(root+"A.BAS") << "10 PRINT 1\n";
        output.clear();
        assert(repl.load_named_program("A", false));
        assert(!std::strcmp(repl.current_filename_, "A.BAS"));
        assert(repl.program_.set_line(10, "PRINT 2"));
        repl.set_current_filename(repl.program_.filename(), true);
        assert(repl.save_current_program());
        assert(!repl.program_dirty_ && !repl.program_.is_dirty());
        assert(!std::strcmp(repl.current_filename_, "A.BAS"));

        assert(repl.program_.set_line(20, "PRINT 3"));
        repl.set_current_filename(repl.program_.filename(), true);
        assert(repl.save_program_as("B"));
        assert(!std::strcmp(repl.current_filename_, "B.BAS"));
        assert(repl.program_.set_line(30, "PRINT 4"));
        repl.set_current_filename(repl.program_.filename(), true);
        assert(repl.save_current_program());
        std::ifstream b(root+"B.BAS");
        const std::string b_text{
            std::istreambuf_iterator<char>(b),
            std::istreambuf_iterator<char>()};
        assert(b_text.find("30 PRINT 4") != std::string::npos);

        const std::string a_before = [&]() {
            std::ifstream a(root+"A.BAS");
            return std::string{
                std::istreambuf_iterator<char>(a),
                std::istreambuf_iterator<char>()};
        }();
        assert(repl.program_.clear());
        repl.set_current_filename("UNTITLED", false);
        assert(!repl.has_current_filename());
        assert(!repl.save_current_program());
        std::ifstream a_after_file(root+"A.BAS");
        const std::string a_after{
            std::istreambuf_iterator<char>(a_after_file),
            std::istreambuf_iterator<char>()};
        assert(a_after == a_before);

        assert(repl.save_program_as("C"));
        assert(!std::strcmp(repl.current_filename_, "C.BAS"));
        assert(repl.program_.set_line(10, "PRINT 5"));
        repl.set_current_filename(repl.program_.filename(), true);
        storage_save_allowed = false;
        assert(!repl.save_current_program());
        assert(repl.program_dirty_ && repl.program_.is_dirty());
        assert(!std::strcmp(repl.current_filename_, "C.BAS"));
        storage_save_allowed = true;
        assert(repl.save_current_program());
    }

    source({"PRINT 42"});
    direct("PRINT 1+2", "3\r\n");
    direct("A=123", "");
    direct("PRINT A", "123\r\n");
    runtime_keys.clear(); runtime_key_index=0;
    direct("PRINT INKEY", "0\r\n");
    runtime_keys={{rmb::platform::RuntimeKeyType::Key,'A'}}; runtime_key_index=0;
    direct("PRINT INKEY", "65\r\n");
    // Version 0.82 hexadecimal literals preserve the existing numeric grammar.
    direct("PRINT &HFF", "255\r\n");
    direct("PRINT &hff", "255\r\n");
    direct("PRINT &Hff+1", "256\r\n");
    direct("PRINT 12.5+1E1", "22.5\r\n");
    output.clear(); repl.run_direct_line("PRINT &H");
    assert(output.find("?")==0);
    output.clear(); repl.run_direct_line("PRINT &HGG");
    assert(output.find("?")==0);

    // I2C SCAN uses the same compiler/VM path in direct and stored modes.
    host_i2c_scan_addresses = {0x20, 0x51};
    direct("I2C SCAN", "I2C0 GP4/GP5 100kHz\r\n20\r\n51\r\n2 DEVICE(S)\r\n");
    source({"PRINT \"I2C TEST\"", "I2C SCAN", "END"});
    run("I2C TEST\r\nI2C0 GP4/GP5 100kHz\r\n20\r\n51\r\n2 DEVICE(S)\r\n[RUN]");
    host_i2c_scan_addresses.clear();
    direct("I2C SCAN", "I2C0 GP4/GP5 100kHz\r\n0 DEVICE(S)\r\n");
    host_i2c_scan_addresses = {0x51};

    // Version 0.82 external-I2C command/function and error coverage.
    host_i2c_result=rmb::platform::I2cResult::Ok; host_i2c_value=66;
    direct("PRINT I2CREAD(&H51,&H02)", "66\r\n");
    direct("I2CWRITE &H20,&H01,&HFF", "");
    output.clear(); repl.run_direct_line("PRINT I2CREAD(7,2)");
    assert(output.find("?BAD I2C ADDRESS")==0);
    host_i2c_result=rmb::platform::I2cResult::Nack;
    output.clear(); repl.run_direct_line("I2CWRITE &H20,1,2");
    assert(output.find("?I2C NACK")==0);
    host_i2c_result=rmb::platform::I2cResult::Timeout;
    output.clear(); repl.run_direct_line("PRINT I2CREAD(&H51,2)");
    assert(output.find("?I2C TIMEOUT")==0);
    host_i2c_result=rmb::platform::I2cResult::Ok;

    // Version 0.83 audio statements remain background operations.
    direct("BEEP 880,100", "");
    assert(host_audio_active);
    direct("PRINT PLAYING()", "1\r\n");
    direct("PLAY STOP", "");
    assert(!host_audio_active);
    output.clear(); repl.run_direct_line("BEEP 10,100");
    assert(output.find("?BAD BEEP FREQUENCY")==0);

    direct("PLAY \"T120O4L8 CDEFGAB>C\"", "");
    assert(host_audio_active && host_audio_voice_count==1);
    direct("PLAY PAUSE", "");
    assert(host_audio_paused && host_audio_active);
    direct("PLAY RESUME", "");
    assert(!host_audio_paused && host_audio_active);
    direct("PLAY \"C\",\"E\",\"G\"", "");
    assert(host_audio_voice_count==3);
    direct("PLAY STOP", "");

    direct("WAVPLAY \"DEMO.WAV\"", "");
    assert(host_audio_active);
    direct("WAVPAUSE", "");
    assert(host_audio_paused);
    direct("WAVRESUME", "");
    assert(!host_audio_paused);
    direct("WAVSTOP", "");
    assert(!host_audio_active);
    host_audio_wav_allowed=false;
    output.clear(); repl.run_direct_line("WAVPLAY \"NO.WAV\"");
    assert(output.find("?SD CARD NOT AVAILABLE")==0);
    host_audio_wav_allowed=true;

    host_audio_active=true;
    host_audio_service_countdown=3;
    direct("PLAY WAIT", "");
    assert(!host_audio_active && host_audio_service_countdown==0);
    host_audio_service_countdown=-1;

    // Pixel GLOCATE and graphics-only GPRINT do not touch the console output.
    graphics_text_x=graphics_text_y=-1;
    graphics_text_output.clear();
    direct("GLOCATE 11,19:GPRINT \"A\",12", "");
    assert(graphics_text_x==11 && graphics_text_y==19);
    assert(graphics_text_output=="A12");
    graphics_define_calls=0;
    direct("GDEF \"A\"=\"183C66667E666600\"", "");
    direct("GDEF \"A\"=\"\"", "");
    direct("GDEF CLEAR", "");
    assert(graphics_define_calls==3);
    graphics_palette_calls=0;
    direct("GPALETTE 1,255,0,0", "");
    direct("GPALETTE 2,&H00FF00", "");
    direct("GPALETTE RESET", "");
    assert(graphics_palette_calls==3);
    output.clear(); repl.run_direct_line("GPALETTE 0,1,2,3");
    assert(output.find("?BAD PALETTE INDEX")==0);

    runtime_keys={{rmb::platform::RuntimeKeyType::Key,0xb5}}; runtime_key_index=0;
    direct("PRINT INKEY", "181\r\n");
    runtime_keys={{rmb::platform::RuntimeKeyType::None,0},
                  {rmb::platform::RuntimeKeyType::None,0},
                  {rmb::platform::RuntimeKeyType::Key,'Q'}};
    runtime_key_index=0;
    source({"K=INKEY", "IF K=0 THEN GOTO 10", "PRINT K"});
    run("81\r\n[RUN]");
    runtime_keys={{rmb::platform::RuntimeKeyType::Break,0}}; runtime_key_index=0;
    output.clear(); repl.run_direct_line("PRINT INKEY");
    assert(output.find("?BREAK IN 10") == 0);
    runtime_keys={{rmb::platform::RuntimeKeyType::None,0},
                  {rmb::platform::RuntimeKeyType::Key,'Z'}};
    runtime_key_index=0; const unsigned pause_pixels=pixels;
    direct("PAUSE", ""); assert(pixels==pause_pixels);
    runtime_keys={{rmb::platform::RuntimeKeyType::Break,0}}; runtime_key_index=0;
    host_audio_active=true; const int pause_break_stops=host_audio_stop_count;
    output.clear(); repl.run_direct_line("PAUSE");
    assert(output.find("?BREAK IN 10") == 0);
    assert(!host_audio_active && host_audio_stop_count==pause_break_stops+1);
    runtime_keys.clear(); runtime_key_index=0;
    host_clock_ms=0; sleep_calls=0; break_after_sleep_calls=-1;
    direct("SLEEP 25", "");
    assert(host_clock_ms==25 && sleep_calls==3);
    sleep_calls=0; direct("SLEEP 0", ""); assert(sleep_calls==0);
    direct("SLEEP -5", ""); assert(sleep_calls==0);
    host_clock_ms=0; sleep_calls=0; break_after_sleep_calls=3;
    output.clear(); repl.run_direct_line("SLEEP 10000");
    assert(output.find("?BREAK IN 10") == 0);
    assert(host_clock_ms==30);
    break_after_sleep_calls=-1;

    // BREAK is the only implicit runtime result that stops background audio.
    host_audio_active=true; const int error_stops=host_audio_stop_count;
    output.clear(); repl.run_direct_line("PRINT 1/0");
    assert(host_audio_active && host_audio_stop_count==error_stops);
    source({"END"}); output.clear(); repl.run_program();
    assert(host_audio_active && host_audio_stop_count==error_stops);
    host_audio_service_countdown=-1; host_clock_ms=0; sleep_calls=0;
    break_after_sleep_calls=1; output.clear(); repl.run_direct_line("PLAY WAIT");
    assert(output.find("?BREAK")==0);
    assert(!host_audio_active && host_audio_stop_count==error_stops+1);
    break_after_sleep_calls=-1;

    source({"PRINT 42"});
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
    // Direct input retains the RAM-compatible boundary and synthetic line 10.
    static rmb::ProgramStore legacy_direct;
    static rmb::CompiledProgram legacy_il, direct_il;
    rmb::BasicCompiler compiler;
    for (const std::string s : {std::string("PRINT 12"), std::string("GOTO 10")}) {
        legacy_direct.clear();assert(legacy_direct.set_line(10,s.c_str()));
        assert(compiler.compile(legacy_direct,legacy_il).ok);
        assert(compiler.compile_direct(s.c_str(),direct_il).ok);
        assert(legacy_il.code_count==direct_il.code_count);
        assert(!std::memcmp(legacy_il.code,direct_il.code,direct_il.code_count*sizeof(rmb::Op)));
        assert(legacy_il.number_count==direct_il.number_count);
        assert(!std::memcmp(legacy_il.number_pool,direct_il.number_pool,direct_il.number_count*sizeof(rmb::BasicNumber)));
        assert(direct_il.lines[0].line==10);
    }
    const std::string too_long_direct(192,'X');
    assert(!legacy_direct.set_line(10,too_long_direct.c_str()));
    assert(!compiler.compile_direct(too_long_direct.c_str(),direct_il).ok);
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
    // LIST, RUN and SAVE all consume the full borrowed SD line.
    const std::string long_tail="END_SENTINEL";
    const std::string runtime_long=
        "REM "+std::string(2047-4-long_tail.size(),'L')+long_tail;
    std::ofstream(root+"LONGREM.BAS")<<"10 "<<runtime_long<<"\n20 PRINT 77\n";
    assert(repl.program_.load("LONGREM"));
    output.clear();repl.list_program();
    assert(output.find(long_tail)!=std::string::npos);
    run("77\r\n[RUN]");
    assert(repl.program_.save("LONGREM2"));
    assert(repl.program_.load("LONGREM2"));
    output.clear();repl.list_program();
    assert(output.find(long_tail)!=std::string::npos);
    run("77\r\n[RUN]");

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
