#pragma once

#include <cstddef>
#include <cstdint>

#include "basic_compiler.hpp"
#include "platform.hpp"
#include "program_store.hpp"
#include "vm.hpp"

namespace rmb {

class Repl {
public:
    void run();

private:
    static constexpr int kQuickKeyCount = 10;
    static constexpr int kFilenameSize = 80;

    struct QuickKey {
        char filename[kFilenameSize] = {};
        bool run = true;
    };

    struct Settings {
        bool status_enabled = true;
        ProgramStorageMode storage_mode = ProgramStorageMode::Auto;
        std::uint8_t backlight = 160;
        std::uint8_t theme = 0;
        std::uint16_t cpu_mhz = 150;
        std::uint32_t console_foreground = 0xffffff;
        std::uint32_t console_background = 0x000000;
        platform::ConsoleMode console_mode = platform::ConsoleMode::Both;
        bool wifi_enabled = false;
        bool wifi_auto_rtc = true;
        int wifi_timezone_minutes = 540;
        char wifi_ntp_server[64] = "pool.ntp.org";
        char wifi_ssid[33] = {};
        char wifi_password[64] = {};
        QuickKey quick[kQuickKeyCount] = {};
    };

    // Persistent saved source. Direct statements must never modify this store.
    ProgramStore program_;
    // Transient compile workspace, leased by RUN or one direct statement.
    // IL, literal pools, symbols and line map have no lifetime beyond execution.
    CompiledProgram compiled_;
    bool workspace_busy_ = false;
    BasicCompiler compiler_;
    // Execution stacks are already shared by VM::run_impl; only direct scalar
    // snapshots persist separately, as required by the existing direct mode.
    VM vm_;

    Settings settings_;
    char current_filename_[kFilenameSize] = "UNTITLED";
    bool program_dirty_ = false;
    std::uint32_t last_run_ms_ = 0;

    void print_banner();
    void print_prompt();
    void process_line(char* line);
    void list_program();
    void run_program();
    void run_direct_line(const char* line);
    void run_autorun();

    void load_settings();
    void save_settings();
    void apply_settings();
    void render_status();
    void command_xmodem(char* argument, bool receive);
    void command_ymodem(char* argument, bool receive);
    void run_xmodem_transfer(const char* filename, bool receive);
    void run_ymodem_transfer(const char* filename, bool receive, bool menu_ui = false);
    void render_function_keys();
    void capture_hotkey_screenshot();
    void ensure_body_cursor();

    void show_system_menu();
    bool menu_files();
    void menu_quick_keys();
    void menu_display();
    void menu_console();
    void menu_datetime();
    void menu_wifi();
    void menu_file_server();
    void menu_file_transfer();
    void menu_usb_storage();
    void menu_sd();
    void menu_program_storage();
    void service_background();
    void print_program_error();
    void menu_power();
    void menu_system_info();

    bool pick_program_file(char* output, std::size_t capacity);
    bool pick_transfer_file(char* output, std::size_t capacity, const char* title);
    bool choose_quick_mode(bool& run);
    bool pick_wifi_network(char* ssid, std::size_t capacity, bool& secure);
    bool sync_rtc_from_network(bool show_result);
    void apply_network_settings();
    void assign_quick_key(int index, const char* filename, bool run);
    void handle_quick_key(int key);
    static int function_key_index(int key);

    bool load_named_program(const char* filename, bool run_after_load);
    void set_current_filename(const char* filename, bool dirty = false);

    void draw_menu_header(const char* title, const char* help);
    void draw_menu_option(int row, const char* text, bool selected);
    void draw_menu_message(int row, const char* text);
    void leave_menu_screen();

    bool prompt_text(const char* prompt, char* output, std::size_t capacity);
    void command_sd(char* argument);
    void command_date(char* argument);
    void command_time(char* argument);
    void command_datetime(char* argument);
    void command_profile(char* argument);
};

} // namespace rmb
