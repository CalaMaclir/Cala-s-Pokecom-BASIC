#pragma once

#include <cstddef>
#include <cstdint>

#include "basic_compiler.hpp"
#include "bluetooth_hid_keyboard_core.hpp"
#include "command_history.hpp"
#include "compiled_cache.hpp"
#include "platform.hpp"
#include "program_store.hpp"
#include "serial_transfer.hpp"
#include "system_controls.hpp"
#include "vm.hpp"

namespace rmb {
namespace storage { struct DirectoryEntry; }

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
        std::uint16_t cpu_mhz = system_controls::safe_boot_cpu_mhz();
        system_controls::BoardLedMode board_led =
            system_controls::BoardLedMode::Off;
        std::uint32_t console_foreground = 0xffffff;
        std::uint32_t console_background = 0x000000;
        platform::ConsoleMode console_mode = platform::ConsoleMode::Both;
        std::uint8_t audio_volume = 70;
        std::uint8_t wav_volume = 50;
        std::uint8_t play_volume = 100;
        audio::KeyClickMode key_click = audio::KeyClickMode::Low;
        bool startup_wav = true;
        bluetooth_hid::KeyboardLayout bluetooth_keyboard_layout =
            bluetooth_hid::KeyboardLayout::Jis;
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
    // CompiledProgram is a transient SRAM working set. It exists only while
    // RUN or one direct statement owns the compile workspace.
    CompiledProgram* active_compiled_ = nullptr;
    bool workspace_busy_ = false;
    BasicCompiler compiler_;
    // Stored-program RUN results may persist in PSRAM between executions.
    CompiledProgramCache compiled_cache_;
    // Execution stacks are already shared by VM::run_impl; only direct scalar
    // snapshots persist separately, as required by the existing direct mode.
    VM vm_;
    CommandHistory command_history_;

    Settings settings_;
    char current_filename_[kFilenameSize] = "UNTITLED";
    bool program_dirty_ = false;
    bool full_screen_editor_active_ = false;
    std::uint32_t last_run_ms_ = 0;
    // Compile diagnostics are only valid for this exact source revision/mode.
    std::uint64_t compile_error_revision_ = 0;
    std::int32_t compile_error_location_ = 0;
    ProgramSourceMode compile_error_mode_ = ProgramSourceMode::ClassicNumbered;

    void print_banner();
    void print_prompt();
    void process_line(char* line);
    void process_numbered_line(char* input);
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
    void run_xmodem_transfer(
        const char* filename,
        bool receive,
        platform::SerialTransferRoute route =
            platform::SerialTransferRoute::Auto
    );
    void run_ymodem_transfer(
        const char* filename,
        bool receive,
        bool menu_ui = false,
        platform::SerialTransferRoute route =
            platform::SerialTransferRoute::Auto
    );
    void render_function_keys();
    void capture_hotkey_screenshot();
    void ensure_body_cursor();

    void show_system_menu();
    void open_full_screen_editor();
    bool menu_files();
    void menu_quick_keys();
    void menu_display();
    void menu_console();
    void menu_datetime();
    void menu_audio();
    void menu_wifi();
    void menu_bluetooth();
    void menu_bluetooth_keyboard();
    void menu_bluetooth_devices();
    void menu_file_server();
    void menu_file_transfer();
    void menu_transfer_performance();
    void menu_usb_storage();
    void menu_sd();
    void menu_firmware();
    bool recover_after_sd_remount();
    void menu_program_storage();
    void service_background();
    void print_program_error();
    void menu_power();
    void menu_board_led();
    void menu_system_info();
    void menu_psram_diagnostics();

    bool pick_program_file(char* output, std::size_t capacity);
    bool pick_path_file(char* output, std::size_t capacity, const char* title, bool programs_only);
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
    bool has_current_filename() const;
    bool save_current_program();
    bool save_program_as(const char* filename);
    void menu_save_program(bool save_as);
    bool confirm_program_overwrite(const char* filename);

    void draw_menu_header(const char* title, const char* help);
    void draw_menu_option(int row, const char* text, bool selected);
    void draw_file_option(int row,const storage::DirectoryEntry* entry,bool selected,
                          bool show_size,const char* empty=nullptr);
    void draw_menu_message(int row, const char* text);
    void leave_menu_screen();

    bool prompt_text(
        const char* prompt,
        char* output,
        std::size_t capacity,
        const char* initial = nullptr
    );
    void command_sd(char* argument);
    void command_date(char* argument);
    void command_time(char* argument);
    void command_datetime(char* argument);
    void command_profile(char* argument);
};

} // namespace rmb
