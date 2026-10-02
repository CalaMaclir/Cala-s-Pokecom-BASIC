// Exercises the actual REPL -> compiler -> VM path with host hardware stubs.
#include <cassert>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <array>
#include <functional>
#define private public
#include "repl.hpp"
#undef private
#include "full_screen_editor.hpp"
#include "line_editor.hpp"
#include "storage.hpp"
#include "psram.hpp"

namespace {
struct UiRow { std::string text;std::uint32_t fg=0,bg=0; };
std::array<UiRow,40> ui_rows;
std::function<void()> ui_before_key;
std::string output;
bool interrupt_run = false;
unsigned pixels = 0;
std::uint32_t graphics_hash=2166136261u, current_color=0;
void hash_graphic(std::uint32_t n){graphics_hash^=n;graphics_hash*=16777619u;}
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
    if(ui_before_key)ui_before_key();
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
void clear_lcd() { ui_rows={}; }
void clear_lcd_color(std::uint32_t) { ui_rows={}; }
bool status_area_enabled() { return true; }
void set_function_key_bar_enabled(bool) {}
bool function_key_bar_enabled() { return true; }
bool shift_held() { return false; }
bool caps_lock_enabled() { return false; }
bool get_battery_status(int&, bool&) { return false; }
InternalKeyboardDiagnostics get_internal_keyboard_diagnostics() {
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
    std::uint32_t fg,
    std::uint32_t bg
) {
    assert(row>=0&&row<40);ui_rows[row]={text?text:"",fg,bg};
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
void set_graphics_color(std::uint32_t n) { current_color=n; }
std::uint32_t graphics_color() { return current_color; }
void graphics_clear(std::uint32_t n) { current_color=n; hash_graphic(n); }
void graphics_pixel(int x,int y) { ++pixels; hash_graphic(x);hash_graphic(y);hash_graphic(current_color); }
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
#ifdef RMB_OPTIMIZER_TEST
    extern int stage3_break_polls;
    if(stage3_break_polls>0&&--stage3_break_polls==0)return true;
#endif
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
const char* last_error() { return "OK"; }
bool save_program(const char* name, ProgramStore& program) {
    return storage_save_allowed && program.save(name);
}
bool program_exists(const char*) { return false; }
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
