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
#include "full_screen_editor.hpp"
#include "line_editor.hpp"
#include "storage.hpp"
#include "psram.hpp"

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
rmb::audio::KeyClickMode host_key_click = rmb::audio::KeyClickMode::Low;
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
int host_wav_volume = 50;
int host_play_volume = 100;
int host_audio_service_countdown = -1;
int host_audio_voice_count = 0;
std::size_t host_audio_voice_length[3] = {};
char host_audio_voice_last[3] = {};
int host_audio_stop_count = 0;
char host_audio_error[48] = "OK";

std::vector<int> editor_keys;
std::size_t editor_key_index = 0;
int editor_repeat_count = 0;
bool editor_in_burst = false;
int editor_body_row_draws = 0;
int editor_body_row_draws_in_burst = 0;
int editor_status_row_draws = 0;
int editor_span_draws_in_burst = 0;
int editor_cursor_position_calls = 0;
int editor_cursor_positions_before_burst = 0;
int editor_cursor_visibility_calls = 0;
int editor_last_cursor_row = -1;
bool capture_editor_text=false;
std::vector<std::string> editor_text_rows;
std::vector<std::pair<int, std::string>> editor_screen_rows;
int editor_last_cursor_column = -1;
std::string editor_last_span;
int editor_last_span_columns = 0;

void reset_editor_draw_counters() {
    editor_body_row_draws = 0;
    editor_body_row_draws_in_burst = 0;
    editor_status_row_draws = 0;
    editor_span_draws_in_burst = 0;
    editor_cursor_position_calls = 0;
    editor_cursor_positions_before_burst = 0;
    editor_cursor_visibility_calls = 0;
    editor_last_cursor_row = -1;
}
}
namespace rmb::platform {
void put_char(char c) { output += c; col = c == '\n' || c == '\r' ? 0 : col + 1; }
void put_string(const char* s) { while (*s) put_char(*s++); }
int cursor_column() { return col; }
int cursor_row() { return 3; }
void set_cursor_visible(bool) { ++editor_cursor_visibility_calls; }
void set_cursor_position(int c, int row) {
    col = c;
    editor_last_cursor_row = row;
    editor_last_cursor_column = c;
    ++editor_cursor_position_calls;
}
int text_rows() { return 40; }
int text_columns() { return 53; }
void scroll_text_rows(int) {}
void collect_navigation_burst(
    int key,
    NavigationStepCallback callback,
    void* context
) {
    editor_cursor_positions_before_burst = editor_cursor_position_calls;
    editor_in_burst = true;
    for (int i = 0; i < editor_repeat_count; ++i) {
        host_clock_ms += 25;
        callback(key, context);
    }
    editor_in_burst = false;
}
int get_char() {
    if (editor_key_index == 0) reset_editor_draw_counters();
    if (editor_key_index < editor_keys.size()) {
        return editor_keys[editor_key_index++];
    }
    return 0xb1;
}
void clear_to_eol() {}
ConsoleMode get_console_mode() { return ConsoleMode::Both; }
void set_status_area_enabled(bool) {}
void set_rtc_source(RtcSource) {}
bool set_rtc_address(std::uint8_t) { return true; }
std::uint8_t rtc_address() { return 0x51; }
const char* rtc_source_name() { return "AUTO"; }
bool set_lcd_backlight(std::uint8_t) { return true; }
bool set_cpu_clock_mhz(std::uint32_t) { return true; }
void set_text_color(std::uint32_t,std::uint32_t) {}
void set_console_mode(ConsoleMode) {}
void clear_lcd() {}
void clear_lcd_color(std::uint32_t) {}
bool status_area_enabled() { return true; }
void set_function_key_bar_enabled(bool) {}
bool function_key_bar_enabled() { return true; }
bool shift_held() { return false; }
bool caps_lock_enabled() { return false; }
bool get_battery_status(int&, bool&) { return false; }
InternalKeyboardDiagnostics get_internal_keyboard_diagnostics(bool) {
    return {"OK", "NONE", 0, 0, 0, 0};
}
bool get_datetime(DateTime&) { return false; }
std::uint32_t system_clock_hz() { return 150000000; }
std::uint32_t monotonic_millis() { return host_clock_ms; }
std::uint64_t monotonic_micros() {
    return static_cast<std::uint64_t>(host_clock_ms) * 1000u;
}
void sleep_millis(std::uint32_t ms) { host_clock_ms += ms; ++sleep_calls; }
void draw_text_row(
    int row,
    const char* text,
    std::uint32_t,
    std::uint32_t
) {
    if(capture_editor_text) {
        editor_text_rows.emplace_back(text?text:"");
        editor_screen_rows.emplace_back(row, text?text:"");
    }
    if (row >= 3 && row <= 35) {
        ++editor_body_row_draws;
        if (editor_in_burst) ++editor_body_row_draws_in_burst;
    }
    if (row == 1 || row == 36) ++editor_status_row_draws;
}
void draw_text_span(
    int,
    int,
    const char* text,
    int columns,
    std::uint32_t,
    std::uint32_t
) {
    if (capture_editor_text) {
        editor_last_span = text ? text : "";
        editor_last_span_columns = columns;
    }
    if (editor_in_burst) ++editor_span_draws_in_burst;
}
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
    host_audio_voice_count=count;
    for(int i=0;i<3;++i) {
        host_audio_voice_length[i]=0;
        host_audio_voice_last[i]=0;
    }
    for(int i=0;i<count;++i) {
        host_audio_voice_length[i]=std::strlen(voices[i]);
        if(host_audio_voice_length[i])
            host_audio_voice_last[i]=voices[i][host_audio_voice_length[i]-1];
    }
    host_audio_active=true;
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
void audio_set_play_volume(int percent) { host_play_volume=percent; }
void audio_set_wav_volume(int percent) { host_wav_volume=percent; }
void audio_set_key_click(rmb::audio::KeyClickMode mode) { host_key_click=mode; }
void audio_key_click() {}
int audio_volume() { return host_audio_volume; }
int audio_play_volume() { return host_play_volume; }
int audio_wav_volume() { return host_wav_volume; }
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
bool program_exists(const char*) { return false; }
bool load_program(const char* name, ProgramStore& program) {
    return program.load(name);
}
bool save_screenshot(const char*,int,int,int,int) { return true; }
bool load_image(const char*,int,int) { return true; }
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
namespace rmb::network {
bool init() { return true; }
void shutdown() {}
bool initialized() { return false; }
bool connect(const char*, const char*) { return true; }
bool connected() { return false; }
bool file_server_start() { return true; }
bool file_server_running() { return false; }
}
namespace rmb { std::size_t LineEditor::read(char* b, std::size_t, CommandHistory*, const char*) { b[0]=0; return 0; } }
namespace rmb { std::size_t LineEditor::read(char* b, std::size_t n, CommandHistory* h, const char* s, bool) {return read(b,n,h,s);} }

static rmb::Repl repl;
void workspace_empty() {
    assert(!repl.workspace_busy_);
    assert(repl.active_compiled_ == nullptr);
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
    const std::string expected =
        std::string("RUN...\r\n") + prefix;
    assert(output.rfind(expected,0) == 0);
    workspace_empty();
}

void run_editor_navigation_case(
    rmb::ProgramStore& program,
    std::initializer_list<int> keys,
    int repeats
) {
    char filename[80] = "UNTITLED";
    bool dirty = false;
    // These navigation fixtures represent a loaded, clean program. Keep the
    // store and the editor mirror consistent before testing draw counts.
    assert(program.set_dirty(false));
    editor_keys.assign(keys);
    editor_key_index = 0;
    editor_repeat_count = repeats;
    editor_in_burst = false;
    rmb::FullScreenEditor editor(
        program, filename, sizeof(filename), dirty);
    assert(editor.run());
}

void editor_navigation_render_policy() {
    constexpr int kEscape = 0xb1;
    constexpr int kRight = 0xb7;
    constexpr int kDown = 0xb6;

    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.set_line(10, "ABCDE"));
        run_editor_navigation_case(program, {kRight, kEscape}, 0);
        assert(editor_cursor_positions_before_burst >= 1);
        assert(editor_body_row_draws == 0);
        assert(editor_body_row_draws_in_burst == 0);
        assert(editor_status_row_draws == 2);
        assert(editor_cursor_visibility_calls >= 4);
        assert(editor_last_cursor_row == 3);
    }

    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.set_line(10, "FIRST"));
        assert(program.set_line(20, "SECOND"));
        run_editor_navigation_case(program, {kDown, kEscape}, 0);
        assert(editor_cursor_positions_before_burst >= 1);
        assert(editor_body_row_draws == 0);
        assert(editor_body_row_draws_in_burst == 0);
        assert(editor_last_cursor_row == 4);
    }

    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        for (int i = 1; i <= 40; ++i) {
            assert(program.set_line(i * 10, "PRINT 1"));
        }
        // Initial Down plus 33 repeats crosses the 33-row viewport twice.
        run_editor_navigation_case(program, {kDown, kEscape}, 33);
        assert(editor_cursor_positions_before_burst >= 1);
        assert(editor_body_row_draws_in_burst == 0);
        assert(editor_span_draws_in_burst == 2);
        // The release path performs exactly one 33-row viewport render.
        assert(editor_body_row_draws == 33);
        assert(editor_last_cursor_row == 35);
    }

    // Real editor rendering, wrap boundaries and cursor columns for both modes.
    auto rendered = [](int row, const std::string& text) {
        for (const auto& entry : editor_screen_rows)
            if (entry.first == row && entry.second == text) return true;
        return false;
    };
    capture_editor_text = true;
    for (std::size_t length : {45u, 46u, 47u, 92u, 93u, 138u}) {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.new_program(rmb::ProgramSourceMode::Structured));
        const std::string source = std::string(length - 1, 'A') + 'Z';
        const char* rows[] = {source.c_str(), "", "  PRINT 1"};
        assert(program.replace_source_rows(0, 0, rows, 2));
        assert(program.replace_source_rows(2, 0, rows + 2, 1));
        editor_screen_rows.clear();
        run_editor_navigation_case(program, {0xd5, kEscape}, 0);
        const int wraps = static_cast<int>((length + 45) / 46);
        assert(rendered(3, "00001  " + source.substr(0, 46)));
        for (int subrow = 1; subrow < wraps; ++subrow)
            assert(rendered(3 + subrow, "       " + source.substr(subrow * 46, 46)));
        assert(rendered(3 + wraps, "00002  "));
        assert(rendered(4 + wraps, "00003    PRINT 1"));
        assert(editor_last_cursor_row == 3 + wraps - 1);
        assert(editor_last_cursor_column == (length % 46 == 0
            ? 52 : 7 + static_cast<int>(length % 46)));
        assert(program.size() == 3); // Logical numbers are display-only.
        std::int32_t number; const char* body; std::size_t actual_length;
        assert(program.read_line_text(0, number, body, actual_length));
        assert(std::string(body, actual_length) == source);
        editor_screen_rows.clear();
        run_editor_navigation_case(program, {0xd5, 0xd2, kEscape}, 0);
        assert(editor_last_cursor_row == 3 && editor_last_cursor_column == 7);
    }
    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.set_line(10, "ABCDE"));
        editor_screen_rows.clear();
        run_editor_navigation_case(program, {kRight, kEscape}, 0);
        assert(rendered(3, "         10 ABCDE"));
        assert(editor_last_cursor_column == 13);
    }
    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.new_program(rmb::ProgramSourceMode::Structured));
        const char* row[] = {"PRINT 1"};
        for (int i = 0; i < 40; ++i)
            assert(program.replace_source_rows(program.size(), 0, row, 1));
        run_editor_navigation_case(program, {kDown, kEscape}, 33);
        assert(editor_body_row_draws_in_burst == 0);
        assert(editor_span_draws_in_burst == 2);
        assert(editor_last_span_columns == 7 && editor_last_span == "00035  ");
        assert(editor_last_cursor_column == 7 && editor_last_cursor_row == 35);
    }
    {
        // A viewport starting on a continuation has no repeated logical number.
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.new_program(rmb::ProgramSourceMode::Structured));
        const std::string wrapped = std::string(46, 'A') + 'Z';
        const char* row[] = {wrapped.c_str()};
        for (int i = 0; i < 40; ++i)
            assert(program.replace_source_rows(program.size(), 0, row, 1));
        editor_screen_rows.clear();
        run_editor_navigation_case(program, {kDown, kEscape}, 32);
        assert(rendered(3, "       Z"));
        assert(rendered(4, "00002  " + std::string(46, 'A')));
        assert(editor_last_span_columns == 7 && editor_last_span == "       ");
        assert(editor_body_row_draws_in_burst == 0);
    }
    {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(program.new_program(rmb::ProgramSourceMode::Structured));
        const char* row[] = {"PRINT 1"};
        assert(program.replace_source_rows(0, 0, row, 1));
        // Tab key enters the actual editor path.
        run_editor_navigation_case(program, {0x09, kEscape, 0xb5, 0x0a}, 0);
        std::int32_t number; const char* body; std::size_t length;
        assert(program.read_line_text(0, number, body, length));
        assert(std::string(body, length) == "    PRINT 1");
    }
    capture_editor_text = false;
}

int main(int argc, char** argv) {
    char temporary[]="/tmp/rmb-runtime-XXXXXX";
    assert(mkdtemp(temporary));
    std::string root=std::string(temporary)+"/";
    editor_navigation_render_policy();
    repl.program_.set_root(root.c_str());
    repl.settings_.key_click=rmb::audio::KeyClickMode::High;
    repl.settings_.audio_volume=80;
    repl.settings_.wav_volume=60;
    repl.settings_.play_volume=90;
    repl.save_settings();
    assert(host_config.find("key_click=HIGH\n")!=std::string::npos);
    assert(host_config.find("audio_volume=80\n")!=std::string::npos);
    assert(host_config.find("wav_volume=60\n")!=std::string::npos);
    assert(host_config.find("play_volume=90\n")!=std::string::npos);
    repl.settings_.key_click=rmb::audio::KeyClickMode::Off;
    repl.settings_.audio_volume=0;
    repl.settings_.wav_volume=10;
    repl.settings_.play_volume=0;
    repl.load_settings(); repl.apply_settings();
    assert(repl.settings_.key_click==rmb::audio::KeyClickMode::High);
    assert(host_key_click==rmb::audio::KeyClickMode::High);
    assert(host_audio_volume==80);
    assert(host_wav_volume==60);
    assert(host_play_volume==90);
    // Version 0.85 timbre names migrate to the new LOW volume level.
    host_config="[audio]\nkey_click=CLASSIC\n";
    repl.load_settings(); repl.apply_settings();
    assert(repl.settings_.key_click==rmb::audio::KeyClickMode::Low);
    assert(host_key_click==rmb::audio::KeyClickMode::Low);
    host_config="[ui]\nstatus=on\n"; // Missing setting defaults to LOW.
    repl.load_settings(); repl.apply_settings();
    assert(repl.settings_.key_click==rmb::audio::KeyClickMode::Low);

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

        // FILES dispatches its complete selected value to these APIs.
        std::ofstream(root + "MY PROGRAM.BAS") << "10 PRINT 314\n";
        output.clear();
        assert(repl.load_named_program("MY PROGRAM.BAS", false));
        assert(!std::strcmp(
            repl.current_filename_, "MY PROGRAM.BAS"));
        assert(output.find(
            "LOADING... (/MY PROGRAM.BAS)\r\n"
            "LOADED /MY PROGRAM.BAS\r\n") == 0);
        editor_keys = {0xb1};
        editor_key_index = 0;
        editor_repeat_count = 0;
        repl.open_full_screen_editor();
        assert(!std::strcmp(
            repl.current_filename_, "MY PROGRAM.BAS"));
        output.clear();
        assert(repl.load_named_program("MY PROGRAM.BAS", true));
        assert(output.find(
            "LOADING... (/MY PROGRAM.BAS)\r\n"
            "LOADED /MY PROGRAM.BAS\r\n"
            "RUN...\r\n") == 0);
        assert(output.find("314\r\n[RUN]") != std::string::npos);
        workspace_empty();
    }

    source({"PRINT 42"});
    direct("PRINT 1+2", "3\r\n");
    direct("A=123", "");
    assert(repl.vm_.direct_state_in_psram());
    assert(repl.vm_.direct_state_bytes() >=
        sizeof(rmb::VM::DirectScalar) * rmb::kMaxSymbols);
    assert(rmb::psram::allocation(
        rmb::psram::Client::DirectState).active);
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
    direct("PRINT (", "?EXPECTED EXPRESSION\r\nCompile Error\r\nPRINT (\r\nDIAGNOSTICS: LAST ERROR / S SEND TO USB SERIAL\r\n");
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
    source({"PRINT 42"});
    {
        const auto misses_before = repl.compiled_cache_.misses();
        const auto hits_before = repl.compiled_cache_.hits();
        run("42\r\n[RUN]");
        assert(repl.compiled_cache_.valid());
        assert(repl.compiled_cache_.revision() == repl.program_.revision());
        assert(repl.compiled_cache_.misses() == misses_before + 1);
        assert(rmb::psram::allocation(
            rmb::psram::Client::CompiledCache).active);
        run("42\r\n[RUN]");
        assert(repl.compiled_cache_.hits() == hits_before + 1);

        // Editing the source changes the revision. The next RUN must reject
        // the stale IL, recompile, and republish the cache.
        assert(repl.program_.set_line(10, "PRINT 43"));
        const auto stale_misses = repl.compiled_cache_.misses();
        run("43\r\n[RUN]");
        assert(repl.compiled_cache_.misses() == stale_misses + 1);
        assert(repl.compiled_cache_.revision() == repl.program_.revision());
    }
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
    // Nested entry must not reset, free, or overwrite an active workspace.
    rmb::CompiledProgram sentinel_workspace;
    sentinel_workspace.code_count=7;
    repl.workspace_busy_=true;
    repl.active_compiled_=&sentinel_workspace;
    output.clear(); repl.run_direct_line("A=0");
    assert(output=="?BASIC BUSY\r\n");
    assert(repl.active_compiled_==&sentinel_workspace);
    assert(sentinel_workspace.code_count==7);
    output.clear(); repl.run_program();
    assert(output=="RUN...\r\n?BASIC BUSY\r\n");
    assert(repl.active_compiled_==&sentinel_workspace);
    assert(sentinel_workspace.code_count==7);
    repl.active_compiled_=nullptr;
    repl.workspace_busy_=false;
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
    // Stored source capacity is 1024 x 2047 on PSRAM-enabled hardware in
    // both INTERNAL and SD modes. Direct input remains 191 characters.
    assert(repl.program_.clear());
    assert(repl.program_.line_capacity()==1024);
    assert(repl.program_.line_length_capacity()==2047);
    for (int i=1;i<=1024;++i)
        assert(repl.program_.set_line(i,"REM capacity"));
    assert(!repl.program_.set_line(1025,"REM overflow"));
    run("[RUN]");
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
    // LIST is cooperatively interruptible from the same runtime BREAK path
    // used by RUN. A normal key is preserved/ignored by LIST; BREAK stops it.
    {
        assert(repl.program_.clear());
        assert(repl.program_.set_line(10,"PRINT 1"));
        assert(repl.program_.set_line(20,"PRINT 2"));
        runtime_keys={
            {rmb::platform::RuntimeKeyType::Key,'X'},
            {rmb::platform::RuntimeKeyType::Break,0}
        };
        runtime_key_index=0;
        output.clear();
        repl.list_program();
        assert(output.find("10 PRINT 1\r\n")!=std::string::npos);
        assert(output.find("[LIST BREAK]\r\n")!=std::string::npos);
        assert(output.find("20 PRINT 2\r\n")==std::string::npos);
        runtime_keys.clear();
        runtime_key_index=0;
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

    // Long SD string literals must reach PLAY intact. Version 0.85's parser
    // silently kept only the first 191 characters, which made long scores end
    // early even though the SD source line itself could hold 2047 characters.
    const std::string long_mml =
        "T400O4L32 " + std::string(1000,'C') + "G";
    assert(long_mml.size() > 384);
    std::ofstream(root+"LONGMML.BAS")
        << "10 PLAY \"" << long_mml << "\"\n20 END\n";
    assert(repl.program_.load("LONGMML"));
    output.clear();
    repl.run_program();
    assert(output.find('?')==std::string::npos);
    workspace_empty();
    assert(host_audio_voice_count==1);
    assert(host_audio_voice_length[0]==long_mml.size());
    assert(host_audio_voice_last[0]=='G');
    rmb::platform::audio_stop();

    std::filesystem::copy_file("tests/fixtures/cpb_large_400.bas",root+"LARGE.BAS");
    assert(repl.program_.load("LARGE")); assert(repl.program_.size()==400);
    rmb::BasicCompiler large_compiler; static rmb::CompiledProgram large_il;
    assert(large_compiler.compile(repl.program_,large_il).ok);
    std::printf("400-line fixture: ops=%zu numbers=%zu strings=%zu line-map=%zu\n",
        large_il.code_count,large_il.number_count,large_il.string_used,large_il.line_count);
    output.clear(); repl.run_program();
    assert(output.find('?')==std::string::npos);
    assert(output.find("END OF 400 LINE TEST\r\n[RUN]")!=std::string::npos);
    workspace_empty();
    {
        const auto large_hits = repl.compiled_cache_.hits();
        output.clear(); repl.run_program();
        assert(output.find("END OF 400 LINE TEST")!=std::string::npos);
        assert(repl.compiled_cache_.hits()==large_hits+1);
    }
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

    auto structured_run=[&](const char* source,const char* expected) {
        std::fprintf(stderr,"Structured case: %.48s\n",source);
        std::ofstream(root+"STRUCTURED.BAS")<<source;
        assert(repl.program_.load("STRUCTURED"));
        output.clear();repl.run_program();
        if(output.find(expected)==std::string::npos)std::fprintf(stderr,"Structured source:\n%s\nExpected: %s\nActual: %s\n",source,expected,output.c_str());
        assert(output.find(expected)!=std::string::npos);workspace_empty();
    };
    structured_run("A=75\nIF A>=100 THEN\n PRINT 1\nELSEIF A>=50 THEN\n PRINT 2\nELSE\n PRINT 3\nEND IF\n","2\r\n");
    structured_run("FOR I=1 TO 3\nIF I=2 THEN\nPRINT I\nEND IF\nNEXT I\n","2\r\n");
    structured_run("IF 1 THEN\nA=0\nDO WHILE A<2\nA=A+1\nLOOP\nPRINT A\nEND IF\n","2\r\n");
    structured_run("IF 1 THEN\nIF 0 THEN\nPRINT 1\nELSEIF 1 THEN\nPRINT 4\nELSE\nPRINT 3\nEND IF\nEND IF\n","4\r\n");
    structured_run("IF 1 THEN PRINT 6 ELSE PRINT 7\n","6\r\n");
    for(const char* source:{"IF 1 THEN\nPRINT 1\n","ELSE\n","IF 1 THEN\nELSE\nELSE\nEND IF\n","GOTO 1\n","GOSUB 1\n","ON 1 GOTO 1\n"}) {
        std::ofstream(root+"STRUCTURED.BAS")<<source;assert(repl.program_.load("STRUCTURED"));
        static rmb::CompiledProgram rejected;
        assert(!large_compiler.compile(repl.program_,rejected).ok);
    }

    structured_run("PRINT SQUARE(12)\nFUNCTION SQUARE(X)\n RETURN X*X\nEND FUNCTION\n","144\r\n");
    structured_run("FUNCTION WRAP$(S$)\nRETURN \"[\"+S$+\"]\"\nEND FUNCTION\nPRINT WRAP$(\"CPB\")\n","[CPB]\r\n");
    structured_run("A=100\nFUNCTION TEST(X)\nA=10\nB=X+1\nRETURN A+B\nEND FUNCTION\nPRINT TEST(5)\nPRINT A\n","16\r\n100\r\n");
    structured_run("SCORE=0\nFUNCTION ADD_SCORE(POINT)\nGLOBAL SCORE\nSCORE=SCORE+POINT\nRETURN SCORE\nEND FUNCTION\nPRINT ADD_SCORE(10)\nPRINT ADD_SCORE(20)\nPRINT SCORE\n","10\r\n30\r\n30\r\n");
    structured_run("FUNCTION FACT(N)\nIF N<=1 THEN\nRETURN 1\nEND IF\nRETURN N*FACT(N-1)\nEND FUNCTION\nPRINT FACT(5)\n","120\r\n");
    structured_run("FUNCTION DOUBLE(X)\nRETURN X*2\nEND FUNCTION\nFUNCTION QUAD(X)\nRETURN DOUBLE(DOUBLE(X))\nEND FUNCTION\nPRINT QUAD(3)\n","12\r\n");
    structured_run("FUNCTION SUM(X)\nFOR I=1 TO X\nS=S+I\nNEXT\nRETURN S\nEND FUNCTION\nPRINT SUM(4)\nPRINT SUM(2)\n","10\r\n3\r\n");
    structured_run("FUNCTION EARLY(X)\nFOR I=1 TO 3\nIF I=2 THEN RETURN X+I\nNEXT I\nRETURN 0\nEND FUNCTION\nFOR I=1 TO 2\nPRINT EARLY(I)\nNEXT I\n","3\r\n4\r\n");
    structured_run("FUNCTION GLOBALFOR(X)\nGLOBAL I\nFOR I=1 TO X\nS=S+I\nNEXT I\nRETURN S\nEND FUNCTION\nPRINT GLOBALFOR(4)\nPRINT I\n","10\r\n5\r\n");
    structured_run("FUNCTION ZERO()\nRETURN A+LEN(S$)\nEND FUNCTION\nPRINT ZERO()\n","0\r\n");
    structured_run("FUNCTION R(N)\nIF N<=1 THEN RETURN \"X\"\nRETURN R(N-1)\nEND FUNCTION\nPRINT R(2)\n","?FUNCTION RETURN TYPE MISMATCH"); // numeric return mismatch compile
    structured_run("FUNCTION R$(N)\nIF N<=1 THEN RETURN \"X\"\nRETURN \"[\"+R$(N-1)+\"]\"\nEND FUNCTION\nPRINT R$(5)\n","[[[[X]]]]\r\n");
    structured_run("FUNCTION INF(N)\nRETURN INF(N+1)\nEND FUNCTION\nPRINT INF(1)\n","?FUNCTION CALL DEPTH AT ROW 2");
    structured_run("FUNCTION MISS(X)\nIF X THEN RETURN 1\nEND FUNCTION\nPRINT MISS(0)\n","?FUNCTION RETURN MISSING AT ROW 3");
    structured_run("FUNCTION ERR(X)\nRETURN 1/X\nEND FUNCTION\nPRINT ERR(0)\n","?DIVISION BY ZERO AT ROW 2");
    interrupt_run=true;
    structured_run("FUNCTION WAIT(X)\nDO\nLOOP\nRETURN X\nEND FUNCTION\nPRINT WAIT(1)\n","?BREAK");
    interrupt_run=false;
    structured_run("FUNCTION EIGHT(A,B$,C,D$,E,F$,G,H$)\nRETURN A+C+E+G+LEN(B$)+LEN(D$)+LEN(F$)+LEN(H$)\nEND FUNCTION\nPRINT EIGHT(1,\"a\",2,\"bb\",3,\"ccc\",4,\"dddd\")\n","20\r\n");
    structured_run("NAME$=\"GLOBAL\"\nFUNCTION LOCAL$(X$)\nS$=X$+\"!\"\nRETURN S$+NAME$\nEND FUNCTION\nPRINT LOCAL$(\"A\")\nPRINT NAME$\n","A!\r\nGLOBAL\r\n");
    structured_run("NAME$=\"A\"\nFUNCTION CHANGE$(X$)\nGLOBAL NAME$\nNAME$=NAME$+X$\nRETURN NAME$\nEND FUNCTION\nPRINT CHANGE$(\"B\")\nPRINT NAME$\n","AB\r\nAB\r\n");
    structured_run("FUNCTION DEP(N)\nIF N=1 THEN RETURN 1\nRETURN 1+DEP(N-1)\nEND FUNCTION\nPRINT DEP(16)\n","16\r\n");
    structured_run("A=0\nWHILE A<3\nIF A<1 THEN\nPRINT \"LOW\"\nELSEIF A<2 THEN\nPRINT \"MID\"\nELSEIF A<3 THEN\nPRINT \"HIGH\"\nEND IF\nA=A+1\nWEND\nDO\nA=A+1\nLOOP UNTIL A=5\nPRINT A\n","LOW\r\nMID\r\nHIGH\r\n5\r\n");
    // BREAK happens inside an allocated function frame, after SLEEP services input.
    sleep_calls=0;break_after_sleep_calls=1;
    structured_run("FUNCTION WAIT(X)\nDO\nSLEEP 1\nLOOP\nRETURN X\nEND FUNCTION\nPRINT WAIT(1)\n","IN FUNCTION WAIT");
    assert(output.find("DEPTH 1")!=std::string::npos);break_after_sleep_calls=-1;
    structured_run("FUNCTION OKAY(X)\nRETURN X+1\nEND FUNCTION\nPRINT OKAY(2)\n","3\r\n");
    {
        const auto hits=repl.compiled_cache_.hits();
        output.clear();repl.run_program();assert(output.find("3\r\n")!=std::string::npos);
        assert(repl.compiled_cache_.hits()==hits+1);workspace_empty();
        structured_run("FUNCTION ERR(X)\nRETURN 1/X\nEND FUNCTION\nPRINT ERR(0)\n","?DIVISION BY ZERO AT ROW 2");
        const auto error_hits=repl.compiled_cache_.hits();
        output.clear();repl.run_program();assert(output.find("?DIVISION BY ZERO AT ROW 2")!=std::string::npos);
        assert(output.find("CALLED FROM ROW 4 (DEPTH 1)")!=std::string::npos);
        assert(repl.compiled_cache_.hits()==error_hits+1);workspace_empty();
    }
    // Local symbol totals do not consume the 64 global-symbol pool.
    {
        std::string text="PRINT LEFTFN()+RIGHTFN()\n";
        for(const char* name:{"LEFTFN","RIGHTFN"}) {
            text+=std::string("FUNCTION ")+name+"()\n";
            for(int i=0;i<40;++i)text+="L"+std::to_string(i)+"="+std::to_string(i)+"\n";
            text+="RETURN L39\nEND FUNCTION\n";
        }
        structured_run(text.c_str(),"78\r\n");
        assert(large_compiler.compile(repl.program_,large_il).ok);
        assert(large_il.symbol_count==0&&large_il.function_count==2);
        assert(large_il.functions[0].local_count==40&&large_il.functions[1].local_count==40);
    }
    // Editor-only prompt protection and LIST have no virtual numbers.
    const auto rev=repl.program_.revision();char numbered_attempt[]="10 PRINT 99";repl.process_numbered_line(numbered_attempt);
    assert(repl.program_.revision()==rev);
    output.clear();repl.list_program();assert(output.find("1 FUNCTION")==std::string::npos);
    for(const char* source:{
        "FUNCTION F(X)\nRETURN X\nEND FUNCTION\nPRINT F()\n",
        "FUNCTION F(X)\nRETURN X\nEND FUNCTION\nPRINT F(\"X\")\n",
        "FUNCTION F()\nRETURN 1\nEND FUNCTION\nFUNCTION F$()\nRETURN \"\"\nEND FUNCTION\n",
        "FUNCTION ABS(X)\nRETURN X\nEND FUNCTION\n",
        "FUNCTION STR()\nRETURN 1\nEND FUNCTION\n",
        "FUNCTION F(A,B,C,D,E,F,G,H,I)\nRETURN A\nEND FUNCTION\n",
        "FUNCTION TOOOOOOOOOOOOOOOOOLONG()\nRETURN 1\nEND FUNCTION\n",
        "IF 1 THEN\nFUNCTION F()\nRETURN 1\nEND FUNCTION\nEND IF\n",
        "IF 1 THEN\nELSE\nELSEIF 1 THEN\nEND IF\n",
        "IF 1 THEN\nFOR I=1 TO 2\nELSE\nNEXT I\nEND IF\n",
        "FUNCTION F(A,A)\nRETURN A\nEND FUNCTION\n",
        "FUNCTION F()\nDIM A(2)\nRETURN 0\nEND FUNCTION\n",
        "FUNCTION F()\nA=1\nGLOBAL A\nRETURN A\nEND FUNCTION\n",
        "FUNCTION F()\nRETURN\nEND FUNCTION\n",
        "FUNCTION F()\nPRINT 1\nEND FUNCTION\n",
        "RETURN 1\n",
        "FUNCTION F()\nFUNCTION G()\nRETURN 1\nEND FUNCTION\nEND FUNCTION\n",
        "F=2\nFUNCTION F()\nRETURN 1\nEND FUNCTION\n"
    }) {
        std::ofstream(root+"STRUCTURED.BAS")<<source;assert(repl.program_.load("STRUCTURED"));
        static rmb::CompiledProgram rejected;
        assert(!large_compiler.compile(repl.program_,rejected).ok);
    }
    assert(repl.program_.new_program(rmb::ProgramSourceMode::ClassicNumbered));
    // Shipped acceptance programs run through the same LOAD/compiler/VM path.
    for(const auto& sample:std::vector<std::pair<const char*,const char*>>{
        {"square","144\r\n"},{"wrap","[CPB]\r\n"},{"local_scope","16\r\n100\r\n"},
        {"global_scope","10\r\n30\r\n30\r\n"},{"factorial","120\r\n"},{"block_if","GOOD\r\n"}}) {
        std::ifstream file(std::string("examples/structured/")+sample.first+".bas");assert(file.good());
        const std::string text((std::istreambuf_iterator<char>(file)),{});
        structured_run(text.c_str(),sample.second);
    }
    // Actual Editor event path: Structured Enter and F5 never request a number.
    {
        rmb::ProgramStore p;assert(p.initialize(rmb::ProgramStorageMode::InternalRam));
        assert(p.new_program(rmb::ProgramSourceMode::Structured));
        const char* row[]={"PRINT 1"};assert(p.replace_source_rows(0,0,row,1));
        editor_text_rows.clear();capture_editor_text=true;
        run_editor_navigation_case(p,{0x0a,0xb1,0xb5,0x0a},0);
        assert(p.size()==2);
        run_editor_navigation_case(p,{0x85,0xb1,0xb5,0x0a},0);
        assert(p.size()==3);
        capture_editor_text=false;
        bool mode_shown=false;
        for(const auto& text:editor_text_rows) {
            assert(text.find("Line number:")==std::string::npos);
            assert(text.find("MANUAL NUMBER")==std::string::npos);
            if(text.find("[STRUCTURED]")!=std::string::npos)mode_shown=true;
        }
        assert(mode_shown);
    }
    // RUN compile diagnostics focus the normal EDIT/Control Center editor.
    {
        std::string bad;
        for (int i=0;i<45;++i) bad+="REM before error\n";
        bad+="PRINT (\nPRINT 1\n";
        std::ofstream(root+"STRUCTURED.BAS")<<bad;
        assert(repl.program_.load("STRUCTURED"));
        output.clear();repl.run_program();workspace_empty();
        assert(output.find("AT ROW 46")!=std::string::npos);
        assert(output.find("EDIT: OPEN AT COMPILE ERROR")!=std::string::npos);
        assert(repl.compile_error_location_==46);
        auto open_editor=[&]() {
            editor_text_rows.clear();capture_editor_text=true;
            editor_keys={0xb1,0xb5,0x0a};editor_key_index=0;editor_repeat_count=0;
            repl.open_full_screen_editor();
            capture_editor_text=false;
            workspace_empty();
        };
        auto saw=[&](const std::string& prefix) {
            for(const auto& text:editor_text_rows)
                if(text.rfind(prefix,0)==0)return true;
            return false;
        };
        open_editor();
        assert(saw("ROW:46 Col:1")&&saw("COMPILE ERROR AT ROW 46"));
        assert(!repl.full_screen_editor_active_);
        // Any source edit invalidates the remembered location, even before RUN.
        const char* changed[]={"REM changed"};
        assert(repl.program_.replace_source_rows(0,1,changed,1));
        open_editor();
        assert(saw("ROW:1 Col:1")&&!saw("COMPILE ERROR AT "));
        assert(repl.compile_error_location_==0);
        output.clear();repl.run_program();assert(repl.compile_error_location_==46);
        // Mode/load changes cannot carry a Structured ordinal into Classic.
        assert(repl.program_.new_program(rmb::ProgramSourceMode::ClassicNumbered));
        assert(repl.program_.set_line(10,"PRINT 1"));
        assert(repl.program_.set_line(90000,"PRINT ("));
        open_editor();
        assert(saw("Ln:10 Col:1")&&!saw("COMPILE ERROR AT "));
        output.clear();repl.run_program();workspace_empty();
        assert(repl.compile_error_location_==90000);
        open_editor();
        assert(saw("Ln:90000 Col:1")&&saw("COMPILE ERROR AT LINE 90000"));
        assert(repl.program_.set_line(90000,"END"));
        output.clear();repl.run_program();workspace_empty();
        assert(repl.compile_error_location_==0);
        open_editor();
        assert(saw("Ln:10 Col:1")&&!saw("COMPILE ERROR AT "));
    }
    // Declared function/local limits are separate from global slots.
    for(bool overflow:{false,true}) {
        std::string text;
        for(int f=0;f<(overflow?33:32);++f)
            text+="FUNCTION F"+std::to_string(f)+"()\nRETURN 0\nEND FUNCTION\n";
        std::ofstream(root+"STRUCTURED.BAS")<<text;assert(repl.program_.load("STRUCTURED"));
        static rmb::CompiledProgram limits;
        const auto result=large_compiler.compile(repl.program_,limits);
        assert(result.ok!=overflow);
        if(overflow)assert(std::string(result.message).find("FUNCTION LIMIT")!=std::string::npos);
        text="FUNCTION MANY()\n";
        for(int local=0;local<(overflow?129:128);++local)text+="V"+std::to_string(local)+"=1\n";
        text+="RETURN V0\nEND FUNCTION\n";
        std::ofstream(root+"STRUCTURED.BAS")<<text;assert(repl.program_.load("STRUCTURED"));
        const auto local_result=large_compiler.compile(repl.program_,limits);
        assert(local_result.ok!=overflow);
        if(overflow)assert(std::string(local_result.message).find("FUNCTION LOCAL LIMIT")!=std::string::npos);
    }
    assert(repl.program_.new_program(rmb::ProgramSourceMode::ClassicNumbered));
    assert(repl.program_.set_line(10,"FUNCTION F()"));
    static rmb::CompiledProgram classic_rejected;
    assert(!large_compiler.compile(repl.program_,classic_rejected).ok);
    std::filesystem::remove_all(temporary);
}
