#include "repl.hpp"
#include "psram.hpp"
#include "session_notice.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "audio_file_policy.hpp"
#include "basic_compiler.hpp"
#include "bluetooth_manager.hpp"
#include "bluetooth_hid_keyboard.hpp"
#include "bluetooth_hid_ble_keyboard.hpp"
#include "bluetooth_device_registry.hpp"
#include "console_layout.hpp"
#include "line_editor.hpp"
#include "menu_scroll.hpp"
#include "network.hpp"
#include "file_server.hpp"
#include "file_management.hpp"
#include "full_screen_editor.hpp"
#include "platform.hpp"
#include "program_file_guard.hpp"
#include "storage.hpp"
#include "storage_recovery.hpp"
#include "vm.hpp"
#include "serial_transfer.hpp"
#include "transfer_file.hpp"
#include "usb_device.hpp"
#include "usb_msc.hpp"
#include "usb_storage_menu.hpp"
#include "wireless.hpp"
#include "system_controls.hpp"
#include "xmodem.hpp"
#include "ymodem.hpp"

#ifndef RMB_VERSION
#define RMB_VERSION "0.81"
#endif

#ifndef RMB_BUILD_NUMBER
#define RMB_BUILD_NUMBER "local"
#endif

namespace rmb {

namespace {

// Only the foreground REPL starts BASIC. Service/status/screenshot callbacks
// do not compile or run BASIC. Guard that invariant against future reentrancy,
// and reset the shared workspace on every exit (including errors and BREAK).
void handle_runtime_result(const VmResult& result) {
    if (result.interrupted) platform::audio_stop();
}

class SerialTransferLease {
public:
    SerialTransferLease() = default;
    ~SerialTransferLease() { close(); }
    void close() {
        if (!active_) return;
        platform::end_serial_transfer();
        active_ = false;
    }
    SerialTransferLease(const SerialTransferLease&) = delete;
    SerialTransferLease& operator=(const SerialTransferLease&) = delete;
private:
    bool active_ = true;
};

class CompileWorkspaceLease {
public:
    CompileWorkspaceLease(CompiledProgram& compiled, bool& busy)
        : compiled_(compiled), busy_(busy), acquired_(!busy) {
        if (acquired_) { busy_ = true; compiled_.reset(); }
    }
    ~CompileWorkspaceLease() {
        if (acquired_) { compiled_.reset(); busy_ = false; }
    }
    explicit operator bool() const { return acquired_; }
    CompileWorkspaceLease(const CompileWorkspaceLease&) = delete;
    CompileWorkspaceLease& operator=(const CompileWorkspaceLease&) = delete;
private:
    CompiledProgram& compiled_;
    bool& busy_;
    bool acquired_;
};

constexpr std::size_t kInputBufferSize = 224;
constexpr std::size_t kMenuFileCount = 128;
constexpr std::size_t kMenuFilenameSize = 80;

// File pickers are foreground-only and never nested. Keep one shared BSS union
// so the PROGRAMS name list and DIRECTORY metadata never occupy SRAM together.
union MenuFileScratch {
    char names[kMenuFileCount][kMenuFilenameSize];
    storage::DirectoryEntry entries[kMenuFileCount];
};
MenuFileScratch menu_file_scratch = {};

constexpr int kKeyEnter = 0x0a;
constexpr int kKeyCarriageReturn = 0x0d;
constexpr int kKeyEscape = 0xb1;
constexpr int kKeyLeft = 0xb4;
constexpr int kKeyUp = 0xb5;
constexpr int kKeyDown = 0xb6;
constexpr int kKeyRight = 0xb7;
constexpr int kKeyHome = 0xd2;
constexpr int kKeyDelete = 0xd4;

char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

char* skip_spaces(char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

const char* skip_spaces(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

void trim_right(char* text) {
    std::size_t n = std::strlen(text);
    while (n > 0 &&
           (text[n - 1] == ' ' ||
            text[n - 1] == '\t' ||
            text[n - 1] == '\r' ||
            text[n - 1] == '\n')) {
        text[--n] = '\0';
    }
}

bool command_equals(const char* input, const char* command) {
    while (*input == ' ' || *input == '\t') ++input;

    while (*input && *command) {
        if (ascii_upper(*input) != *command) return false;
        ++input;
        ++command;
    }

    if (*command != '\0') return false;

    while (*input == ' ' || *input == '\t') ++input;
    return *input == '\0';
}

bool ci_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (ascii_upper(*a) != ascii_upper(*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

char* command_argument(char* input, const char* command) {
    input = skip_spaces(input);

    char* p = input;
    const char* c = command;

    while (*p && *c && ascii_upper(*p) == *c) {
        ++p;
        ++c;
    }

    if (*c != '\0') return nullptr;
    if (*p != '\0' && *p != ' ' && *p != '\t') return nullptr;

    return skip_spaces(p);
}

bool parse_filename_argument(char* arg, char* out, std::size_t capacity) {
    if (!arg || !out || capacity == 0) return false;

    arg = skip_spaces(arg);
    if (*arg == '\0') return false;

    std::size_t n = 0;

    if (*arg == '"') {
        ++arg;
        while (*arg && *arg != '"') {
            if (n + 1 >= capacity) return false;
            out[n++] = *arg++;
        }
        if (*arg != '"') return false;
        ++arg;
    } else {
        while (*arg && *arg != ' ' && *arg != '\t') {
            if (n + 1 >= capacity) return false;
            out[n++] = *arg++;
        }
    }

    out[n] = '\0';
    arg = skip_spaces(arg);
    return n > 0 && *arg == '\0';
}

bool parse_line_number(
    char* input,
    std::int32_t& number,
    char*& body
) {
    char* p = skip_spaces(input);

    if (*p < '0' || *p > '9') return false;

    std::int64_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > INT32_MAX) return false;
        ++p;
    }

    number = static_cast<std::int32_t>(value);
    body = skip_spaces(p);
    trim_right(body);
    return true;
}

void print_storage_error() {
    platform::put_char('?');
    platform::put_string(storage::last_error());
    platform::put_string("\r\n");
}

bool key_is_enter(int key) {
    return key == kKeyEnter || key == kKeyCarriageReturn;
}

bool handle_menu_scroll_key(
    int key,
    menu_scroll::State& state,
    int count,
    int visible
) {
    menu_scroll::Move direction = menu_scroll::Move::Up;
    if (!menu_scroll::decode_move_key(
            key, platform::shift_held(), direction)) {
        return false;
    }
    (void)menu_scroll::move(state, direction, count, visible);
    return true;
}

int poll_menu_key(std::uint32_t timeout_ms) {
    const std::uint32_t start = platform::monotonic_millis();
    while (static_cast<std::uint32_t>(
               platform::monotonic_millis() - start) < timeout_ms) {
        const auto result = platform::poll_runtime_key();
        if (result.type == platform::RuntimeKeyType::Key) {
            platform::audio_key_click();
            return result.code;
        }
        if (result.type == platform::RuntimeKeyType::Break) {
            return kKeyEscape;
        }
        platform::sleep_millis(10);
    }
    return -1;
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) ||
           (year % 400 == 0);
}

int days_in_month(int year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month - 1];
}

bool valid_datetime(const platform::DateTime& dt) {
    if (dt.year < 2000 || dt.year > 2099) return false;
    if (dt.month < 1 || dt.month > 12) return false;
    if (dt.day < 1 || dt.day > days_in_month(dt.year, dt.month)) {
        return false;
    }
    return dt.hour >= 0 && dt.hour <= 23 &&
           dt.minute >= 0 && dt.minute <= 59 &&
           dt.second >= 0 && dt.second <= 59;
}

bool parse_date_value(const char* text, platform::DateTime& dt) {
    int y = 0;
    int m = 0;
    int d = 0;
    if (!text || std::sscanf(text, "%d-%d-%d", &y, &m, &d) != 3) {
        return false;
    }

    dt.year = y;
    dt.month = m;
    dt.day = d;
    return valid_datetime(dt);
}

bool parse_time_value(const char* text, platform::DateTime& dt) {
    int h = 0;
    int m = 0;
    int s = 0;
    if (!text || std::sscanf(text, "%d:%d:%d", &h, &m, &s) != 3) {
        return false;
    }

    dt.hour = h;
    dt.minute = m;
    dt.second = s;
    return valid_datetime(dt);
}

bool parse_datetime_value(const char* text, platform::DateTime& dt) {
    if (!text || std::strlen(text) != 14) return false;

    for (int i = 0; i < 14; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }

    auto digits = [text](int offset, int count) {
        int value = 0;
        for (int i = 0; i < count; ++i) {
            value = value * 10 + (text[offset + i] - '0');
        }
        return value;
    };

    dt.year = digits(0, 4);
    dt.month = digits(4, 2);
    dt.day = digits(6, 2);
    dt.hour = digits(8, 2);
    dt.minute = digits(10, 2);
    dt.second = digits(12, 2);
    return valid_datetime(dt);
}

const char* console_name(platform::ConsoleMode mode) {
    switch (mode) {
        case platform::ConsoleMode::Lcd: return "LCD";
        case platform::ConsoleMode::Serial: return "SERIAL";
        case platform::ConsoleMode::Both: return "BOTH";
    }
    return "?";
}

const char* theme_name(std::uint8_t theme) {
    switch (theme % 3) {
        case 0: return "BLUE";
        case 1: return "GREEN";
        default: return "AMBER";
    }
}

std::uint32_t theme_status_bg(std::uint8_t theme) {
    switch (theme % 3) {
        case 0: return 0x002040;
        case 1: return 0x003020;
        default: return 0x402000;
    }
}

std::uint32_t theme_status_fg(std::uint8_t theme) {
    switch (theme % 3) {
        case 0: return 0xb0e8ff;
        case 1: return 0xb8ffb8;
        default: return 0xffe0a0;
    }
}

std::uint32_t theme_selected_bg(std::uint8_t theme) {
    switch (theme % 3) {
        case 0: return 0x104878;
        case 1: return 0x185820;
        default: return 0x704000;
    }
}

struct ConsoleThemeEntry {
    const char* name;
    std::uint32_t foreground;
    std::uint32_t background;
};

constexpr ConsoleThemeEntry kConsoleThemes[] = {
    {"CLASSIC",   0xffffff, 0x000000},
    {"GREEN CRT", 0x80ff80, 0x001008},
    {"AMBER CRT", 0xffc060, 0x100800},
    {"ICE BLUE",  0xe0f8ff, 0x001830},
    {"BLUE",      0xffffff, 0x001848},
    {"PAPER",     0x202020, 0xfff0d0},
    {"MONO",      0xc0c0c0, 0x202020}
};

constexpr int kConsoleThemeCount =
    static_cast<int>(sizeof(kConsoleThemes) / sizeof(kConsoleThemes[0]));

int console_theme_index(
    std::uint32_t foreground,
    std::uint32_t background
) {
    foreground &= 0x00ffffffu;
    background &= 0x00ffffffu;

    for (int i = 0; i < kConsoleThemeCount; ++i) {
        if (kConsoleThemes[i].foreground == foreground &&
            kConsoleThemes[i].background == background) {
            return i;
        }
    }
    return -1;
}

const char* console_theme_name(
    std::uint32_t foreground,
    std::uint32_t background
) {
    const int index = console_theme_index(foreground, background);
    return index >= 0 ? kConsoleThemes[index].name : "CUSTOM";
}

void cycle_console_theme(
    std::uint32_t& foreground,
    std::uint32_t& background,
    int direction
) {
    int index = console_theme_index(foreground, background);

    if (index < 0) {
        index = direction < 0 ? kConsoleThemeCount - 1 : 0;
    } else {
        index += direction < 0 ? -1 : 1;
        if (index < 0) index = kConsoleThemeCount - 1;
        if (index >= kConsoleThemeCount) index = 0;
    }

    foreground = kConsoleThemes[index].foreground;
    background = kConsoleThemes[index].background;
}

std::uint32_t parse_rgb_setting(const char* text, std::uint32_t fallback) {
    if (!text || !*text) return fallback;
    if (*text == '#') ++text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (!end || *end != '\0') return fallback;
    return static_cast<std::uint32_t>(value & 0x00ffffffu);
}

void sort_file_names(
    char files[][kMenuFilenameSize],
    std::size_t count
) {
    char temp[kMenuFilenameSize] = {};

    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (std::strcmp(files[j], files[i]) < 0) {
                std::snprintf(temp, sizeof(temp), "%s", files[i]);
                std::snprintf(files[i], kMenuFilenameSize, "%s", files[j]);
                std::snprintf(files[j], kMenuFilenameSize, "%s", temp);
            }
        }
    }
}

void sort_directory_entries(
    storage::DirectoryEntry* entries,
    std::size_t count
) {
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (std::strcmp(entries[j].name, entries[i].name) < 0) {
                const storage::DirectoryEntry temp = entries[i];
                entries[i] = entries[j];
                entries[j] = temp;
            }
        }
    }
}

} // namespace

void Repl::set_current_filename(const char* filename, bool dirty) {
    std::snprintf(
        current_filename_,
        sizeof(current_filename_),
        "%s",
        filename && *filename ? filename : "UNTITLED"
    );
    program_dirty_ = dirty;
    program_.set_dirty(dirty);
}

bool Repl::has_current_filename() const {
    const char* filename = program_.filename();
    return filename && *filename && !ci_equal(filename, "UNTITLED");
}

bool Repl::save_program_as(const char* filename) {
    if (!filename || !*filename) return false;
    if (!storage::save_program(filename, program_)) return false;
    // ProgramStore owns filename normalization. Keep the status display and
    // the next argument-free SAVE tied to that same canonical name.
    set_current_filename(program_.filename(), false);
    return true;
}

bool Repl::save_current_program() {
    if (!has_current_filename()) return false;
    char filename[kFilenameSize] = {};
    std::snprintf(filename, sizeof(filename), "%s", program_.filename());
    return save_program_as(filename);
}

void Repl::load_settings() {
    settings_ = Settings{};

    char text[3072] = {};
    bool loaded = storage::read_root_text(
        "RMBASIC.CFG",
        text,
        sizeof(text)
    );

    // A truncated FAT write can still produce a readable but empty/partial
    // file. Treat a config without its [ui] section as invalid and fall back
    // to the last known-good backup when available.
    if (!loaded || std::strstr(text, "[ui]") == nullptr) {
        text[0] = '\0';
        loaded = storage::read_root_text(
            "RMBASIC.BAK",
            text,
            sizeof(text)
        );
    }

    if (!loaded || std::strstr(text, "[ui]") == nullptr) {
        return;
    }

    char* cursor = text;
    while (*cursor) {
        char* line = cursor;

        while (*cursor && *cursor != '\r' && *cursor != '\n') {
            ++cursor;
        }

        if (*cursor) {
            *cursor++ = '\0';
            while (*cursor == '\r' || *cursor == '\n') {
                ++cursor;
            }
        }

        line = skip_spaces(line);
        trim_right(line);

        if (*line == '\0' ||
            *line == '#' ||
            *line == ';' ||
            *line == '[') {
            continue;
        }

        char* eq = std::strchr(line, '=');
        if (!eq) continue;

        *eq++ = '\0';
        trim_right(line);
        eq = skip_spaces(eq);
        trim_right(eq);

        if (ci_equal(line, "program_storage")) {
            if (ci_equal(eq,"SD")) settings_.storage_mode=ProgramStorageMode::SdCard;
            else if (ci_equal(eq,"RAM")) settings_.storage_mode=ProgramStorageMode::InternalRam;
            else settings_.storage_mode=ProgramStorageMode::Auto;
            continue;
        }
        if (ci_equal(line, "status")) {
            settings_.status_enabled =
                ci_equal(eq, "on") ||
                ci_equal(eq, "yes") ||
                ci_equal(eq, "true") ||
                std::strcmp(eq, "1") == 0;
            continue;
        }

        if (ci_equal(line, "backlight")) {
            int value = std::atoi(eq);
            if (value < 16) value = 16;
            if (value > 255) value = 255;
            settings_.backlight = static_cast<std::uint8_t>(value);
            continue;
        }

        if (ci_equal(line, "cpu_mhz")) {
            // CPU profiles are session-only. Ignore legacy persisted values so
            // every reboot has a guaranteed 150 MHz recovery path.
            settings_.cpu_mhz = system_controls::safe_boot_cpu_mhz(
                static_cast<std::uint32_t>(std::strtoul(eq, nullptr, 10)));
            continue;
        }

        if (ci_equal(line, "board_led")) {
            settings_.board_led =
                system_controls::board_led_mode_from_setting(eq);
            continue;
        }

        if (ci_equal(line, "theme")) {
            int value = std::atoi(eq);
            if (value < 0) value = 0;
            settings_.theme = static_cast<std::uint8_t>(value % 3);
            continue;
        }

        if (ci_equal(line, "console_fg")) {
            settings_.console_foreground =
                parse_rgb_setting(eq, settings_.console_foreground);
            continue;
        }

        if (ci_equal(line, "console_bg")) {
            settings_.console_background =
                parse_rgb_setting(eq, settings_.console_background);
            continue;
        }

        if (ci_equal(line, "console")) {
            if (ci_equal(eq, "lcd")) {
                settings_.console_mode = platform::ConsoleMode::Lcd;
            } else if (ci_equal(eq, "serial")) {
                settings_.console_mode = platform::ConsoleMode::Serial;
            } else {
                settings_.console_mode = platform::ConsoleMode::Both;
            }
            continue;
        }

        if (ci_equal(line, "rtc_source")) { if(ci_equal(eq,"EXTERNAL"))platform::set_rtc_source(platform::RtcSource::External);else if(ci_equal(eq,"INTERNAL"))platform::set_rtc_source(platform::RtcSource::Internal);else if(ci_equal(eq,"OFF"))platform::set_rtc_source(platform::RtcSource::Off);else platform::set_rtc_source(platform::RtcSource::Auto);continue; }
        if (ci_equal(line, "rtc_address")) { char* end=nullptr;long v=std::strtol(eq,&end,0);if(end!=eq)platform::set_rtc_address(static_cast<std::uint8_t>(v));continue; }
        if (ci_equal(line, "audio_volume")) {
            int value = std::atoi(eq);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            settings_.audio_volume = static_cast<std::uint8_t>(value);
            continue;
        }
        if (ci_equal(line, "wav_volume")) {
            int value = std::atoi(eq);
            value = std::max(10, std::min(100, value));
            value = ((value + 5) / 10) * 10;
            settings_.wav_volume = static_cast<std::uint8_t>(value);
            continue;
        }
        if (ci_equal(line, "play_volume")) {
            int value = std::atoi(eq);
            value = std::max(0, std::min(100, value));
            value = ((value + 5) / 10) * 10;
            settings_.play_volume = static_cast<std::uint8_t>(value);
            continue;
        }
        if (ci_equal(line, "key_click")) {
            settings_.key_click = audio::key_click_from_setting(eq);
            continue;
        }
        if (ci_equal(line, "startup_wav")) {
            settings_.startup_wav =
                ci_equal(eq, "on") || ci_equal(eq, "yes") ||
                ci_equal(eq, "true") || std::strcmp(eq, "1") == 0;
            continue;
        }
        if (ci_equal(line, "keyboard_layout")) {
            settings_.bluetooth_keyboard_layout = ci_equal(eq, "US")
                ? bluetooth_hid::KeyboardLayout::Us
                : bluetooth_hid::KeyboardLayout::Jis;
            continue;
        }

        if (ci_equal(line, "wifi_enabled")) {
            // Wi-Fi is intentionally session-only. Always start disconnected
            // after boot even if an older configuration persisted "on".
            settings_.wifi_enabled = false;
            continue;
        }

        if (ci_equal(line, "wifi_auto_rtc")) {
            settings_.wifi_auto_rtc =
                ci_equal(eq, "on") ||
                ci_equal(eq, "yes") ||
                ci_equal(eq, "true") ||
                std::strcmp(eq, "1") == 0;
            continue;
        }

        if (ci_equal(line, "wifi_timezone_minutes")) {
            int value = std::atoi(eq);
            if (value < -720) value = -720;
            if (value > 840) value = 840;
            settings_.wifi_timezone_minutes = value;
            continue;
        }

        if (ci_equal(line, "wifi_ntp_server")) {
            std::snprintf(
                settings_.wifi_ntp_server,
                sizeof(settings_.wifi_ntp_server),
                "%s",
                *eq ? eq : "pool.ntp.org"
            );
            continue;
        }

        if (ci_equal(line, "wifi_ssid")) {
            std::snprintf(
                settings_.wifi_ssid,
                sizeof(settings_.wifi_ssid),
                "%s",
                eq
            );
            continue;
        }

        if (ci_equal(line, "wifi_password")) {
            std::snprintf(
                settings_.wifi_password,
                sizeof(settings_.wifi_password),
                "%s",
                eq
            );
            continue;
        }

        if (ascii_upper(line[0]) == 'F' &&
            std::isdigit(static_cast<unsigned char>(line[1]))) {
            const int index = std::atoi(line + 1) - 1;
            if (index < 0 || index >= kQuickKeyCount) continue;

            char* comma = std::strchr(eq, ',');
            if (comma) {
                *comma++ = '\0';
                comma = skip_spaces(comma);
                trim_right(comma);
            }

            trim_right(eq);
            std::snprintf(
                settings_.quick[index].filename,
                sizeof(settings_.quick[index].filename),
                "%s",
                eq
            );
            settings_.quick[index].run =
                !comma || ci_equal(comma, "run");
        }
    }
}

void Repl::save_settings() {
    if (!storage::available()) return;

    char text[3072] = {};
    std::size_t used = 0;

    int n = std::snprintf(
        text,
        sizeof(text),
        "[ui]\n"
        "program_storage=%s\n"
        "status=%s\n"
        "backlight=%u\n"
        "board_led=%s\n"
        "theme=%u\n"
        "console_fg=%06lX\n"
        "console_bg=%06lX\n"
        "console=%s\n"
        "\n[rtc]\n"
        "rtc_source=%s\n"
        "rtc_address=0x%02X\n"
        "\n[audio]\n"
        "audio_volume=%u\n"
        "wav_volume=%u\n"
        "play_volume=%u\n"
        "key_click=%s\n"
        "startup_wav=%s\n"
        "\n[bluetooth]\n"
        "keyboard_layout=%s\n"
        "\n[wifi]\n"
        "wifi_enabled=%s\n"
        "wifi_ssid=%s\n"
        "wifi_password=%s\n"
        "wifi_auto_rtc=%s\n"
        "wifi_timezone_minutes=%d\n"
        "wifi_ntp_server=%s\n"
        "\n[fkeys]\n",
        settings_.storage_mode == ProgramStorageMode::SdCard ? "SD" :
            settings_.storage_mode == ProgramStorageMode::InternalRam ? "RAM" : "AUTO",
        settings_.status_enabled ? "on" : "off",
        static_cast<unsigned>(settings_.backlight),
        system_controls::board_led_mode_setting(settings_.board_led),
        static_cast<unsigned>(settings_.theme),
        static_cast<unsigned long>(settings_.console_foreground),
        static_cast<unsigned long>(settings_.console_background),
        console_name(settings_.console_mode),
        platform::rtc_source_name(),
        static_cast<unsigned>(platform::rtc_address()),
        static_cast<unsigned>(settings_.audio_volume),
        static_cast<unsigned>(settings_.wav_volume),
        static_cast<unsigned>(settings_.play_volume),
        audio::key_click_name(settings_.key_click),
        settings_.startup_wav ? "on" : "off",
        bluetooth_hid::keyboard_layout_name(
            settings_.bluetooth_keyboard_layout),
        "off",
        settings_.wifi_ssid,
        settings_.wifi_password,
        settings_.wifi_auto_rtc ? "on" : "off",
        settings_.wifi_timezone_minutes,
        settings_.wifi_ntp_server
    );

    if (n < 0) return;
    used = static_cast<std::size_t>(n);
    if (used >= sizeof(text)) return;

    for (int i = 0; i < kQuickKeyCount; ++i) {
        const QuickKey& key = settings_.quick[i];
        if (key.filename[0] == '\0') continue;

        n = std::snprintf(
            text + used,
            sizeof(text) - used,
            "F%d=%s,%s\n",
            i + 1,
            key.filename,
            key.run ? "RUN" : "LOAD"
        );

        if (n < 0) return;
        const std::size_t wrote = static_cast<std::size_t>(n);
        if (wrote >= sizeof(text) - used) return;
        used += wrote;
    }

    // Preserve the previous valid config before replacing it. This gives the
    // next boot a recovery source if power loss or an unstable experiment
    // interrupts a FAT write.
    char previous[3072] = {};
    const bool have_previous =
        storage::read_root_text(
            "RMBASIC.CFG",
            previous,
            sizeof(previous)
        ) &&
        std::strstr(previous, "[ui]") != nullptr;

    if (have_previous) {
        storage::write_root_text("RMBASIC.BAK", previous);
    }

    if (!storage::write_root_text("RMBASIC.CFG", text) &&
        have_previous) {
        storage::write_root_text("RMBASIC.CFG", previous);
    }
}

void Repl::apply_settings() {
    platform::set_status_area_enabled(settings_.status_enabled);
    platform::set_lcd_backlight(settings_.backlight);
    if (!platform::set_cpu_clock_mhz(settings_.cpu_mhz)) {
        settings_.cpu_mhz = 150;
        platform::set_cpu_clock_mhz(settings_.cpu_mhz);
    }
    platform::set_text_color(
        settings_.console_foreground,
        settings_.console_background
    );
    platform::set_console_mode(settings_.console_mode);
    platform::audio_set_volume(settings_.audio_volume);
    platform::audio_set_wav_volume(settings_.wav_volume);
    platform::audio_set_play_volume(settings_.play_volume);
    platform::audio_set_key_click(settings_.key_click);
    if (!wireless::set_board_led_mode(settings_.board_led)) {
        settings_.board_led = system_controls::BoardLedMode::Off;
    }
    bluetooth_hid::set_layout(settings_.bluetooth_keyboard_layout);
}

void Repl::render_status() {
    platform::set_status_area_enabled(settings_.status_enabled);
    if (!settings_.status_enabled) return;

    const storage::Owner sd_owner = storage::owner();
    const bool firmware_owner = sd_owner == storage::Owner::Firmware;
    const bool present = firmware_owner && storage::card_present();
    const bool mounted = firmware_owner && storage::available();
    if (firmware_owner) program_.ready();
    const char* sd =
        (sd_owner == storage::Owner::UsbHost || sd_owner == storage::Owner::Transition)
            ? "USB"
            : (sd_owner == storage::Owner::Unavailable || program_.suspended())
                ? "ERR"
                : present ? (mounted ? "OK" : "ERR") : "NO";

    int battery = -1;
    bool charging = false;
    const bool battery_ok =
        platform::get_battery_status(battery, charging);

    char filename[24] = {};
    std::snprintf(
        filename,
        sizeof(filename),
        "%.*s%s",
        program_dirty_ ? 18 : 19,
        current_filename_,
        program_dirty_ ? "*" : ""
    );

    char line1[80] = {};
    if (battery_ok) {
        std::snprintf(
            line1,
            sizeof(line1),
            "CPB v%s %-19.19s SD:%-3s BAT:%3d%%%c",
            RMB_VERSION,
            filename,
            sd,
            battery,
            charging ? '+' : ' '
        );
    } else {
        std::snprintf(
            line1,
            sizeof(line1),
            "CPB v%s %-19.19s SD:%-3s BAT: --",
            RMB_VERSION,
            filename,
            sd
        );
    }

    char run_text[20] = {};
    if (last_run_ms_ == 0) {
        std::snprintf(run_text, sizeof(run_text), "RUN:--");
    } else {
        const std::uint32_t minutes = last_run_ms_ / 60000u;
        const std::uint32_t seconds = (last_run_ms_ / 1000u) % 60u;
        if (minutes > 0) {
            std::snprintf(
                run_text,
                sizeof(run_text),
                "RUN:%lu:%02lu.%03lus",
                static_cast<unsigned long>(minutes),
                static_cast<unsigned long>(seconds),
                static_cast<unsigned long>(last_run_ms_ % 1000u)
            );
        } else {
            std::snprintf(
                run_text,
                sizeof(run_text),
                "RUN:%lu.%03lus",
                static_cast<unsigned long>(seconds),
                static_cast<unsigned long>(last_run_ms_ % 1000u)
            );
        }
    }

    platform::DateTime dt;
    char line2[80] = {};
    if (platform::get_datetime(dt)) {
        std::snprintf(
            line2,
            sizeof(line2),
            "%04d-%02d-%02d %02d:%02d:%02d WiFi:%c CAPS:%c",
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute,
            dt.second,
            network::connected() ? '+' : (settings_.wifi_enabled ? '*' : '-'),
            platform::caps_lock_enabled() ? 'A' : 'a'
        );
    } else {
        std::snprintf(
            line2,
            sizeof(line2),
            "---- -- -- --:--:-- WiFi:%c CAPS:%c",
            network::connected() ? '+' : (settings_.wifi_enabled ? '*' : '-'),
            platform::caps_lock_enabled() ? 'A' : 'a'
        );
    }

    // Read the actual clock: a failed profile switch must not mislabel it.
    const auto cpu_mhz = platform::system_clock_hz() / 1000000u;
    const char* profile =
        system_controls::cpu_profile_status_name(cpu_mhz);
    char line3[80] = {};
    std::snprintf(
        line3, sizeof(line3), "CPU:%s %luMHz CON:%s %s",
        profile, static_cast<unsigned long>(cpu_mhz),
        console_name(settings_.console_mode), run_text
    );

    const std::uint32_t bg = theme_status_bg(settings_.theme);
    const std::uint32_t fg = theme_status_fg(settings_.theme);

    const auto draw_status_row = [fg, bg](int row, const char* text) {
        platform::draw_text_row(row, text, fg, bg);
    };
    draw_status_row(0, line1);
    draw_status_row(1, line2);
    draw_status_row(2, line3);
}

void Repl::render_function_keys() {
    if (!platform::function_key_bar_enabled()) return;

    const int first = platform::shift_held() ? 5 : 0;
    char line[80] = {};
    std::size_t used = 0;

    for (int i = 0; i < 5; ++i) {
        const int index = first + i;
        const char* filename = settings_.quick[index].filename;

        char short_name[10] = "-";
        if (filename && *filename) {
            std::size_t length = std::strlen(filename);
            if (length >= 4) {
                const char* ext = filename + length - 4;
                if (ext[0] == '.' &&
                    ascii_upper(ext[1]) == 'B' &&
                    ascii_upper(ext[2]) == 'A' &&
                    ascii_upper(ext[3]) == 'S') {
                    length -= 4;
                }
            }
            if (length > 9) length = 9;
            if (length > 0) {
                std::memcpy(short_name, filename, length);
                short_name[length] = '\0';
            }
        }

        const int written = std::snprintf(
            line + used,
            sizeof(line) - used,
            i < 4 ? "%-9.9s  " : "%-9.9s",
            short_name
        );
        if (written <= 0) break;
        used += static_cast<std::size_t>(written);
        if (used >= sizeof(line)) {
            line[sizeof(line) - 1] = '\0';
            break;
        }
    }

    const std::uint32_t bg = theme_status_bg(settings_.theme);
    platform::draw_text_row(
        39,
        line,
        0xffffff,
        bg
    );
}

void Repl::capture_hotkey_screenshot() {
    char base[24] = {};
    bool found = false;

    for (int number = 1; number <= 9999; ++number) {
        std::snprintf(
            base,
            sizeof(base),
            "SCREEN%04d",
            number
        );
        if (!storage::screenshot_exists(base)) {
            found = true;
            break;
        }
    }

    if (!found) {
        if (platform::function_key_bar_enabled()) {
            platform::draw_text_row(
                39,
                "SCREENSHOT: NO FREE SCREEN0001..9999 NAME",
                0xff8080,
                theme_status_bg(settings_.theme)
            );
            platform::sleep_millis(700);
            render_function_keys();
        }
        return;
    }

    const bool ok = storage::save_screenshot(base);

    char filename[32] = {};
    std::snprintf(filename, sizeof(filename), "%s.BMP", base);

    if (platform::function_key_bar_enabled()) {
        char message[80] = {};
        std::snprintf(
            message,
            sizeof(message),
            ok ? "SAVED %s" : "SCREENSHOT ERROR: %s",
            ok ? filename : storage::last_error()
        );
        platform::draw_text_row(
            39,
            message,
            ok ? 0xffffff : 0xff8080,
            theme_status_bg(settings_.theme)
        );
        platform::sleep_millis(500);
        render_function_keys();
    }

    platform::serial_put_string_raw(
        ok ? "SCREENSHOT SAVED " : "SCREENSHOT ERROR "
    );
    platform::serial_put_string_raw(
        ok ? filename : storage::last_error()
    );
    platform::serial_put_string_raw("\r\n");
}

void Repl::ensure_body_cursor() {
    if (settings_.status_enabled &&
        platform::cursor_row() < console_layout::status_rows) {
        platform::set_cursor_position(0, console_layout::status_rows);
    }
}

void Repl::print_banner() {
    render_status();
    ensure_body_cursor();

    char version_line[96] = {};
    std::snprintf(
        version_line,
        sizeof(version_line),
        " Version %s  Build %s\r\n",
        RMB_VERSION,
        RMB_BUILD_NUMBER
    );

    platform::set_function_key_bar_enabled(true);
    render_function_keys();

    platform::put_string("--------------------------------------------\r\n");
    platform::put_string(" Cala's Pokecom BASIC\r\n");
    platform::put_string(version_line);
    platform::put_string(" Copyright (C) 2026 Cala Maclir\r\n");
    platform::put_string(" PicoCalc / Raspberry Pi Pico 2 W\r\n");
    platform::put_string("--------------------------------------------\r\n");

    char capacity_line[96] = {};
    std::snprintf(
        capacity_line,
        sizeof(capacity_line),
        " Program: RAM 256x191 / SD 1024x2047\r\n"
    );
    platform::put_string(capacity_line);

    if (storage::available()) {
        platform::put_string(" SD: READY\r\n\r\n");
    } else {
        platform::put_string(" SD: ");
        platform::put_string(storage::last_error());
        platform::put_string("\r\n\r\n");
    }
}

void Repl::print_prompt() {
    platform::set_function_key_bar_enabled(true);
    render_status();
    render_function_keys();

    if (platform::get_console_mode() != platform::ConsoleMode::Serial) {
        ensure_body_cursor();
    }

    // The line editor now scrolls only when typed input actually wraps below
    // the visible console. Use the full body through row 38 and clear any
    // stale pixels left by a previous scrolled line before drawing the prompt.
    platform::clear_to_eol();
    platform::put_string("BASIC> ");
}

void Repl::print_program_error() {
    platform::put_char('?'); platform::put_string(program_.error()); platform::put_string("\r\n");
}

void Repl::list_program() {
    if (!program_.ready()) { print_program_error(); return; }
    if (program_.size() == 0) {
        platform::put_string("(empty)\r\n");
        return;
    }

    char number[24] = {};
    for (std::size_t i = 0; i < program_.size(); ++i) {
        std::int32_t line_number=0;
        const char* text=nullptr;
        std::size_t length=0;
        if(!program_.read_line_text(i,line_number,text,length)) {
            print_program_error();return;
        }
        std::snprintf(
            number,sizeof(number),"%ld ",static_cast<long>(line_number));
        platform::put_string(number);
        platform::put_string(text);
        platform::put_string("\r\n");
    }
}

bool Repl::load_named_program(
    const char* filename,
    bool run_after_load
) {
    if (!filename || !*filename) return false;

    if (!storage::load_program(filename, program_)) {
        print_storage_error();
        return false;
    }

    set_current_filename(program_.filename(), false);

    platform::put_string("LOADED ");
    platform::put_string(program_.filename());
    platform::put_string("\r\n");

    if (run_after_load) {
        run_program();
    }

    return true;
}

void Repl::command_profile(char* argument) {
    argument = skip_spaces(argument ? argument : const_cast<char*>(""));

    if (*argument == '\0') {
        const char* mode = "OFF";
        if (vm_.profile_mode() == VmProfileMode::Counts) {
            mode = "ON";
        } else if (vm_.profile_mode() == VmProfileMode::Timed) {
            mode = "TIME";
        }

        platform::put_string("PROFILE: ");
        platform::put_string(mode);
        platform::put_string(
            "\r\nPROFILE ON    = LOW-OVERHEAD OPCODE COUNTS"
            "\r\nPROFILE TIME  = PER-OPCODE TIMING (SLOWER)"
            "\r\nPROFILE SHOW  = SHOW LAST RUN"
            "\r\nPROFILE RESET = CLEAR LAST RUN"
            "\r\nPROFILE OFF   = DISABLE PROFILING\r\n"
        );
        return;
    }

    if (ci_equal(argument, "ON") || ci_equal(argument, "COUNT")) {
        vm_.set_profile_mode(VmProfileMode::Counts);
        vm_.reset_profile();
        platform::put_string(
            "PROFILE COUNT ON - RUN, THEN PROFILE SHOW\r\n"
        );
        return;
    }

    if (ci_equal(argument, "TIME") || ci_equal(argument, "TIMED")) {
        vm_.set_profile_mode(VmProfileMode::Timed);
        vm_.reset_profile();
        platform::put_string(
            "PROFILE TIME ON - TIMING OVERHEAD IS EXPECTED\r\n"
            "RUN, THEN PROFILE SHOW\r\n"
        );
        return;
    }

    if (ci_equal(argument, "OFF")) {
        vm_.set_profile_mode(VmProfileMode::Off);
        platform::put_string("PROFILE OFF\r\n");
        return;
    }

    if (ci_equal(argument, "SHOW")) {
        vm_.print_profile_report();
        return;
    }

    if (ci_equal(argument, "RESET")) {
        vm_.reset_profile();
        platform::put_string("PROFILE RESET\r\n");
        return;
    }

    platform::put_string(
        "?PROFILE ON/TIME/OFF/SHOW/RESET\r\n"
    );
}

void Repl::process_line(char* line) {
    trim_right(line);
    char* input = skip_spaces(line);

    if (*input == '\0') return;

    std::int32_t line_number = 0;
    char* body = nullptr;

    if (*input >= '0' && *input <= '9') {
        if (!parse_line_number(input, line_number, body)) {
            platform::put_string("?BAD LINE NUMBER\r\n");
            return;
        }

        if (*body == '\0') {
            if (!program_.erase_line(line_number)) print_program_error();
            else program_dirty_ = true;
            return;
        }

        if (std::strlen(body) >= kMaxProgramLineLength) {
            platform::put_string("?LINE TOO LONG\r\n");
            return;
        }

        if (!program_.set_line(line_number, body)) {
            print_program_error();
        } else {
            program_dirty_ = true;
        }
        return;
    }

    if (command_equals(input, "LIST")) {
        list_program();
        return;
    }

    if (command_equals(input, "RUN")) {
        run_program();
        return;
    }

    if (command_equals(input, "EDIT")) {
        open_full_screen_editor();
        return;
    }

    if (char* arg = command_argument(input, "PROFILE")) {
        command_profile(arg);
        return;
    }

    if (command_equals(input, "MENU")) {
        show_system_menu();
        return;
    }

    if (command_equals(input, "NEW")) {
        if (!program_.clear()) { print_program_error(); return; }
        vm_.clear_direct_state();
        set_current_filename("UNTITLED", false);
        platform::put_string("OK\r\n");
        return;
    }

    if (command_equals(input, "CLEAR")) {
        vm_.clear_direct_state();
        platform::put_string("OK (DIRECT VARIABLES CLEARED)\r\n");
        return;
    }

    if (command_equals(input, "CLS")) {
        platform::clear_screen();
        return;
    }

    if (command_equals(input, "STANDBY")) {
        if (!storage::firmware_owns_card()) {
            platform::put_string("?EJECT USB STORAGE BEFORE STANDBY\r\n");
            return;
        }
        platform::put_string("STANDBY - PRESS ANY KEY TO WAKE\r\n");
        platform::enter_sleep_mode();
        return;
    }

    if (command_equals(input, "FILES")) {
        if (!storage::list_program_files()) print_storage_error();
        return;
    }
    if (command_equals(input, "DIR")) {
        if (!storage::list_root_files()) print_storage_error();
        return;
    }

    if (char* arg = command_argument(input, "XRECV")) {
        command_xmodem(arg, true);
        return;
    }
    if (char* arg = command_argument(input, "XSEND")) {
        command_xmodem(arg, false);
        return;
    }
    if (command_equals(input, "YRECV")) {
        command_ymodem(nullptr, true);
        return;
    }
    if (char* arg = command_argument(input, "YSEND")) {
        command_ymodem(arg, false);
        return;
    }
    if (char* arg = command_argument(input, "LOAD")) {
        char filename[80] = {};
        if (!parse_filename_argument(arg, filename, sizeof(filename))) {
            platform::put_string("?FILENAME REQUIRED\r\n");
            return;
        }

        load_named_program(filename, false);
        return;
    }

    if (char* arg = command_argument(input, "SAVE")) {
        char filename[80] = {};
        const bool has_argument = *skip_spaces(arg) != '\0';
        if (has_argument &&
            !parse_filename_argument(arg, filename, sizeof(filename))) {
            platform::put_string("?FILENAME REQUIRED\r\n");
            return;
        }
        if (!has_argument && !has_current_filename()) {
            platform::put_string("?FILENAME REQUIRED\r\n");
            return;
        }

        const bool saved = has_argument
            ? save_program_as(filename)
            : save_current_program();
        if (!saved) {
            print_storage_error();
            return;
        }

        platform::put_string("SAVED ");
        platform::put_string(program_.filename());
        platform::put_string("\r\n");
        return;
    }

    if (char* arg = command_argument(input, "SD")) {
        command_sd(arg);
        return;
    }

    if (char* arg = command_argument(input, "DATETIME")) {
        command_datetime(arg);
        return;
    }

    if (char* arg = command_argument(input, "DATE")) {
        command_date(arg);
        return;
    }

    if (char* arg = command_argument(input, "TIME")) {
        command_time(arg);
        return;
    }

    if (command_equals(input, "SCREENSHOT")) {
        if (!storage::save_screenshot("SCREEN")) {
            print_storage_error();
            return;
        }
        platform::put_string("SAVED SCREEN.BMP\r\n");
        return;
    }

    if (char* arg = command_argument(input, "SCREENSHOT")) {
        char filename[80] = {};
        if (!parse_filename_argument(arg, filename, sizeof(filename))) {
            platform::put_string("?FILENAME REQUIRED\r\n");
            return;
        }

        if (!storage::save_screenshot(filename)) {
            print_storage_error();
            return;
        }

        platform::put_string("SAVED ");
        platform::put_string(filename);
        platform::put_string(".BMP\r\n");
        return;
    }

    if (char* arg = command_argument(input, "SERIAL")) {
        if (*arg == '\0') {
            platform::put_string(
                "SERIAL: USB CDC + UART0 115200 8N1\r\n"
                "SERIAL ON   = LCD + SERIAL\r\n"
                "SERIAL OFF  = LCD ONLY\r\n"
                "SERIAL ONLY = SERIAL ONLY\r\n"
                "MODE: "
            );
            platform::put_string(console_name(settings_.console_mode));
            platform::put_string("\r\n");
            return;
        }

        if (command_equals(arg, "ON") ||
            command_equals(arg, "BOTH")) {
            settings_.console_mode = platform::ConsoleMode::Both;
        } else if (command_equals(arg, "OFF")) {
            settings_.console_mode = platform::ConsoleMode::Lcd;
        } else if (command_equals(arg, "ONLY")) {
            settings_.console_mode = platform::ConsoleMode::Serial;
        } else {
            platform::put_string("?SERIAL ON/OFF/ONLY\r\n");
            return;
        }

        platform::set_console_mode(settings_.console_mode);
        save_settings();
        platform::put_string("CONSOLE: ");
        platform::put_string(console_name(settings_.console_mode));
        platform::put_string("\r\n");
        return;
    }

    if (char* arg = command_argument(input, "CONSOLE")) {
        if (command_equals(arg, "LCD")) {
            settings_.console_mode = platform::ConsoleMode::Lcd;
        } else if (command_equals(arg, "BOTH")) {
            settings_.console_mode = platform::ConsoleMode::Both;
        } else if (command_equals(arg, "SERIAL")) {
            settings_.console_mode = platform::ConsoleMode::Serial;
        } else {
            platform::put_string("?CONSOLE LCD/BOTH/SERIAL\r\n");
            return;
        }

        platform::set_console_mode(settings_.console_mode);
        save_settings();
        platform::put_string("CONSOLE: ");
        platform::put_string(console_name(settings_.console_mode));
        platform::put_string("\r\n");
        return;
    }

    if (command_equals(input, "HELP")) {
        platform::put_string("HOME (SHIFT+TAB) - system menu\r\n");
        platform::put_string("F1-F10 - configured quick LOAD/RUN\r\n");
        platform::put_string("ALT+S - automatic BMP screenshot\r\n");
        platform::put_string("ALT+, / ALT+. - LCD brightness down/up\r\n");
        platform::put_string("ALT+SPACE - keyboard backlight cycle\r\n");
        platform::put_string("POWER or ALT+P - standby / any key wake\r\n");
        platform::put_string("LIST / NEW / RUN / EDIT / CLS / MENU / STANDBY\r\n");
        platform::put_string("PAUSE - wait for a key / INKEY - poll key code\r\n");
        platform::put_string("FILES - list BASIC programs\r\n");
        platform::put_string("DIR   - list SD card files\r\n");
        platform::put_string("LOAD HELLO / SAVE - save current program\r\n");
        platform::put_string("SAVE TEST - save as TEST.BAS\r\n");
        platform::put_string("XRECV \"FILE\" / XSEND \"FILE\" - XMODEM CRC\r\n");
        platform::put_string("YRECV / YSEND \"FILE\" - YMODEM exact size\r\n");
        platform::put_string("SD [STATUS|REMOUNT]\r\n");
        platform::put_string("DATE [YYYY-MM-DD]\r\n");
        platform::put_string("TIME [HH:MM:SS]\r\n");
        platform::put_string("DATETIME [YYYYMMDDHHMMSS]\r\n");
        platform::put_string("SCREENSHOT [name] - save LCD as BMP\r\n");
        platform::put_string("SERIAL [ON|OFF|ONLY]\r\n");
        platform::put_string("CONSOLE LCD|BOTH|SERIAL\r\n");
        platform::put_string("Bluetooth Keyboard: Control Center -> Bluetooth\r\n");
        platform::put_string("CLEAR - clear direct-mode variables\r\n");
        return;
    }

    run_direct_line(input);
}

void Repl::run_direct_line(const char* line) {
    if (!line || !*line) return;

    CompileWorkspaceLease workspace(compiled_, workspace_busy_);
    if (!workspace) {
        platform::put_string("?BASIC BUSY\r\n");
        return;
    }

    const CompileResult cr =
        compiler_.compile_direct(line, compiled_);

    if (!cr.ok) {
        platform::put_char('?');
        platform::put_string(cr.message);
        platform::put_string("\r\n");
        return;
    }

    const VmResult vr = vm_.run_direct(compiled_);
    handle_runtime_result(vr);
    if (!vr.ok) {
        ensure_body_cursor();
        platform::put_char('?');
        platform::put_string(vr.message);
        platform::put_string("\r\n");
    }
}

void Repl::run_autorun() {
    if (!storage::available()) return;
    if (!storage::program_exists("AUTORUN")) return;

    platform::put_string("[AUTORUN] AUTORUN.BAS\r\n");

    if (!storage::load_program("AUTORUN", program_)) {
        print_storage_error();
        return;
    }

    set_current_filename("AUTORUN.BAS", false);
    run_program();
}

void Repl::run_program() {
    // Text output keeps the footer. Graphics APIs release it when needed.
    CompileWorkspaceLease workspace(compiled_, workspace_busy_);
    if (!workspace) {
        platform::put_string("?BASIC BUSY\r\n");
        return;
    }

    const CompileResult cr = compiler_.compile(program_, compiled_);

    if (!cr.ok) {
        char message[160] = {};
        if (cr.line > 0) {
            std::snprintf(
                message,
                sizeof(message),
                "?%s IN %ld\r\n",
                cr.message,
                static_cast<long>(cr.line)
            );
        } else {
            std::snprintf(
                message,
                sizeof(message),
                "?%s\r\n",
                cr.message
            );
        }
        platform::set_function_key_bar_enabled(true);
        render_function_keys();
        ensure_body_cursor();
        platform::put_string(message);
        return;
    }

    const std::uint32_t run_start_ms = platform::monotonic_millis();
    const VmResult vr = vm_.run(compiled_);
    handle_runtime_result(vr);
    const std::uint32_t run_end_ms = platform::monotonic_millis();
    const std::uint32_t elapsed_ms = run_end_ms - run_start_ms;
    last_run_ms_ = elapsed_ms;

    platform::set_function_key_bar_enabled(true);
    render_status();
    render_function_keys();
    ensure_body_cursor();
    if (platform::cursor_column() != 0) platform::put_string("\r\n");

    if (!vr.ok) {
        platform::put_char('?');
        platform::put_string(vr.message);
        platform::put_string("\r\n");
    }

    const std::uint32_t minutes = elapsed_ms / 60000u;
    const std::uint32_t seconds = (elapsed_ms / 1000u) % 60u;
    const std::uint32_t millis = elapsed_ms % 1000u;

    char timing[64] = {};
    std::snprintf(
        timing,
        sizeof(timing),
        "[RUN] %lum %lu.%03lus\r\n",
        static_cast<unsigned long>(minutes),
        static_cast<unsigned long>(seconds),
        static_cast<unsigned long>(millis)
    );
    platform::put_string(timing);
}

int Repl::function_key_index(int key) {
    if (key >= 0x81 && key <= 0x89) {
        return key - 0x81;
    }
    if (key == 0x90) {
        return 9;
    }
    return -1;
}

void Repl::assign_quick_key(
    int index,
    const char* filename,
    bool run
) {
    if (index < 0 || index >= kQuickKeyCount) return;

    std::snprintf(
        settings_.quick[index].filename,
        sizeof(settings_.quick[index].filename),
        "%s",
        filename ? filename : ""
    );
    settings_.quick[index].run = run;
    save_settings();
    render_function_keys();
}

void Repl::handle_quick_key(int key) {
    const int index = function_key_index(key);
    if (index < 0) return;

    platform::put_string("\r\n");

    const QuickKey& quick = settings_.quick[index];
    if (quick.filename[0] == '\0') {
        char message[48] = {};
        std::snprintf(
            message,
            sizeof(message),
            "F%d: NOT ASSIGNED (HOME -> QUICK LOAD KEYS)\r\n",
            index + 1
        );
        platform::put_string(message);
        return;
    }

    char message[128] = {};
    std::snprintf(
        message,
        sizeof(message),
        "F%d: %s %s\r\n",
        index + 1,
        quick.run ? "RUN" : "LOAD",
        quick.filename
    );
    platform::put_string(message);
    load_named_program(quick.filename, quick.run);
}

void Repl::draw_menu_header(
    const char* title,
    const char* help
) {
    // MENU loops call this function on every key press.  Clearing the whole
    // LCD here made even a one-line cursor move flash the complete screen.
    // Cache the current screen identity and only rebuild the screen when the
    // title/help, status-bar mode, or theme actually changes.
    static bool valid = false;
    static bool cached_status_enabled = false;
    static std::uint8_t cached_theme = 0xff;
    static char cached_title[64] = {};
    static char cached_help[128] = {};

    // Internal invalidation hook used after text-entry screens or when leaving
    // the control center.
    if (!title && !help) {
        valid = false;
        return;
    }

    const char* actual_title = title ? title : "SYSTEM MENU";
    const char* actual_help = help ? help : "";

    const bool same_screen =
        valid &&
        cached_status_enabled == settings_.status_enabled &&
        cached_theme == settings_.theme &&
        std::strcmp(cached_title, actual_title) == 0 &&
        std::strcmp(cached_help, actual_help) == 0;

    if (same_screen) {
        return;
    }

    platform::set_function_key_bar_enabled(true);
    platform::clear_lcd_color(0x000000);
    platform::set_status_area_enabled(settings_.status_enabled);
    render_status();
    render_function_keys();

    const int top = settings_.status_enabled ? console_layout::status_rows : 0;
    const std::uint32_t bg = theme_status_bg(settings_.theme);

    platform::draw_text_row(
        top,
        actual_title,
        0xffffff,
        bg
    );
    platform::draw_text_row(
        top + 1,
        actual_help,
        0xa0a0a0,
        0x000000
    );

    std::snprintf(cached_title, sizeof(cached_title), "%s", actual_title);
    std::snprintf(cached_help, sizeof(cached_help), "%s", actual_help);
    cached_status_enabled = settings_.status_enabled;
    cached_theme = settings_.theme;
    valid = true;
}

void Repl::draw_menu_option(
    int row,
    const char* text,
    bool selected
) {
    platform::draw_text_row(
        row,
        text ? text : "",
        selected ? 0xffffff : 0x00ff80,
        selected ? theme_selected_bg(settings_.theme) : 0x000000
    );
}

void Repl::draw_menu_message(int row, const char* text) {
    platform::draw_text_row(
        row,
        text ? text : "",
        0xffff80,
        0x000000
    );
}

void Repl::leave_menu_screen() {
    draw_menu_header(nullptr, nullptr);
    platform::set_function_key_bar_enabled(true);
    platform::clear_lcd();
    platform::set_status_area_enabled(settings_.status_enabled);
    render_status();
    render_function_keys();
    platform::set_cursor_position(
        0,
        settings_.status_enabled ? console_layout::status_rows : 0
    );
}

bool Repl::choose_quick_mode(bool& run) {
    int selected = run ? 1 : 0;

    while (true) {
        draw_menu_header(
            "QUICK KEY ACTION",
            "UP/DOWN SELECT  ENTER OK  ESC CANCEL"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        draw_menu_option(top + 4, "LOAD ONLY", selected == 0);
        draw_menu_option(top + 5, "LOAD & RUN", selected == 1);

        const int key = platform::get_char();
        if (key == kKeyUp || key == kKeyDown) {
            selected = 1 - selected;
        } else if (key_is_enter(key)) {
            run = selected == 1;
            return true;
        } else if (key == kKeyEscape || key == 0x1b) {
            return false;
        }
    }
}

bool Repl::pick_program_file(
    char* output,
    std::size_t capacity
) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';

    auto& files = menu_file_scratch.names;
    const std::size_t count = storage::collect_program_files(
        &files[0][0],
        kMenuFileCount,
        kMenuFilenameSize
    );

    if (count == 0) {
        draw_menu_header("SELECT BASIC FILE", "ESC BACK");
        draw_menu_message(
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4,
            storage::available() ? "(NO .BAS FILES)" : storage::last_error()
        );
        while (true) {
            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key_is_enter(key)) {
                return false;
            }
        }
    }

    sort_file_names(files, count);

    constexpr int visible = 27;
    menu_scroll::State scroll;
    menu_scroll::normalize(
        scroll, static_cast<int>(count), visible);

    while (true) {
        draw_menu_header(
            "SELECT BASIC FILE",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER USE  ESC"
        );

        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;

        menu_scroll::normalize(
            scroll, static_cast<int>(count), visible);

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int index = scroll.offset + row_index;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + row_index, "", false);
                continue;
            }
            char row[96] = {};
            std::snprintf(
                row, sizeof(row), "%2d  %-46.46s",
                index + 1, files[index]);
            draw_menu_option(
                first_row + row_index,
                row,
                index == scroll.selected
            );
        }

        char position[48] = {};
        std::snprintf(
            position, sizeof(position),
            "%d-%d / %u",
            scroll.offset + 1,
            menu_scroll::last_exclusive(
                scroll, static_cast<int>(count), visible),
            static_cast<unsigned>(count)
        );
        draw_menu_message(first_row + visible, position);

        const int key = platform::get_char();
        if (handle_menu_scroll_key(
                key, scroll, static_cast<int>(count), visible)) {
            continue;
        }
        if (key_is_enter(key)) {
            std::snprintf(
                output, capacity, "%s", files[scroll.selected]);
            return true;
        }
        if (key == kKeyEscape || key == 0x1b) return false;
    }
}

bool Repl::pick_transfer_file(
    char* output,
    std::size_t capacity,
    const char* title
) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';

    auto& files = menu_file_scratch.names;
    const std::size_t count = storage::collect_transfer_files(
        &files[0][0], kMenuFileCount, kMenuFilenameSize);

    if (count == 0) {
        draw_menu_header(title, "ESC BACK");
        draw_menu_message(
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4,
            storage::available() ? "(NO TRANSFERABLE FILES)" : storage::last_error());
        while (true) {
            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key_is_enter(key)) return false;
        }
    }

    sort_file_names(files, count);
    constexpr int visible = 27;
    menu_scroll::State scroll;
    menu_scroll::normalize(
        scroll, static_cast<int>(count), visible);

    while (true) {
        draw_menu_header(
            title,
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER SEND  ESC"
        );
        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;

        menu_scroll::normalize(
            scroll, static_cast<int>(count), visible);

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int index = scroll.offset + row_index;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + row_index, "", false);
                continue;
            }
            char row[96] = {};
            std::snprintf(
                row, sizeof(row), "%2d  %-46.46s",
                index + 1, files[index]);
            draw_menu_option(
                first_row + row_index,
                row,
                index == scroll.selected);
        }

        char position[48] = {};
        std::snprintf(
            position, sizeof(position),
            "%d-%d / %u",
            scroll.offset + 1,
            menu_scroll::last_exclusive(
                scroll, static_cast<int>(count), visible),
            static_cast<unsigned>(count)
        );
        draw_menu_message(first_row + visible, position);

        const int key = platform::get_char();
        if (handle_menu_scroll_key(
                key, scroll, static_cast<int>(count), visible)) {
            continue;
        }
        if (key_is_enter(key)) {
            std::snprintf(
                output, capacity, "%s", files[scroll.selected]);
            return true;
        }
        if (key == kKeyEscape || key == 0x1b) return false;
    }
}

bool Repl::menu_files() {
    enum class Mode { Programs, Directory };
    Mode mode = Mode::Programs;
    constexpr int visible = 24;
    menu_scroll::State scroll;
    bool info_visible = false;
    storage::DirectoryEntry inline_info;
    char playing_name[kMenuFilenameSize] = {};
    std::size_t count = 0;
    bool scan_ok = true;
    bool scan_requested = true;

    auto show_error = [&](const char* title, const char* message) {
        draw_menu_header(title, "PRESS ANY KEY");
        const int top = settings_.status_enabled
            ? console_layout::status_rows : 0;
        draw_menu_message(top + 5, message ? message : "FILE OPERATION FAILED");
        (void)platform::get_char();
    };

    auto confirm_delete = [&](const char* name) {
        int selected = 0; // Safe default: Cancel.
        while (true) {
            draw_menu_header(
                "DELETE FILE?",
                "UP/DOWN SELECT  ENTER OK  ESC CANCEL"
            );
            const int top = settings_.status_enabled
                ? console_layout::status_rows : 0;
            draw_menu_message(top + 4, name);
            draw_menu_option(top + 7, "Cancel", selected == 0);
            draw_menu_option(top + 8, "Delete", selected == 1);
            const int key = platform::get_char();
            if (key == kKeyUp || key == kKeyDown) {
                selected = 1 - selected;
            } else if (key_is_enter(key)) {
                return selected == 1;
            } else if (key == kKeyEscape || key == 0x1b) {
                return false;
            }
        }
    };

    while (true) {
        if (playing_name[0] && !platform::audio_playing()) {
            playing_name[0] = '\0';
            scan_requested = true;
        }
        if (scan_requested) {
            if (mode == Mode::Programs) {
                auto& names = menu_file_scratch.names;
                count = storage::collect_program_files(
                    &names[0][0], kMenuFileCount, kMenuFilenameSize);
                sort_file_names(names, count);
            } else {
                auto& entries = menu_file_scratch.entries;
                count = storage::collect_root_entries(entries, kMenuFileCount);
                sort_directory_entries(entries, count);
            }
            scan_ok = std::strcmp(storage::last_error(), "OK") == 0;
            scan_requested = false;
        }
        menu_scroll::normalize(scroll, static_cast<int>(count), visible);

        draw_menu_header(
            mode == Mode::Programs ? "FILES [PROGRAMS]" : "FILES [DIRECTORY]",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  LEFT/RIGHT MODE"
        );

        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int index = scroll.offset + row_index;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(
                    first_row + row_index,
                    row_index == 0 && count == 0
                        ? (scan_ok
                            ? (mode == Mode::Programs
                                ? "(NO .BAS FILES)" : "(NO FILES)")
                            : storage::last_error())
                        : "",
                    false
                );
                continue;
            }

            char row[96] = {};
            if (mode == Mode::Programs) {
                std::snprintf(
                    row, sizeof(row), "%2d  %-46.46s",
                    index + 1, menu_file_scratch.names[index]);
            } else {
                const auto& entry = menu_file_scratch.entries[index];
                if (entry.directory) {
                    std::snprintf(
                        row, sizeof(row), "%2d  %-38.38s <DIR>",
                        index + 1, entry.name);
                } else {
                    std::snprintf(
                        row, sizeof(row), "%2d  %-36.36s %9lu",
                        index + 1, entry.name,
                        static_cast<unsigned long>(entry.size));
                }
            }
            draw_menu_option(
                first_row + row_index,
                row,
                index == scroll.selected
            );
        }

        char position[80] = {};
        if (count != 0) {
            std::snprintf(
                position, sizeof(position),
                "%d-%d / %u",
                scroll.offset + 1,
                menu_scroll::last_exclusive(
                    scroll, static_cast<int>(count), visible),
                static_cast<unsigned>(count)
            );
        }
        draw_menu_message(first_row + visible, position);
        draw_menu_message(
            first_row + visible + 1,
            mode == Mode::Programs
                ? "ENTER LOAD  R RUN  E EDIT  N RENAME"
                : "P PLAY/STOP  N RENAME  DEL DELETE"
        );
        draw_menu_message(
            first_row + visible + 2,
            mode == Mode::Programs
                ? "DEL DELETE  I INFO  F REFRESH  ESC BACK"
                : "I INFO  F REFRESH  LEFT/RIGHT  ESC"
        );

        char info_row[112] = {};
        if (info_visible) {
            std::snprintf(
                info_row, sizeof(info_row), "Info: %s", inline_info.name);
            draw_menu_message(first_row + visible + 4, info_row);
            std::snprintf(
                info_row, sizeof(info_row), "Type: %s",
                inline_info.directory ? "DIRECTORY" :
                audio::has_extension_ci(inline_info.name, ".BAS")
                    ? "BASIC PROGRAM" :
                audio::has_extension_ci(inline_info.name, ".WAV")
                    ? "WAV AUDIO" :
                audio::has_extension_ci(inline_info.name, ".MP3")
                    ? "MP3 AUDIO" : "FILE");
            draw_menu_message(first_row + visible + 5, info_row);
            if (!inline_info.directory) {
                std::snprintf(
                    info_row, sizeof(info_row), "Size: %lu bytes",
                    static_cast<unsigned long>(inline_info.size));
                draw_menu_message(first_row + visible + 6, info_row);
            } else {
                draw_menu_message(first_row + visible + 6, "");
            }
        } else {
            if (playing_name[0]) {
                std::snprintf(
                    info_row, sizeof(info_row),
                    "Playing: %.70s", playing_name);
                draw_menu_message(first_row + visible + 4, info_row);
            } else {
                draw_menu_message(first_row + visible + 4, "");
            }
            draw_menu_message(first_row + visible + 5, "");
            draw_menu_message(first_row + visible + 6, "");
        }

        const int key = platform::get_char();
        if (key == kKeyLeft || key == kKeyRight) {
            if (playing_name[0]) {
                platform::audio_stop();
                playing_name[0] = '\0';
            }
            mode = mode == Mode::Programs ? Mode::Directory : Mode::Programs;
            scroll = menu_scroll::State{};
            info_visible = false;
            scan_requested = true;
            continue;
        }
        if (handle_menu_scroll_key(
                key, scroll, static_cast<int>(count), visible)) {
            info_visible = false;
            continue;
        }
        if (key == 'f' || key == 'F') {
            if (playing_name[0]) {
                platform::audio_stop();
                playing_name[0] = '\0';
            }
            info_visible = false;
            scan_requested = true;
            continue;
        }
        if (key == 3 && playing_name[0]) {
            platform::audio_stop();
            playing_name[0] = '\0';
            info_visible = false;
            continue;
        }
        if (key == kKeyEscape || key == 0x1b) {
            if (playing_name[0]) platform::audio_stop();
            return false;
        }
        if (count == 0) continue;

        const char* selected_name = mode == Mode::Programs
            ? menu_file_scratch.names[scroll.selected]
            : menu_file_scratch.entries[scroll.selected].name;
        const bool selected_directory = mode == Mode::Directory &&
            menu_file_scratch.entries[scroll.selected].directory;

        if ((key == 'p' || key == 'P') &&
            mode == Mode::Directory && !selected_directory) {
            if (!audio::playable_audio_filename(selected_name)) {
                show_error("AUDIO", "SELECT A WAV OR MP3 FILE");
                continue;
            }
            if (playing_name[0] && ci_equal(playing_name, selected_name)) {
                platform::audio_stop();
                playing_name[0] = '\0';
            } else if (platform::audio_wavplay(selected_name)) {
                std::snprintf(
                    playing_name, sizeof(playing_name), "%s", selected_name);
            } else {
                show_error("AUDIO", platform::audio_last_error());
            }
            info_visible = false;
            continue;
        }

        if (key_is_enter(key) && mode == Mode::Programs) {
            char filename[kMenuFilenameSize] = {};
            std::snprintf(filename, sizeof(filename), "%s", selected_name);
            leave_menu_screen();
            load_named_program(filename, false);
            return true;
        }
        if ((key == 'r' || key == 'R') && mode == Mode::Programs) {
            char filename[kMenuFilenameSize] = {};
            std::snprintf(filename, sizeof(filename), "%s", selected_name);
            leave_menu_screen();
            load_named_program(filename, true);
            return true;
        }
        if ((key == 'e' || key == 'E') && mode == Mode::Programs) {
            char filename[kMenuFilenameSize] = {};
            std::snprintf(filename, sizeof(filename), "%s", selected_name);
            if (!storage::load_program(filename, program_)) {
                show_error("EDIT", storage::last_error());
                continue;
            }
            set_current_filename(program_.filename(), false);
            leave_menu_screen();
            open_full_screen_editor();
            return true;
        }

        if (key == 'i' || key == 'I') {
            if (mode == Mode::Directory) {
                inline_info = menu_file_scratch.entries[scroll.selected];
            } else if (!storage::root_entry_info(
                           selected_name, inline_info)) {
                show_error("FILE INFO", storage::last_error());
                info_visible = false;
                continue;
            }
            info_visible = true;
            continue;
        }

        if (key == 'n' || key == 'N') {
            if (playing_name[0]) {
                platform::audio_stop();
                playing_name[0] = '\0';
            }
            if (selected_directory) {
                show_error("RENAME", "DIRECTORY RENAME NOT AVAILABLE");
                continue;
            }
            char prompt[128] = {};
            std::snprintf(
                prompt, sizeof(prompt), "RENAME\r\n\r\nOld: %s\r\nNew: ",
                selected_name);
            char new_name[kMenuFilenameSize] = {};
            if (!prompt_text(
                    prompt, new_name, sizeof(new_name), selected_name)) {
                continue;
            }
            char normalized_name[kMenuFilenameSize] = {};
            const char* rename_target = new_name;
            if (mode == Mode::Programs) {
                if (!file_management::normalize_program_name(
                        new_name, normalized_name, sizeof(normalized_name))) {
                    show_error("RENAME FAILED", "BAD FILENAME");
                    continue;
                }
                rename_target = normalized_name;
            }
            const bool renaming_current =
                program_files::equal(selected_name, current_filename_);
            if (!storage::rename_root_file(
                    selected_name, rename_target, current_filename_)) {
                show_error("RENAME FAILED", storage::last_error());
            } else if (renaming_current &&
                       !program_.note_source_renamed(
                           selected_name, rename_target)) {
                char session_error[96] = {};
                std::snprintf(
                    session_error, sizeof(session_error), "%s",
                    program_.error());
                if (!storage::rename_root_file(
                        rename_target, selected_name,
                        selected_name, true)) {
                    show_error(
                        "RENAME RECOVERY FAILED",
                        "CHECK SOURCE FILE AND SESSION");
                } else {
                    show_error("RENAME FAILED", session_error);
                }
            } else if (renaming_current) {
                set_current_filename(
                    program_.filename(), program_.is_dirty());
            }
            info_visible = false;
            scan_requested = true;
            continue;
        }

        if (key == kKeyDelete) {
            if (playing_name[0]) {
                platform::audio_stop();
                playing_name[0] = '\0';
            }
            if (selected_directory) {
                show_error("DELETE", "DIRECTORY DELETE NOT AVAILABLE");
                continue;
            }
            char filename[kMenuFilenameSize] = {};
            std::snprintf(filename, sizeof(filename), "%s", selected_name);
            if (!confirm_delete(filename)) continue;
            if (!storage::delete_root_file(filename, current_filename_)) {
                show_error("DELETE FAILED", storage::last_error());
            }
            info_visible = false;
            scan_requested = true;
            continue;
        }

        const int quick = mode == Mode::Programs
            ? function_key_index(key) : -1;
        if (quick >= 0) {
            bool run = true;
            if (choose_quick_mode(run)) {
                assign_quick_key(quick, selected_name, run);
            }
        }
    }
}

void Repl::menu_quick_keys() {
    int selected = 0;

    while (true) {
        draw_menu_header(
            "QUICK LOAD KEYS",
            "ENTER ASSIGN  R TOGGLE LOAD/RUN  DEL CLEAR  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        for (int i = 0; i < kQuickKeyCount; ++i) {
            const QuickKey& key = settings_.quick[i];
            char row[96] = {};
            std::snprintf(
                row,
                sizeof(row),
                "F%-2d %-35.35s %s",
                i + 1,
                key.filename[0] ? key.filename : "<not assigned>",
                key.filename[0]
                    ? (key.run ? "RUN" : "LOAD")
                    : ""
            );
            draw_menu_option(first_row + i, row, i == selected);
        }

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown &&
                   selected + 1 < kQuickKeyCount) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key == kKeyDelete) {
            settings_.quick[selected] = QuickKey{};
            save_settings();
        } else if ((key == 'r' || key == 'R') &&
                   settings_.quick[selected].filename[0]) {
            settings_.quick[selected].run =
                !settings_.quick[selected].run;
            save_settings();
        } else if (key_is_enter(key)) {
            char filename[kFilenameSize] = {};
            if (pick_program_file(filename, sizeof(filename))) {
                bool run = true;
                if (choose_quick_mode(run)) {
                    assign_quick_key(selected, filename, run);
                }
            }
        } else {
            const int quick = function_key_index(key);
            if (quick >= 0) selected = quick;
        }
    }
}

void Repl::menu_display() {
    // ALT+, / ALT+. are handled inside the PicoCalc keyboard MCU and do not
    // appear in the host key FIFO. Sync the menu with the real hardware level.
    std::uint8_t hardware_backlight = 0;
    if (platform::get_lcd_backlight(hardware_backlight)) {
        settings_.backlight = hardware_backlight;
    }

    int selected = 0;

    while (true) {
        draw_menu_header(
            "DISPLAY",
            "LEFT/RIGHT CHANGE  ENTER ACTION  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 4;

        char row[96] = {};
        std::snprintf(
            row,
            sizeof(row),
            "Status bar          %s",
            settings_.status_enabled ? "ON" : "OFF"
        );
        draw_menu_option(first_row, row, selected == 0);

        std::snprintf(
            row,
            sizeof(row),
            "LCD backlight       %3u / 255",
            static_cast<unsigned>(settings_.backlight)
        );
        draw_menu_option(first_row + 1, row, selected == 1);

        std::snprintf(
            row,
            sizeof(row),
            "Status theme        %s",
            theme_name(settings_.theme)
        );
        draw_menu_option(first_row + 2, row, selected == 2);

        std::snprintf(
            row,
            sizeof(row),
            "Console theme       %s",
            console_theme_name(
                settings_.console_foreground,
                settings_.console_background
            )
        );
        draw_menu_option(first_row + 3, row, selected == 3);

        draw_menu_option(
            first_row + 4,
            "Advanced console RGB...",
            selected == 4
        );

        draw_menu_message(
            first_row + 7,
            "Console theme changes BASIC text and background together."
        );
        draw_menu_message(
            first_row + 8,
            "Advanced RGB is optional; themes are recommended."
        );

        draw_menu_message(
            first_row + 10,
            "Console preview:"
        );
        platform::draw_text_row(
            first_row + 11,
            "Cala's Pokecom BASIC READY.",
            settings_.console_foreground,
            settings_.console_background
        );
        platform::draw_text_row(
            first_row + 12,
            "10 PRINT \"Hello, PicoCalc!\"",
            settings_.console_foreground,
            settings_.console_background
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 4) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            save_settings();
            platform::set_text_color(
                settings_.console_foreground,
                settings_.console_background
            );
            return;
        } else if (selected == 0 &&
                   (key_is_enter(key) ||
                    key == kKeyLeft ||
                    key == kKeyRight)) {
            settings_.status_enabled = !settings_.status_enabled;
            platform::set_status_area_enabled(settings_.status_enabled);
            save_settings();
            draw_menu_header(nullptr, nullptr);
        } else if (selected == 1 &&
                   (key == kKeyLeft ||
                    key == kKeyRight ||
                    key_is_enter(key))) {
            int value = settings_.backlight;
            value += key == kKeyLeft ? -16 : 16;
            if (value < 16) value = 16;
            if (value > 255) value = 255;
            settings_.backlight = static_cast<std::uint8_t>(value);
            platform::set_lcd_backlight(settings_.backlight);
            save_settings();
        } else if (selected == 2 &&
                   (key == kKeyLeft ||
                    key == kKeyRight ||
                    key_is_enter(key))) {
            int theme = settings_.theme;
            theme += key == kKeyLeft ? -1 : 1;
            if (theme < 0) theme = 2;
            if (theme > 2) theme = 0;
            settings_.theme = static_cast<std::uint8_t>(theme);
            save_settings();
            draw_menu_header(nullptr, nullptr);
        } else if (selected == 3 &&
                   (key == kKeyLeft ||
                    key == kKeyRight ||
                    key_is_enter(key))) {
            const int direction = key == kKeyLeft ? -1 : 1;
            cycle_console_theme(
                settings_.console_foreground,
                settings_.console_background,
                direction
            );
            platform::set_text_color(
                settings_.console_foreground,
                settings_.console_background
            );
            save_settings();
        } else if (selected == 4 && key_is_enter(key)) {
            char foreground[24] = {};
            char background[24] = {};

            if (!prompt_text(
                    "TEXT RGB HEX (RRGGBB): ",
                    foreground,
                    sizeof(foreground))) {
                continue;
            }
            if (!prompt_text(
                    "BACKGROUND RGB HEX (RRGGBB): ",
                    background,
                    sizeof(background))) {
                continue;
            }

            const std::uint32_t old_foreground =
                settings_.console_foreground;
            const std::uint32_t old_background =
                settings_.console_background;

            settings_.console_foreground =
                parse_rgb_setting(foreground, old_foreground);
            settings_.console_background =
                parse_rgb_setting(background, old_background);

            if (settings_.console_foreground ==
                settings_.console_background) {
                settings_.console_foreground = old_foreground;
                settings_.console_background = old_background;
            }

            platform::set_text_color(
                settings_.console_foreground,
                settings_.console_background
            );
            save_settings();
            draw_menu_header(nullptr, nullptr);
        }
    }
}

void Repl::menu_firmware() {
    static const char* items[] = {
        "Enter BOOTSEL",
        "Reboot",
        "Back"
    };

    int selected = 0;
    while (true) {
        draw_menu_header(
            "FIRMWARE",
            "UP/DOWN SELECT  ENTER ACTION  ESC BACK"
        );

        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4;

        for (int i = 0; i < 3; ++i) {
            draw_menu_option(first_row + i, items[i], selected == i);
        }

        draw_menu_message(
            first_row + 5,
            "BOOTSEL: enter RP2350 USB firmware update mode."
        );
        draw_menu_message(
            first_row + 6,
            "Reboot: restart Cala's Pokecom BASIC from flash."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
            continue;
        }
        if (key == kKeyDown && selected < 2) {
            ++selected;
            continue;
        }
        if (key == kKeyEscape || key == 0x1b ||
            (key_is_enter(key) && selected == 2)) {
            return;
        }
        if (!key_is_enter(key)) continue;

        const storage::Owner owner = storage::owner();
        if (owner == storage::Owner::UsbHost ||
            owner == storage::Owner::Transition) {
            draw_menu_header("FIRMWARE", "PRESS ANY KEY");
            draw_menu_message(
                first_row + 2,
                "RETURN / EJECT USB STORAGE BEFORE FIRMWARE ACTION"
            );
            platform::get_char();
            draw_menu_header(nullptr, nullptr);
            continue;
        }

        const bool bootsel = selected == 0;
        draw_menu_header(
            bootsel ? "ENTER BOOTSEL?" : "REBOOT CPB?",
            "ENTER CONFIRM  ESC CANCEL"
        );
        draw_menu_message(
            first_row + 1,
            bootsel
                ? "CPB will stop and reconnect as the RP2350 boot device."
                : "CPB will restart immediately from flash."
        );
        draw_menu_message(
            first_row + 2,
            "Unsaved runtime state will be lost."
        );

        const int confirm = platform::get_char();
        if (!key_is_enter(confirm)) {
            draw_menu_header(nullptr, nullptr);
            continue;
        }

        draw_menu_header(
            "FIRMWARE",
            bootsel ? "ENTERING BOOTSEL..." : "REBOOTING..."
        );
        platform::sleep_millis(120);

        if (bootsel) {
            platform::enter_bootsel();
        } else {
            platform::reboot_system();
        }
    }
}

void Repl::menu_power() {
    static const std::uint16_t profiles[] = {200, 150, 100, 75};
    static const char* names[] = {
        "EXP     200 MHz  [EXPERIMENTAL]",
        "FULL    150 MHz",
        "NORMAL  100 MHz",
        "ECO      75 MHz"
    };

    int selected = 1;
    const auto current_mhz =
        platform::system_clock_hz() / 1000000u;
    for (int i = 0; i < 4; ++i) {
        if (current_mhz == profiles[i]) {
            selected = i;
            break;
        }
    }

    const auto apply_cpu_profile = [&](std::uint16_t requested) {
        const bool pll_transition =
            (requested == 200u &&
             platform::full_cpu_clock_hz() < 200000000u) ||
            (requested != 200u &&
             platform::full_cpu_clock_hz() > 150000000u);
        if (!pll_transition || !wireless::initialized()) {
            return platform::set_cpu_clock_mhz(requested);
        }

        // Reprogramming PLL_SYS while CYW43 PIO is live is unsafe. Stop and
        // restore the affected services internally so the user does not need
        // to reboot or manually disable the board LED and radios.
        const bool wifi_initialized = network::initialized();
        const bool wifi_connected = network::connected();
        const bool bluetooth_enabled = bluetooth_manager::enabled();
        const bool file_server_running = network::file_server_running();
        const auto led_mode = wireless::board_led_mode();

        if (bluetooth_enabled) bluetooth_manager::disable();
        if (wifi_initialized) network::shutdown();
        wireless::deinit();

        const bool changed = platform::set_cpu_clock_mhz(requested);

        bool wifi_restored = true;
        if (wifi_initialized) {
            wifi_restored = network::init();
            if (wifi_restored && wifi_connected) {
                wifi_restored = network::connect(
                    settings_.wifi_ssid, settings_.wifi_password);
                settings_.wifi_enabled = wifi_restored;
            }
        }
        if (bluetooth_enabled) {
            (void)bluetooth_manager::enable();
        }
        if (led_mode != system_controls::BoardLedMode::Off) {
            (void)wireless::set_board_led_mode(led_mode);
        }
        if (file_server_running && wifi_restored &&
            network::connected()) {
            (void)network::file_server_start();
        }
        return changed;
    };

    while (true) {
        draw_menu_header(
            "POWER / CPU",
            "UP/DOWN SELECT  ENTER APPLY  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 4;

        char row[96] = {};
        std::snprintf(
            row,
            sizeof(row),
            "Current CPU        %lu MHz",
            static_cast<unsigned long>(
                platform::system_clock_hz() / 1000000u
            )
        );
        draw_menu_message(first_row, row);

        for (int i = 0; i < 4; ++i) {
            char option[64] = {};
            std::snprintf(
                option,
                sizeof(option),
                "%s%s",
                names[i],
                (platform::system_clock_hz() / 1000000u) == profiles[i]
                    ? "  *" : ""
            );
            draw_menu_option(first_row + 2 + i, option, selected == i);
        }

        draw_menu_option(first_row + 7, "STANDBY NOW", selected == 4);
        draw_menu_message(
            first_row + 10,
            "200 MHz is session-only and applies immediately."
        );
        draw_menu_message(
            first_row + 11,
            "Wi-Fi/Bluetooth are restarted automatically if needed."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 4) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key_is_enter(key)) {
            if (selected < 4) {
                const std::uint16_t requested = profiles[selected];
                if (usb_msc::active()) {
                    draw_menu_message(
                        first_row + 13,
                        "EJECT USB STORAGE BEFORE CPU CHANGE"
                    );
                    platform::sleep_millis(1000);
                    continue;
                }

                if (apply_cpu_profile(requested)) {
                    settings_.cpu_mhz = requested;
                } else {
                    draw_menu_message(
                        first_row + 13,
                        "CPU CLOCK CHANGE FAILED"
                    );
                    platform::sleep_millis(900);
                }
            } else {
                if (!storage::firmware_owns_card()) {
                    draw_menu_message(
                        first_row + 13,
                        "EJECT USB STORAGE BEFORE STANDBY"
                    );
                    platform::sleep_millis(900);
                } else {
                    platform::enter_sleep_mode();
                }
                draw_menu_header(nullptr, nullptr);
            }
        }
    }
}

void Repl::menu_console() {
    int selected = 1;
    if (settings_.console_mode == platform::ConsoleMode::Lcd) {
        selected = 0;
    } else if (settings_.console_mode == platform::ConsoleMode::Serial) {
        selected = 2;
    }

    while (true) {
        draw_menu_header(
            "CONSOLE",
            "UP/DOWN SELECT  ENTER APPLY  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 4;

        draw_menu_option(first_row, "LCD ONLY", selected == 0);
        draw_menu_option(first_row + 1, "LCD + SERIAL", selected == 1);
        draw_menu_option(first_row + 2, "SERIAL ONLY", selected == 2);
        draw_menu_message(
            first_row + 5,
            "Physical HOME remains active in SERIAL ONLY mode."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 2) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key_is_enter(key)) {
            settings_.console_mode =
                selected == 0 ? platform::ConsoleMode::Lcd :
                selected == 2 ? platform::ConsoleMode::Serial :
                platform::ConsoleMode::Both;
            save_settings();
            return;
        }
    }
}

bool Repl::prompt_text(
    const char* prompt,
    char* output,
    std::size_t capacity,
    const char* initial
) {
    if (!output || capacity == 0) return false;

    // Text entry temporarily replaces the menu screen.  Invalidate the cached
    // menu header so returning from the editor performs one clean redraw.
    draw_menu_header(nullptr, nullptr);
    platform::clear_lcd_color(0x000000);
    render_status();
    platform::set_cursor_position(
        0,
        (settings_.status_enabled ? console_layout::status_rows : 0) + 2
    );

    // Menu text-entry screens are always white on black, regardless of the
    // selected BASIC console theme. Restore the console colors afterwards.
    platform::set_text_color(0xffffff, 0x000000);
    platform::put_string(prompt ? prompt : "");
    const std::size_t length = LineEditor::read(
        output, capacity, nullptr, initial);
    platform::set_text_color(
        settings_.console_foreground,
        settings_.console_background
    );

    trim_right(output);
    return length > 0;
}

bool Repl::pick_wifi_network(
    char* ssid,
    std::size_t capacity,
    bool& secure
) {
    if (!ssid || capacity == 0) return false;

    draw_menu_header("WIRELESS LAN", "SCANNING...");
    draw_menu_message(
        (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
        "Searching for access points..."
    );

    network::AccessPoint access_points[24];
    const int count = network::scan(access_points, 24);
    if (count <= 0) {
        draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
        draw_menu_message(
            (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
            network::last_error()
        );
        platform::get_char();
        return false;
    }

    constexpr int visible = 18;
    menu_scroll::State scroll;
    menu_scroll::normalize(scroll, count, visible);

    while (true) {
        draw_menu_header(
            "SELECT WIRELESS NETWORK",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER USE  ESC"
        );

        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        menu_scroll::normalize(scroll, count, visible);

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int i = scroll.offset + row_index;
            char row[96] = {};
            if (i < count) {
                std::snprintf(
                    row,
                    sizeof(row),
                    "%c %-32.32s %4d dBm",
                    access_points[i].secure ? '*' : ' ',
                    access_points[i].ssid,
                    access_points[i].rssi
                );
            }
            draw_menu_option(
                first_row + row_index,
                row,
                i == scroll.selected
            );
        }

        char position[64] = {};
        std::snprintf(
            position, sizeof(position),
            "* = secured   %d-%d / %d",
            scroll.offset + 1,
            menu_scroll::last_exclusive(scroll, count, visible),
            count
        );
        draw_menu_message(first_row + visible + 1, position);

        const int key = platform::get_char();
        if (handle_menu_scroll_key(key, scroll, count, visible)) {
            continue;
        }
        if (key == kKeyEscape || key == 0x1b) {
            return false;
        }
        if (key_is_enter(key)) {
            std::snprintf(
                ssid,
                capacity,
                "%s",
                access_points[scroll.selected].ssid
            );
            secure = access_points[scroll.selected].secure;
            return true;
        }
    }
}

bool Repl::sync_rtc_from_network(bool show_result) {
    network::NetworkDateTime ntp;
    if (!network::ntp_time(
            ntp,
            settings_.wifi_timezone_minutes,
            settings_.wifi_ntp_server)) {
        if (show_result) {
            platform::put_string("NTP: ");
            platform::put_string(network::last_error());
            platform::put_string("\r\n");
        }
        return false;
    }

    platform::DateTime value;
    value.year = ntp.year;
    value.month = ntp.month;
    value.day = ntp.day;
    value.hour = ntp.hour;
    value.minute = ntp.minute;
    value.second = ntp.second;

    platform::set_datetime(value);

    if (show_result) {
        char line[128] = {};
        std::snprintf(
            line,
            sizeof(line),
            "NTP: %04d-%02d-%02d %02d:%02d:%02d\r\n",
            value.year,
            value.month,
            value.day,
            value.hour,
            value.minute,
            value.second
        );
        platform::put_string(line);
    }

    return true;
}

void Repl::apply_network_settings() {
    if (!settings_.wifi_enabled || settings_.wifi_ssid[0] == '\0') {
        if (network::initialized()) {
            network::disconnect();
        }
        return;
    }

    platform::put_string("WiFi: connecting to ");
    platform::put_string(settings_.wifi_ssid);
    platform::put_string("...\r\n");

    if (!network::connect(
            settings_.wifi_ssid,
            settings_.wifi_password)) {
        platform::put_string("WiFi: ");
        platform::put_string(network::last_error());
        platform::put_string("\r\n");
        return;
    }

    char ip[32] = {};
    platform::put_string("WiFi: connected");
    if (network::get_ip(ip, sizeof(ip))) {
        platform::put_string("  ");
        platform::put_string(ip);
    }
    platform::put_string("\r\n");

    if (settings_.wifi_auto_rtc) {
        sync_rtc_from_network(true);
    }
}

void Repl::menu_wifi() {
    int selected = 0;

    while (true) {
        draw_menu_header(
            "WIRELESS LAN",
            "UP/DOWN SELECT  ENTER ACTION  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        char row[96] = {};
        std::snprintf(
            row,
            sizeof(row),
            "Enabled    : %s",
            settings_.wifi_enabled ? "YES" : "NO"
        );
        draw_menu_message(first_row, row);

        std::snprintf(
            row,
            sizeof(row),
            "SSID       : %s",
            settings_.wifi_ssid[0] ? settings_.wifi_ssid : "(not set)"
        );
        draw_menu_message(first_row + 1, row);

        std::snprintf(
            row,
            sizeof(row),
            "Link       : %s",
            network::connected() ? "CONNECTED" : "DISCONNECTED"
        );
        draw_menu_message(first_row + 2, row);

        char ip[32] = {};
        std::snprintf(
            row,
            sizeof(row),
            "IP         : %s",
            network::get_ip(ip, sizeof(ip)) ? ip : "-"
        );
        draw_menu_message(first_row + 3, row);

        std::snprintf(
            row,
            sizeof(row),
            "Auto Time  : %s   TZ: UTC%+d:%02d",
            settings_.wifi_auto_rtc ? "ON" : "OFF",
            settings_.wifi_timezone_minutes / 60,
            std::abs(settings_.wifi_timezone_minutes % 60)
        );
        draw_menu_message(first_row + 4, row);

        std::snprintf(
            row,
            sizeof(row),
            "NTP Server : %.60s",
            settings_.wifi_ntp_server
        );
        draw_menu_message(first_row + 5, row);

        draw_menu_option(
            first_row + 8,
            settings_.wifi_enabled ? "Disable Wi-Fi" : "Enable Wi-Fi",
            selected == 0
        );
        draw_menu_option(
            first_row + 9,
            "Scan & select SSID",
            selected == 1
        );
        draw_menu_option(
            first_row + 10,
            "Connect now",
            selected == 2
        );
        draw_menu_option(
            first_row + 11,
            settings_.wifi_auto_rtc ? "Auto Time: ON" : "Auto Time: OFF",
            selected == 3
        );
        draw_menu_option(
            first_row + 12,
            "Set NTP server",
            selected == 4
        );
        draw_menu_option(
            first_row + 13,
            "Sync time from NTP now",
            selected == 5
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
            continue;
        }
        if (key == kKeyDown && selected < 5) {
            ++selected;
            continue;
        }
        if (key == kKeyEscape || key == 0x1b) {
            save_settings();
            return;
        }
        if (!key_is_enter(key)) continue;

        if (selected == 0) {
            settings_.wifi_enabled = !settings_.wifi_enabled;
            save_settings();

            if (!settings_.wifi_enabled) {
                network::disconnect();
            } else if (settings_.wifi_ssid[0] != '\0') {
                draw_menu_header("WIRELESS LAN", "CONNECTING...");
                draw_menu_message(
                    (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                    settings_.wifi_ssid
                );

                const bool ok = network::connect(
                    settings_.wifi_ssid,
                    settings_.wifi_password
                );
                if (ok && settings_.wifi_auto_rtc) {
                    sync_rtc_from_network(false);
                }

                draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
                draw_menu_message(
                    (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                    ok ? "CONNECTED" : network::last_error()
                );
                platform::get_char();
            }
        } else if (selected == 1) {
            char chosen[33] = {};
            bool secure = true;
            if (!pick_wifi_network(chosen, sizeof(chosen), secure)) {
                continue;
            }

            char password[64] = {};
            if (secure) {
                if (!prompt_text(
                        "WIFI PASSWORD: ",
                        password,
                        sizeof(password))) {
                    continue;
                }
            }

            std::snprintf(
                settings_.wifi_ssid,
                sizeof(settings_.wifi_ssid),
                "%s",
                chosen
            );
            std::snprintf(
                settings_.wifi_password,
                sizeof(settings_.wifi_password),
                "%s",
                password
            );
            settings_.wifi_enabled = true;
            save_settings();

            draw_menu_header("WIRELESS LAN", "CONNECTING...");
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                settings_.wifi_ssid
            );

            const bool ok = network::connect(
                settings_.wifi_ssid,
                settings_.wifi_password
            );

            if (ok && settings_.wifi_auto_rtc) {
                sync_rtc_from_network(false);
            }

            draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                ok ? "CONNECTED" : network::last_error()
            );
            if (ok) {
                char addr[32] = {};
                if (network::get_ip(addr, sizeof(addr))) {
                    draw_menu_message(
                        (settings_.status_enabled ? console_layout::status_rows : 0) + 7,
                        addr
                    );
                }
            }
            platform::get_char();
        } else if (selected == 2) {
            if (settings_.wifi_ssid[0] == '\0') {
                draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
                draw_menu_message(
                    (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                    "SSID NOT SET - SCAN FIRST"
                );
                platform::get_char();
                continue;
            }

            draw_menu_header("WIRELESS LAN", "CONNECTING...");
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                settings_.wifi_ssid
            );
            const bool ok = network::connect(
                settings_.wifi_ssid,
                settings_.wifi_password
            );
            if (ok && settings_.wifi_auto_rtc) {
                sync_rtc_from_network(false);
            }
            draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                ok ? "CONNECTED" : network::last_error()
            );
            platform::get_char();
        } else if (selected == 3) {
            settings_.wifi_auto_rtc = !settings_.wifi_auto_rtc;
            save_settings();

            if (settings_.wifi_auto_rtc && network::connected()) {
                draw_menu_header("WIRELESS LAN", "NTP SYNC...");
                const bool ok = sync_rtc_from_network(false);
                draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
                draw_menu_message(
                    (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                    ok ? "NTP TIME APPLIED" : network::last_error()
                );
                platform::get_char();
            }
        } else if (selected == 4) {
            char server[64] = {};
            if (!prompt_text(
                    "NTP SERVER (HOST OR IP): ",
                    server,
                    sizeof(server))) {
                continue;
            }

            std::snprintf(
                settings_.wifi_ntp_server,
                sizeof(settings_.wifi_ntp_server),
                "%s",
                server
            );
            save_settings();
        } else {
            draw_menu_header("WIRELESS LAN", "NTP SYNC...");
            const bool ok = sync_rtc_from_network(false);
            draw_menu_header("WIRELESS LAN", "PRESS ANY KEY");
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5,
                ok ? "NTP TIME APPLIED" : network::last_error()
            );
            draw_menu_message(
                (settings_.status_enabled ? console_layout::status_rows : 0) + 7,
                settings_.wifi_ntp_server
            );
            platform::get_char();
        }
    }
}

void Repl::menu_file_server() {
    int selected = 0;
    while (true) {
        draw_menu_header(
            "WI-FI FILE SERVER",
            "ENTER START/STOP  ESC BACK"
        );
        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;
        char row[96] = {};
        std::snprintf(row, sizeof(row), "Wi-Fi : %s",
            network::connected() ? "CONNECTED" : "NOT CONNECTED");
        draw_menu_message(first_row, row);
        std::snprintf(row, sizeof(row), "HTTP  : %s",
            network::file_server_running() ? "RUNNING" : "STOPPED");
        draw_menu_message(first_row + 1, row);
        char ip[32] = {};
        std::snprintf(row, sizeof(row), "IP    : %s",
            network::get_ip(ip, sizeof(ip)) ? ip : "-");
        draw_menu_message(first_row + 2, row);
        draw_menu_message(first_row + 3, "Port  : 80");
        if (network::file_server_running() && ip[0]) {
            std::snprintf(row, sizeof(row), "URL   : http://%s/?k=%s",
                ip, network::file_server_token());
            draw_menu_message(first_row + 5, row);
        } else {
            draw_menu_message(first_row + 5, "URL   : -");
        }
        draw_menu_option(first_row + 8,
            network::file_server_running() ? "Stop File Server" : "Start File Server",
            selected == 0);

        const int key = platform::get_char();
        if (key == kKeyEscape || key == 0x1b) return;
        if (!key_is_enter(key)) continue;
        if (network::file_server_running()) {
            network::file_server_stop();
        } else if (!network::connected()) {
            draw_menu_header("WI-FI FILE SERVER", "PRESS ANY KEY");
            draw_menu_message(first_row + 3, "Wi-Fi is not connected.");
            platform::get_char();
        } else if (!network::file_server_start()) {
            draw_menu_header("WI-FI FILE SERVER", "PRESS ANY KEY");
            draw_menu_message(first_row + 3, network::file_server_last_error());
            platform::get_char();
        }
    }
}

void Repl::menu_datetime() {
    int selected = 0;
    while (true) {
        platform::DateTime dt;
        platform::get_datetime(dt);

        draw_menu_header(
            "RTC SETTINGS",
            "UP/DOWN SELECT  ENTER CHANGE  ESC BACK"
        );
        const int first =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        char row[80] = {};

        std::snprintf(
            row, sizeof(row), "Source        %s",
            platform::rtc_source_name());
        draw_menu_option(first, row, selected == 0);

        std::snprintf(
            row, sizeof(row), "Type          %s",
            platform::rtc_device_name());
        draw_menu_message(first + 1, row);

        std::snprintf(
            row, sizeof(row), "Target        0x%02X",
            static_cast<unsigned>(platform::rtc_address()));
        draw_menu_option(first + 2, row, selected == 1);

        std::snprintf(
            row, sizeof(row), "Active        %s 0x%02X",
            platform::rtc_location(),
            static_cast<unsigned>(platform::rtc_active_address()));
        draw_menu_message(first + 3, row);

        draw_menu_message(first + 5, "External SDA  GP4");
        draw_menu_message(first + 6, "External SCL  GP5");
        draw_menu_message(first + 7, "I2C Speed     100 kHz");
        draw_menu_option(first + 9, "Probe RTC", selected == 2);
        draw_menu_option(first + 10, "Set date & time", selected == 3);
        draw_menu_option(first + 11, "Back", selected == 4);

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
            continue;
        }
        if (key == kKeyDown && selected < 4) {
            ++selected;
            continue;
        }
        if (key == kKeyEscape || key == 0x1b ||
            (key_is_enter(key) && selected == 4)) {
            return;
        }
        if (!key_is_enter(key)) continue;

        if (selected == 0) {
            const int current =
                platform::rtc_source() == platform::RtcSource::Auto ? 0 :
                platform::rtc_source() == platform::RtcSource::External ? 1 :
                platform::rtc_source() == platform::RtcSource::Internal ? 2 : 3;
            platform::set_rtc_source(
                static_cast<platform::RtcSource>((current + 1) % 4));
            save_settings();
        } else if (selected == 1) {
            char input[16] = {};
            if (prompt_text(
                    "RTC target address (08-77 hex): ",
                    input, sizeof(input))) {
                char* parse_end = nullptr;
                const long address = std::strtol(input, &parse_end, 16);
                if (!platform::set_rtc_address(
                        static_cast<std::uint8_t>(address))) {
                    platform::put_string("BAD I2C ADDRESS\r\n");
                } else {
                    save_settings();
                }
            }
        } else if (selected == 2) {
            const bool found = platform::probe_rtc();
            draw_menu_header("RTC PROBE", "PRESS ANY KEY");
            if (found) {
                std::snprintf(
                    row, sizeof(row), "%s FOUND %s 0x%02X",
                    platform::rtc_device_name(),
                    platform::rtc_location(),
                    static_cast<unsigned>(platform::rtc_active_address()));
                draw_menu_message(first + 4, row);
            } else {
                draw_menu_message(first + 4, "RTC NOT FOUND");
            }
            platform::get_char();
        } else {
            char input[48] = {};
            if (prompt_text(
                    "SET YYYYMMDDHHMMSS: ", input, sizeof(input))) {
                platform::DateTime value;
                if (parse_datetime_value(input, value) &&
                    platform::set_datetime(value)) {
                    platform::put_string("CLOCK UPDATED\r\n");
                } else {
                    platform::put_string("INVALID DATE/TIME\r\n");
                }
            }
        }
    }
}

void Repl::menu_audio() {
    int selected = 0;
    while (true) {
        draw_menu_header(
            "AUDIO SETTINGS",
            "UP/DOWN SELECT  ENTER CHANGE  ESC BACK"
        );
        const int first =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
        char row[64] = {};
        if (settings_.audio_volume == 0) {
            std::snprintf(row, sizeof(row), "Master       OFF");
        } else {
            std::snprintf(
                row, sizeof(row), "Master       %u%%",
                static_cast<unsigned>(settings_.audio_volume)
            );
        }
        draw_menu_option(first, row, selected == 0);
        std::snprintf(
            row, sizeof(row), "WAV/MP3      %u%%",
            static_cast<unsigned>(settings_.wav_volume)
        );
        draw_menu_option(first + 1, row, selected == 1);
        if (settings_.play_volume == 0) {
            std::snprintf(row, sizeof(row), "PLAY         OFF");
        } else {
            std::snprintf(
                row, sizeof(row), "PLAY         %u%%",
                static_cast<unsigned>(settings_.play_volume)
            );
        }
        draw_menu_option(first + 2, row, selected == 2);
        std::snprintf(
            row, sizeof(row), "Startup WAV  %s",
            settings_.startup_wav ? "ON" : "OFF"
        );
        draw_menu_option(first + 3, row, selected == 3);
        std::snprintf(row, sizeof(row), "Key Click    %s",
                      audio::key_click_name(settings_.key_click));
        draw_menu_option(first + 4, row, selected == 4);
        draw_menu_option(first + 6, "Back", selected == 5);

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 5) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b ||
                   (key_is_enter(key) && selected == 5)) {
            return;
        } else if (key_is_enter(key) && selected == 0) {
            settings_.audio_volume =
                static_cast<std::uint8_t>(
                    settings_.audio_volume >= 100
                        ? 0 : settings_.audio_volume + 10
                );
            platform::audio_set_volume(settings_.audio_volume);
            save_settings();
        } else if (key_is_enter(key) && selected == 1) {
            settings_.wav_volume =
                static_cast<std::uint8_t>(
                    settings_.wav_volume >= 100
                        ? 10 : settings_.wav_volume + 10
                );
            platform::audio_set_wav_volume(settings_.wav_volume);
            save_settings();
        } else if (key_is_enter(key) && selected == 2) {
            settings_.play_volume =
                static_cast<std::uint8_t>(
                    settings_.play_volume >= 100
                        ? 0 : settings_.play_volume + 10
                );
            platform::audio_set_play_volume(settings_.play_volume);
            save_settings();
        } else if (key_is_enter(key) && selected == 3) {
            settings_.startup_wav = !settings_.startup_wav;
            save_settings();
        } else if (key_is_enter(key) && selected == 4) {
            settings_.key_click = static_cast<audio::KeyClickMode>(
                (static_cast<unsigned>(settings_.key_click) + 1u) % 4u);
            platform::audio_set_key_click(settings_.key_click);
            save_settings();
        }
    }
}

bool Repl::recover_after_sd_remount() {
    const bool remounted = storage::remount();
    if (!storage_recovery::reload_after_remount(remounted)) return false;

    // A successful remount follows the normal boot recovery order.
    load_settings();
    apply_settings();
    apply_network_settings();

    const auto action = storage_recovery::program_action(
        true, settings_.storage_mode, program_.backend_type(), program_.is_dirty());

    if (action == storage_recovery::ProgramAction::UseInternalRam) {
        if (!program_.switch_mode(ProgramStorageMode::InternalRam, false))
            return false;
    } else if (action == storage_recovery::ProgramAction::AskDirtyRam) {
        int selected = 0; // Keep is deliberately the safe default.
        while (true) {
            draw_menu_header(
                "SD SESSION FOUND",
                "UP/DOWN SELECT  ENTER  ESC CANCEL"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            draw_menu_message(top, "Current RAM program is modified.");
            draw_menu_option(top + 3, "Keep current program", selected == 0);
            draw_menu_option(top + 4, "Restore SD session", selected == 1);
            draw_menu_option(top + 5, "Cancel", selected == 2);
            const int key = platform::get_char();
            if (key == kKeyUp && selected > 0) --selected;
            else if (key == kKeyDown && selected < 2) ++selected;
            else if (key == kKeyEscape || key == 0x1b) return true;
            else if (key_is_enter(key)) {
                if (selected == 2) return true;
                if (selected == 0) {
                    if (!program_.switch_mode(settings_.storage_mode, false))
                        return false;
                } else if (!program_.recover_session(
                               settings_.storage_mode, true)) {
                    return false;
                }
                break;
            }
        }
    } else if (action == storage_recovery::ProgramAction::RecoverSession) {
        if (!program_.recover_session(settings_.storage_mode, true)) {
            // No valid session is normal on a fresh card. Preserve the current
            // clean program while creating a new transactional SD workspace.
            if (program_.backend_type() == ProgramBackend::Ram) {
                if (!program_.switch_mode(settings_.storage_mode, false))
                    return false;
            } else if (!program_.resume()) {
                return false;
            }
        }
    }

    set_current_filename(
        *program_.filename() ? program_.filename() : "UNTITLED",
        program_.is_dirty()
    );
    render_status();
    render_function_keys();
    return true;
}

void Repl::menu_sd() {
    int selected = 0;

    while (true) {
        draw_menu_header(
            "SD CARD",
            "UP/DOWN SELECT  ENTER ACTION  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        char row[96] = {};
        std::snprintf(
            row,
            sizeof(row),
            "Card detect : %s",
            storage::card_present() ? "PRESENT" : "NOT PRESENT"
        );
        draw_menu_message(first_row, row);

        std::snprintf(
            row,
            sizeof(row),
            "Mounted     : %s",
            storage::available() ? "YES" : "NO"
        );
        draw_menu_message(first_row + 1, row);

        std::snprintf(
            row,
            sizeof(row),
            "Last status : %s",
            storage::last_error()
        );
        draw_menu_message(first_row + 2, row);

        draw_menu_option(first_row + 5, "Refresh status", selected == 0);
        draw_menu_option(first_row + 6, "Remount SD card", selected == 1);
        draw_menu_option(first_row + 7, "Reload RMBASIC.CFG", selected == 2);

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 2) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key_is_enter(key)) {
            if (selected == 0) {
                storage::init();
            } else if (selected == 1) {
                recover_after_sd_remount();
            } else {
                load_settings();
                apply_settings();
                apply_network_settings();
                render_status();
                render_function_keys();
            }
        }
    }
}



void Repl::menu_board_led() {
    using system_controls::BoardLedMode;
    static const BoardLedMode modes[] = {
        BoardLedMode::Off,
        BoardLedMode::On,
        BoardLedMode::Heartbeat
    };
    static const char* names[] = {"OFF", "ON", "HEARTBEAT"};

    int selected = static_cast<int>(settings_.board_led);
    while (true) {
        draw_menu_header(
            "BOARD LED",
            "UP/DOWN SELECT  ENTER APPLY  ESC BACK"
        );
        const int top =
            settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 4;

        for (int i = 0; i < 3; ++i) {
            char option[48] = {};
            std::snprintf(
                option,
                sizeof(option),
                "%-12s%s",
                names[i],
                settings_.board_led == modes[i] ? "*" : ""
            );
            draw_menu_option(first_row + i, option, selected == i);
        }
        draw_menu_message(
            first_row + 6,
            "Pico 2 W LED uses the shared CYW43 device."
        );
        draw_menu_message(
            first_row + 7,
            "HEARTBEAT: 120 ms pulse every second."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 2) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key_is_enter(key)) {
            if (wireless::set_board_led_mode(modes[selected])) {
                settings_.board_led = modes[selected];
                // Persist immediately: Firmware -> Reboot can leave the
                // Control Center without reaching its normal exit save.
                save_settings();
            } else {
                draw_menu_message(first_row + 10, wireless::last_error());
                platform::sleep_millis(900);
            }
        }
    }
}

void Repl::menu_system_info() {
    while (true) {
        draw_menu_header(
            "SYSTEM INFORMATION",
            "ENTER/ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        int row = top + 3;

        char text[96] = {};
        std::snprintf(
            text,
            sizeof(text),
            "Cala's Pokecom BASIC  v%s",
            RMB_VERSION
        );
        draw_menu_message(row++, text);

        const auto& psram_info = psram::info();
        if (psram_info.available) {
            std::snprintf(
                text,
                sizeof(text),
                "PicoCalc PSRAM  %lu KiB / PIO1 SM%d / %lu MHz",
                static_cast<unsigned long>(psram_info.size_bytes / 1024u),
                psram_info.pio_state_machine,
                static_cast<unsigned long>(
                    psram_info.bus_clock_hz / 1000000u)
            );
        } else {
            std::snprintf(
                text,
                sizeof(text),
                "PicoCalc PSRAM  NOT AVAILABLE (%s)",
                psram_info.error
            );
        }
        draw_menu_message(row++, text);
        const char* psram_owner = psram::owner() == psram::Client::EditorHistory
            ? "EDITOR HISTORY"
            : psram::owner() == psram::Client::Diagnostic
                ? "DIAGNOSTIC" : "IDLE";
        std::snprintf(
            text,
            sizeof(text),
            "PSRAM runtime   %lu KiB / %s",
            static_cast<unsigned long>(psram::used_bytes() / 1024u),
            psram_owner
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Build           %s",
            RMB_BUILD_NUMBER
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "CPU clock       %lu MHz",
            static_cast<unsigned long>(
                platform::system_clock_hz() / 1000000u
            )
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Uptime          %lu sec",
            static_cast<unsigned long>(
                platform::monotonic_millis() / 1000u
            )
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Program         %s%s",
            current_filename_,
            program_dirty_ ? " *" : ""
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Program lines   %lu / %lu",
            static_cast<unsigned long>(program_.size()),
            static_cast<unsigned long>(program_.backend_type()==ProgramBackend::Sd ? kMaxSdProgramLines : kMaxRamProgramLines)
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Line capacity   %lu chars",
            static_cast<unsigned long>(
                program_.backend_type()==ProgramBackend::Sd
                    ?kMaxSdProgramLineLength-1
                    :kMaxProgramLineLength-1)
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Console         %s",
            console_name(settings_.console_mode)
        );
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "SD              %s / %s",
            storage::card_present() ? "PRESENT" : "ABSENT",
            storage::available() ? "MOUNTED" : "NOT MOUNTED"
        );
        draw_menu_message(row++, text);

        int battery = -1;
        bool charging = false;
        if (platform::get_battery_status(battery, charging)) {
            std::snprintf(
                text,
                sizeof(text),
                "Battery         %d%% %s",
                battery,
                charging ? "CHARGING" : ""
            );
        } else {
            std::snprintf(text, sizeof(text), "Battery         N/A");
        }
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Wi-Fi           %s%s%s",
            settings_.wifi_enabled ? "ON" : "OFF",
            network::connected() ? " / CONNECTED / " : "",
            network::connected() ? network::current_ssid() : ""
        );
        draw_menu_message(row++, text);

        platform::DateTime dt;
        if (platform::get_datetime(dt)) {
            std::snprintf(
                text,
                sizeof(text),
                "RTC             %04d-%02d-%02d %02d:%02d:%02d",
                dt.year,
                dt.month,
                dt.day,
                dt.hour,
                dt.minute,
                dt.second
            );
        } else {
            std::snprintf(text, sizeof(text), "RTC             N/A");
        }
        draw_menu_message(row++, text);

        std::snprintf(
            text,
            sizeof(text),
            "Last RUN        %lu ms",
            static_cast<unsigned long>(last_run_ms_)
        );
        draw_menu_message(row++, text);

        const int key = platform::get_char();
        if (key_is_enter(key) ||
            key == kKeyEscape ||
            key == 0x1b ||
            key == kKeyHome) {
            return;
        }
    }
}

void Repl::menu_psram_diagnostics() {
    static const char* items[] = {
        "Quick test (1 MiB)",
        "Full detected capacity",
        "Full stress (3 passes)",
        "Re-run hardware probe",
        "Back"
    };
    int selected = 0;
    while (true) {
        draw_menu_header(
            "PICOCALC PSRAM DIAGNOSTICS",
            "UP/DOWN SELECT  ENTER RUN  ESC BACK"
        );
        const int top = settings_.status_enabled
            ? console_layout::status_rows : 0;
        const int first_row = top + 4;
        const auto& device = psram::info();
        char text[80] = {};
        if (device.available) {
            std::snprintf(
                text, sizeof(text),
                "%lu KiB, profile %d, ID %02X %02X %02X, %lu MHz",
                static_cast<unsigned long>(device.size_bytes / 1024u),
                device.selected_probe_attempt + 1,
                device.manufacturer_id,
                device.known_good_die,
                device.electronic_id,
                static_cast<unsigned long>(device.bus_clock_hz / 1000000u));
        } else {
            std::snprintf(text, sizeof(text), "Unavailable: %s", device.error);
        }
        draw_menu_message(first_row, text);
        draw_menu_message(
            first_row + 1, "Probe: F=falling/fudge, N=normal, read=0B/03");
        for (std::size_t i = 0; i < psram::kMaximumProbeAttempts; ++i) {
            if (i >= device.probe_attempt_count) {
                draw_menu_message(
                    first_row + 2 + static_cast<int>(i), "");
                continue;
            }
            const auto& attempt = device.probe_attempts[i];
            std::snprintf(
                text, sizeof(text),
                "%c%lu %2luM %c %s I:%02X%02X%02X R:%02X%02X%02X%02X %s",
                device.selected_probe_attempt == static_cast<int>(i) ? '*' : ' ',
                static_cast<unsigned long>(i + 1),
                static_cast<unsigned long>(attempt.bus_clock_hz / 1000000u),
                attempt.falling_edge_fudge ? 'F' : 'N',
                attempt.fast_read ? "0B" : "03",
                attempt.manufacturer_id,
                attempt.known_good_die,
                attempt.electronic_id,
                attempt.readback[0],
                attempt.readback[1],
                attempt.readback[2],
                attempt.readback[3],
                attempt.passed ? "PASS" : "FAIL");
            draw_menu_message(
                first_row + 2 + static_cast<int>(i), text);
        }
        const int option_row = first_row + 11;
        for (int i = 0; i < 5; ++i)
            draw_menu_option(option_row + i, items[i], selected == i);
        draw_menu_message(
            option_row + 6,
            "Probe touches only the final 16 PSRAM bytes and restores them."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) { --selected; continue; }
        if (key == kKeyDown && selected < 4) { ++selected; continue; }
        if (key == kKeyEscape || key == 0x1b ||
            (key_is_enter(key) && selected == 4)) return;
        if (!key_is_enter(key)) continue;
        if (selected == 3) {
            draw_menu_message(option_row + 7, "Probing PicoCalc PSRAM...");
            (void)psram::reprobe();
            continue;
        }
        if (!device.available) {
            draw_menu_message(option_row + 7, device.error);
            platform::get_char();
            continue;
        }

        draw_menu_header(
            selected == 0 ? "RUN QUICK PSRAM TEST?" :
            selected == 1 ? "RUN FULL PSRAM TEST?" :
                            "RUN 3-PASS PSRAM STRESS?",
            "ENTER CONFIRM  ESC CANCEL"
        );
        draw_menu_message(
            first_row + 1,
            "PSRAM contents will be overwritten with test patterns."
        );
        draw_menu_message(
            first_row + 2,
            selected == 0
                ? "Range: first 1 MiB."
                : "Range: all detected PSRAM; this can take a while."
        );
        if (!key_is_enter(platform::get_char())) continue;

        platform::clear_lcd_color(0x000000);
        platform::draw_text_row(
            0, "PSRAM DIAGNOSTIC RUNNING", 0xffffff, 0x203060);
        auto progress = [](
            const char* stage,
            std::uint32_t completed,
            std::uint32_t total,
            void*
        ) {
            char line[80] = {};
            const unsigned percent = total == 0 ? 0 :
                static_cast<unsigned>((
                    static_cast<std::uint64_t>(completed) * 100u) / total);
            std::snprintf(
                line, sizeof(line), "%-20s %3u%%", stage, percent);
            platform::draw_text_row(5, line, 0xffff80, 0x000000);
        };
        psram::DiagnosticResult result;
        const std::uint32_t requested = selected == 0
            ? 1024u * 1024u : device.size_bytes;
        const int requested_passes = selected == 2 ? 3 : 1;
        int completed_passes = 0;
        for (; completed_passes < requested_passes; ++completed_passes) {
            if (!psram::run_diagnostic(
                    requested, result, progress, nullptr)) break;
        }

        platform::clear_lcd_color(0x000000);
        platform::draw_text_row(
            0,
            result.passed ? "PSRAM DIAGNOSTIC: PASS"
                          : "PSRAM DIAGNOSTIC: FAIL",
            result.passed ? 0x80ff80 : 0xff8080,
            0x203060);
        std::snprintf(
            text, sizeof(text), "Range           %lu KiB",
            static_cast<unsigned long>(result.tested_bytes / 1024u));
        platform::draw_text_row(4, text, 0xffffff, 0x000000);
        std::snprintf(
            text, sizeof(text), "Passes          %d / %d",
            completed_passes, requested_passes);
        platform::draw_text_row(5, text, 0xffffff, 0x000000);
        std::snprintf(
            text, sizeof(text), "Write           %lu KiB/s (%lu us)",
            static_cast<unsigned long>(result.write_bytes_per_second / 1024u),
            static_cast<unsigned long>(result.write_microseconds));
        platform::draw_text_row(6, text, 0xffffff, 0x000000);
        std::snprintf(
            text, sizeof(text), "Read            %lu KiB/s (%lu us)",
            static_cast<unsigned long>(result.read_bytes_per_second / 1024u),
            static_cast<unsigned long>(result.read_microseconds));
        platform::draw_text_row(7, text, 0xffffff, 0x000000);
        std::snprintf(
            text, sizeof(text), "CRC32           %08lX / %08lX",
            static_cast<unsigned long>(result.expected_crc32),
            static_cast<unsigned long>(result.actual_crc32));
        platform::draw_text_row(8, text, 0xffffff, 0x000000);
        std::snprintf(
            text, sizeof(text), "Errors          %lu  Stage: %s",
            static_cast<unsigned long>(result.error_count),
            result.failed_stage);
        platform::draw_text_row(9, text, 0xffffff, 0x000000);
        if (!result.passed && result.error_count != 0) {
            std::snprintf(
                text, sizeof(text), "First failure   0x%06lX",
                static_cast<unsigned long>(result.first_failure));
            platform::draw_text_row(10, text, 0xffffff, 0x000000);
        }
        platform::draw_text_row(
            11, "Sequential benchmark (KiB/s):", 0xa0a0a0, 0x000000);
        for (std::size_t i = 0; i < result.benchmark_count; ++i) {
            const auto& sample = result.benchmarks[i];
            std::snprintf(
                text, sizeof(text), "%4lu KiB  W %7lu  R %7lu",
                static_cast<unsigned long>(sample.bytes / 1024u),
                static_cast<unsigned long>(
                    sample.sequential_write_bytes_per_second / 1024u),
                static_cast<unsigned long>(
                    sample.sequential_read_bytes_per_second / 1024u));
            platform::draw_text_row(
                12 + static_cast<int>(i), text, 0xffffff, 0x000000);
        }
        if (result.benchmark_count != 0) {
            const auto& sample =
                result.benchmarks[result.benchmark_count - 1];
            std::snprintf(
                text, sizeof(text), "Random 4B ops/s  W %lu  R %lu",
                static_cast<unsigned long>(
                    sample.random_write_operations_per_second),
                static_cast<unsigned long>(
                    sample.random_read_operations_per_second));
            platform::draw_text_row(18, text, 0xffffff, 0x000000);
        }
        platform::draw_text_row(
            21, "PRESS ANY KEY", 0xa0a0a0, 0x000000);
        platform::get_char();
    }
}

void Repl::menu_program_storage() {
    auto message = [&](const char* text) {
        draw_menu_header("PROGRAM STORAGE", "ENTER / ESC BACK");
        draw_menu_message((settings_.status_enabled ? console_layout::status_rows : 0)+4,text);
        while (true) { int k=platform::get_char(); if(key_is_enter(k)||k==kKeyEscape||k==0x1b) break; }
    };
    auto save_as = [&]() {
        char name[80] = {};
        if(!prompt_text("SAVE AS (BAS filename)",name,sizeof(name))) return false;
        if(storage::program_exists(name) && !confirm_program_overwrite(name)) return false;
        if(!save_program_as(name)) {message(storage::last_error());return false;}
        return true;
    };
    auto discard_confirm = [&]() {
        draw_menu_header("DISCARD CURRENT PROGRAM?", "D DISCARD   ESC CANCEL");
        draw_menu_message((settings_.status_enabled ? console_layout::status_rows : 0)+4,
                          "Saved BAS files will be kept.");
        while(true) {int k=platform::get_char(); if(k=='d'||k=='D') return true; if(k==kKeyEscape||k==0x1b) return false;}
    };
    int selected=0;
    const char* items[]={"Storage Mode","Save As","New Program","Resume SD Storage","Back"};
    while(true) {
        program_.ready();
        draw_menu_header("PROGRAM STORAGE", "UP/DOWN SELECT   ENTER   ESC BACK");
        int top=(settings_.status_enabled ? console_layout::status_rows : 0)+3;
        char row[96];
        std::snprintf(row,sizeof(row),"Mode : %s",ProgramStore::mode_name(program_.mode()));draw_menu_message(top,row);
        std::snprintf(row,sizeof(row),"Active : %s%s",program_.backend_type()==ProgramBackend::Sd?"SD CARD":"INTERNAL RAM",program_.suspended()?" (SUSPENDED)":"");draw_menu_message(top+1,row);
        std::snprintf(row,sizeof(row),"SD : %s",storage::card_present()?(storage::available()?"READY":"NOT MOUNTED"):"NOT AVAILABLE");draw_menu_message(top+2,row);
        std::snprintf(row,sizeof(row),"Program: %.38s%s",current_filename_,program_dirty_?"*":"");draw_menu_message(top+3,row);
        std::snprintf(row,sizeof(row),"Lines: %lu   Size: %lu bytes",static_cast<unsigned long>(program_.size()),static_cast<unsigned long>(program_.size_bytes()));draw_menu_message(top+4,row);
        for(int i=0;i<5;++i) draw_menu_option(top+7+i,items[i],selected==i);
        int k=platform::get_char();
        if(k==kKeyEscape||k==0x1b||k==kKeyHome) return;
        if(k==kKeyUp&&selected>0) --selected;
        if(k==kKeyDown&&selected<4) ++selected;
        if(!key_is_enter(k)) continue;
        if(selected==4) return;
        if(selected==1) {save_as();continue;}
        if(selected==2) {
            if(program_dirty_&&!discard_confirm()) continue;
            if(!program_.clear()) message(program_.error());
            else {vm_.clear_direct_state();set_current_filename("UNTITLED",false);}
            continue;
        }
        if(selected==3) {message(program_.resume()?"PROGRAM STORAGE READY":program_.error());continue;}
        int choice=static_cast<int>(program_.mode());bool cancelled=false;
        while(true) {
            draw_menu_header("STORAGE MODE", "UP/DOWN SELECT   ENTER   ESC CANCEL");
            for(int i=0;i<3;++i) draw_menu_option(top+i,ProgramStore::mode_name(static_cast<ProgramStorageMode>(i)),choice==i);
            k=platform::get_char();
            if(k==kKeyEscape||k==0x1b) {cancelled=true;break;}
            if(k==kKeyUp&&choice>0) --choice;
            if(k==kKeyDown&&choice<2) ++choice;
            if(key_is_enter(k)) break;
        }
        if(cancelled) continue;
        auto mode=static_cast<ProgramStorageMode>(choice);
        bool to_sd=mode==ProgramStorageMode::SdCard || (mode==ProgramStorageMode::Auto&&storage::available()&&storage::card_present());
        if(to_sd && (!storage::available()||!storage::card_present())) {message("SD CARD NOT AVAILABLE");continue;}
        bool discard=false;
        if(program_.suspended()) {
            if(to_sd) {message("Reinsert card; select Resume SD Storage.");continue;}
            if(!discard_confirm()) continue;
            discard=true;
        } else if(to_sd&&program_.backend_type()==ProgramBackend::Ram&&program_dirty_) {
            draw_menu_header("RAM PROGRAM HAS BEEN MODIFIED", "S SAVE   D DISCARD   ESC CANCEL");
            draw_menu_message(top,"Save to SD card before switching?");
            while(true) {
                k=platform::get_char();
                if(k=='s'||k=='S') {cancelled=!save_as();break;}
                if(k=='d'||k=='D') {discard=true;break;}
                if(k==kKeyEscape||k==0x1b) {cancelled=true;break;}
            }
            if(cancelled) continue;
        }
        if(!program_.switch_mode(mode,discard)) {message(program_.error());continue;}
        settings_.storage_mode=mode;
        if(discard) {set_current_filename("UNTITLED",false);vm_.clear_direct_state();}
        save_settings();
    }
}

bool Repl::confirm_program_overwrite(const char* filename) {
    int selected = 0; // Cancel is intentionally the safe default.
    while (true) {
        draw_menu_header(
            "PROGRAM FILE ALREADY EXISTS",
            "UP/DOWN SELECT   ENTER OK   ESC CANCEL"
        );
        const int top =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        char text[96] = {};
        std::snprintf(text, sizeof(text), "%.70s", filename ? filename : "");
        draw_menu_message(top, text);
        draw_menu_option(top + 2, "Cancel", selected == 0);
        draw_menu_option(top + 3, "Overwrite", selected == 1);

        const int key = platform::get_char();
        if (key == kKeyUp || key == kKeyDown) {
            selected = 1 - selected;
        } else if (key_is_enter(key)) {
            return selected == 1;
        } else if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
            return false;
        }
    }
}

void Repl::menu_save_program(bool save_as) {
    auto wait_message = [&](const char* title, const char* text) {
        draw_menu_header(title, "ENTER / ESC BACK");
        draw_menu_message(
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4,
            text
        );
        while (true) {
            const int key = platform::get_char();
            if (key_is_enter(key) || key == kKeyEscape ||
                key == 0x1b || key == kKeyHome) return;
        }
    };

    if (!save_as && has_current_filename()) {
        if (!save_current_program()) {
            wait_message("SAVE PROGRAM", storage::last_error());
            return;
        }
        char text[96] = {};
        std::snprintf(text, sizeof(text), "SAVED %.70s", program_.filename());
        wait_message("SAVE PROGRAM", text);
        return;
    }

    char filename[kFilenameSize] = {};
    if (!prompt_text(
            "SAVE PROGRAM AS\r\nFilename: ",
            filename,
            sizeof(filename))) return;

    if (storage::program_exists(filename) &&
        !confirm_program_overwrite(filename)) return;

    if (!save_program_as(filename)) {
        wait_message("SAVE PROGRAM AS", storage::last_error());
        return;
    }

    char text[96] = {};
    std::snprintf(text, sizeof(text), "SAVED %.70s", program_.filename());
    wait_message("SAVE PROGRAM AS", text);
}

void Repl::service_background() {
    platform::audio_service();
    wireless::service_board_led();
    network::file_server_poll();
    bluetooth_manager::service();

    storage::poll();

    switch (storage::take_usb_event()) {
    case storage::UsbEvent::ReturnedToFirmware:
        usb_msc::set_media_present(false);
        if (program_.backend_type() == ProgramBackend::Sd && program_.suspended()) {
            if (program_.resume_after_usb()) {
                program_dirty_ = false;
                program_.set_dirty(false);
            }
        }
        break;
    case storage::UsbEvent::UnsafeDisconnect:
    case storage::UsbEvent::CardRemoved:
    case storage::UsbEvent::IoError:
        usb_msc::set_media_present(false);
        break;
    case storage::UsbEvent::None:
        break;
    }
}

void Repl::menu_usb_storage() {
    auto wait_message = [&](const char* text) {
        draw_menu_header("USB STORAGE", "ENTER / ESC BACK");
        const int row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 7;
        draw_menu_message(row, text);
        while (true) {
            const int key = platform::get_char();
            if (key_is_enter(key) || key == kKeyEscape || key == 0x1b) return;
        }
    };

    auto draw_options = [&](int first_row,
                            const usb_storage_menu::Model& model,
                            std::size_t selected) {
        // Always repaint every option slot. State changes must not leave an
        // old Enable/Back row behind on the cached Control Center screen.
        for (std::size_t i = 0; i < 3; ++i) {
            draw_menu_option(first_row + static_cast<int>(i),
                             i < model.count ? model.items[i] : "",
                             i < model.count && i == selected);
        }
    };

    auto confirm_force_disconnect = [&]() {
        draw_menu_header("FORCE USB STORAGE DISCONNECT?",
                         "UP/DOWN SELECT  ENTER  ESC CANCEL");
        const int top =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        draw_menu_message(top, "The host may have pending writes.");
        draw_menu_message(top + 1, "Filesystem data may be lost.");
        const auto model = usb_storage_menu::force_confirmation();
        std::size_t selected = model.default_selection; // Cancel
        while (true) {
            draw_options(top + 5, model, selected);
            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return false;
            if (key == kKeyDown || key == kKeyUp)
                selected = selected == 0 ? 1 : 0;
            if (key_is_enter(key)) return selected == 1;
        }
    };

    while (true) {
        service_background();
        const int top =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        const int option_row = top + 9;
        draw_menu_header("USB STORAGE", "UP/DOWN SELECT  ENTER  ESC BACK");

        char row[96] = {};
        std::snprintf(row, sizeof(row), "USB CDC       %s",
                      usb_device::usb_connected() ? "CONNECTED" : "NOT CONNECTED");
        draw_menu_message(top, row);

        const storage::Owner owner = storage::owner();
        const char* mass_storage = owner == storage::Owner::UsbHost ? "ACTIVE" :
                                   owner == storage::Owner::Transition ? "STOPPING" :
                                   owner == storage::Owner::Unavailable ? "ERROR" : "OFF";
        std::snprintf(row, sizeof(row), "Mass Storage  %s", mass_storage);
        draw_menu_message(top + 1, row);
        std::snprintf(row, sizeof(row), "SD Card       %s", storage::owner_name());
        draw_menu_message(top + 2, row);
        draw_menu_message(top + 5, "");
        draw_menu_message(top + 6, "");

        usb_storage_menu::State state = usb_storage_menu::State::Off;
        if (owner == storage::Owner::UsbHost) state = usb_storage_menu::State::Active;
        else if (owner == storage::Owner::Transition) state = usb_storage_menu::State::Transition;
        else if (owner == storage::Owner::Unavailable) state = usb_storage_menu::State::Unavailable;
        const auto model = usb_storage_menu::model(state);
        std::size_t selected = model.default_selection;

        if (state == usb_storage_menu::State::Active) {
            draw_menu_message(top + 5, usb_msc::host_ejected()
                ? "USB disk ejected. Return is safe."
                : "Eject the USB disk on the host before Return.");
            while (true) {
                draw_options(option_row, model, selected);
                const int key = platform::get_char();
                if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return;
                if (key == kKeyDown) selected = (selected + 1) % model.count;
                if (key == kKeyUp) selected = (selected + model.count - 1) % model.count;
                if (!key_is_enter(key)) continue;
                if (selected == 2) return;
                if (selected == 0) {
                    if (!usb_msc::host_ejected()) {
                        wait_message("PLEASE EJECT USB DISK ON HOST FIRST");
                        break;
                    }
                    usb_msc::set_media_present(false);
                    if (!storage::request_usb_safe_return(true)) {
                        wait_message("USB STORAGE RETURN FAILED");
                        break;
                    }
                    service_background();
                    wait_message(program_.suspended()
                        ? program_.error() : "SD CARD RETURNED TO CPB");
                    break;
                }
                if (!confirm_force_disconnect()) break;
                // Only the MSC medium is withdrawn. The composite USB device
                // and CDC console remain mounted and enumerated.
                usb_msc::set_media_present(false);
                if (!storage::request_usb_force_return()) {
                    wait_message("USB STORAGE DISCONNECT FAILED");
                    break;
                }
                service_background();
                wait_message(program_.suspended()
                    ? program_.error() : "SD CARD RETURNED TO CPB");
                break;
            }
            continue;
        }

        if (state == usb_storage_menu::State::Transition) {
            draw_menu_message(top + 5, "Finishing USB storage return...");
            draw_options(option_row, model, selected);
            const int key = platform::get_char();
            if (key_is_enter(key) || key == kKeyEscape || key == 0x1b || key == kKeyHome) return;
            continue;
        }

        if (state == usb_storage_menu::State::Unavailable) {
            draw_menu_message(top + 5, storage::last_error());
            draw_menu_message(top + 6, "Check/reinsert the SD card, then recover.");
            while (true) {
                draw_options(option_row, model, selected);
                const int key = platform::get_char();
                if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return;
                if (key == kKeyDown || key == kKeyUp) selected = selected == 0 ? 1 : 0;
                if (!key_is_enter(key)) continue;
                if (selected == 1) return;
                if (!storage::recover_firmware_ownership()) {
                    wait_message(storage::last_error());
                } else {
                    service_background();
                    wait_message(program_.suspended() ? program_.error() : "SD CARD RETURNED TO CPB");
                }
                break;
            }
            continue;
        }

        draw_menu_message(top + 5, "Expose the complete SD card to the computer.");
        draw_menu_message(top + 6, "CPB SD access is disabled until returned.");
        while (true) {
            draw_options(option_row, model, selected);
            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return;
            if (key == kKeyDown || key == kKeyUp) selected = selected == 0 ? 1 : 0;
            if (!key_is_enter(key)) continue;
            if (selected == 1) return;

            bool cancelled = false;
            if (program_.backend_type() == ProgramBackend::Sd &&
                (program_dirty_ || program_.is_dirty())) {
                draw_menu_header("SD PROGRAM MODIFIED", "S SAVE  D DISCARD  ESC CANCEL");
                draw_menu_message(top + 3, "Resolve changes before USB host ownership.");
                while (true) {
                    const int choice = platform::get_char();
                    if (choice == kKeyEscape || choice == 0x1b) {
                        cancelled = true;
                        break;
                    }
                    if (choice == 's' || choice == 'S') {
                        char name[80] = {};
                        const char* target = program_.filename();
                        if (!target || !*target) {
                            if (!prompt_text("SAVE AS (BAS filename)", name, sizeof(name))) {
                                cancelled = true;
                                break;
                            }
                            target = name;
                        }
                        if (!storage::save_program(target, program_)) {
                            wait_message(storage::last_error());
                            cancelled = true;
                        } else {
                            set_current_filename(program_.filename(), false);
                        }
                        break;
                    }
                    if (choice == 'd' || choice == 'D') {
                        const char* backing = program_.filename();
                        if (backing && *backing) {
                            char saved_name[80] = {};
                            std::snprintf(saved_name, sizeof(saved_name), "%s", backing);
                            if (!storage::load_program(saved_name, program_)) {
                                wait_message(storage::last_error());
                                cancelled = true;
                            } else {
                                set_current_filename(program_.filename(), false);
                            }
                        } else if (!program_.clear()) {
                            wait_message(program_.error());
                            cancelled = true;
                        } else {
                            set_current_filename("UNTITLED", false);
                        }
                        break;
                    }
                }
            }
            if (cancelled) break;

            network::file_server_stop();
            platform::audio_stop();
            bool suspended = false;
            if (program_.backend_type() == ProgramBackend::Sd) {
                if (!program_.suspend_for_usb()) {
                    wait_message(program_.error());
                    break;
                }
                suspended = true;
            }
            if (!storage::begin_usb_host_ownership()) {
                if (suspended) program_.cancel_usb_suspend();
                wait_message(storage::last_error());
                break;
            }
            if (!usb_msc::set_media_present(true)) {
                storage::request_usb_force_return();
                storage::poll();
                service_background();
                wait_message("USB MEDIA START FAILED");
                break;
            }
            break;
        }
    }
}


void Repl::menu_bluetooth_devices() {
    // Reused by list/actions; keep 32-entry snapshots off the embedded stack.
    static bluetooth_manager::PairedDeviceInfo current[32];
    if (!bluetooth_manager::enabled()) {
        draw_menu_header("PAIRED BLUETOOTH DEVICES", "ENTER / ESC BACK");
        const int row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 5;
        draw_menu_message(row, "Enable Bluetooth first.");
        while (true) {
            const int key = platform::get_char();
            if (key_is_enter(key) || key == kKeyEscape ||
                key == 0x1b || key == kKeyHome) return;
        }
    }

    auto format_address = [](const std::uint8_t address[6],
                             char* output, std::size_t capacity) {
        std::snprintf(
            output, capacity,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            static_cast<unsigned>(address[0]),
            static_cast<unsigned>(address[1]),
            static_cast<unsigned>(address[2]),
            static_cast<unsigned>(address[3]),
            static_cast<unsigned>(address[4]),
            static_cast<unsigned>(address[5])
        );
    };

    auto confirm_forget = [&](const bluetooth_manager::PairedDeviceInfo& device) {
        char address[24] = {};
        format_address(device.address, address, sizeof(address));
        int selected = 0; // Cancel by default.
        while (true) {
            draw_menu_header(
                "FORGET BLUETOOTH DEVICE?",
                "UP/DOWN SELECT  ENTER  ESC CANCEL"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            draw_menu_message(top, address);
            draw_menu_message(
                top + 1,
                "This device must be paired again."
            );
            draw_menu_option(top + 4, "Cancel", selected == 0);
            draw_menu_option(top + 5, "Forget this device", selected == 1);

            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
                return false;
            }
            if (key == kKeyUp || key == kKeyDown) {
                selected = 1 - selected;
                continue;
            }
            if (key_is_enter(key)) return selected == 1;
        }
    };


    auto forget = [&](const bluetooth_manager::PairedDeviceInfo& device) {
        return device.profile == bluetooth::Profile::BleKeyboard
            ? bluetooth_hid_ble::forget_paired_keyboard(device.address, device.address_type)
            : bluetooth_manager::forget_paired_device(device.address);
    };

    auto wait_for_connect = [&](const bluetooth_manager::PairedDeviceInfo& device) {
        const bool ble = device.profile == bluetooth::Profile::BleKeyboard;
        const bool started = ble
            ? bluetooth_hid_ble::connect_paired_keyboard(device.address, device.address_type, device.name)
            : bluetooth_hid::connect_paired_keyboard(device.address);
        if (!started) {
            draw_menu_header("BLUETOOTH CONNECT", "ENTER / ESC BACK");
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5;
            draw_menu_message(
                top,
                ble ? bluetooth_hid_ble::last_error()
                    : bluetooth_hid::last_error()
            );
            while (true) {
                const int key = platform::get_char();
                if (key_is_enter(key) || key == kKeyEscape ||
                    key == 0x1b || key == kKeyHome) return false;
            }
        }

        while (true) {
            service_background();

            const std::size_t count =
                bluetooth_manager::paired_devices(current, 32);
            bool connected_now = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (current[i].address_type == device.address_type && std::memcmp(
                        current[i].address,
                        device.address,
                        sizeof(device.address)) == 0) {
                    connected_now = current[i].connected;
                    break;
                }
            }

            draw_menu_header("BLUETOOTH CONNECT", "ESC CANCEL");
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            char name[80] = {};
            std::snprintf(
                name, sizeof(name),
                "Device: %s",
                device.name[0] ? device.name : "(name unknown)"
            );
            draw_menu_message(top, name);
            draw_menu_message(
                top + 2,
                connected_now ? "CONNECTED" :
                                "WAITING FOR SELECTED DEVICE..."
            );
            if (!connected_now) {
                draw_menu_message(
                    top + 3,
                    "Waiting for the keyboard HID link."
                );
            }

            if (connected_now) {
                platform::sleep_millis(500);
                return true;
            }

            if (ble && bluetooth_hid_ble::pairing_code_available()) {
                char code[64];
                std::snprintf(code, sizeof(code), "TYPE ON KEYBOARD: %06lu THEN ENTER",
                              static_cast<unsigned long>(bluetooth_hid_ble::pairing_code()));
                draw_menu_message(top + 4, code);
            }
            const bool active = ble ? bluetooth_hid_ble::connect_active()
                                    : bluetooth_hid::connect_active();
            if (!active) {
                draw_menu_message(top + 2, "CONNECT WAIT ENDED");
                draw_menu_message(
                    top + 3,
                    ble ? bluetooth_hid_ble::last_error()
                        : bluetooth_hid::last_error()
                );
                (void)platform::get_char();
                return false;
            }

            const int key = poll_menu_key(200);
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
                if (ble) bluetooth_hid_ble::cancel_connect();
                else bluetooth_hid::cancel_connect();
                return false;
            }
        }
    };

    constexpr std::size_t capacity = 32;
    constexpr int visible = 20;
    static bluetooth_manager::PairedDeviceInfo devices[capacity];
    menu_scroll::State scroll;

    while (true) {
        service_background();
        std::memset(devices, 0, sizeof(devices));
        const std::size_t count =
            bluetooth_manager::paired_devices(devices, capacity);

        draw_menu_header(
            "PAIRED BLUETOOTH DEVICES",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER ACTION"
        );
        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;

        if (count == 0) {
            draw_menu_message(first_row + 2, "(NO STORED PAIRINGS)");
            draw_menu_message(
                first_row + 4,
                "Use Bluetooth -> Pair Keyboard first."
            );
            draw_menu_message(
                first_row + 5,
                "ESC/HOME: back"
            );
            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return;
            continue;
        }

        menu_scroll::normalize(
            scroll, static_cast<int>(count), visible);

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int index = scroll.offset + row_index;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + row_index, "", false);
                continue;
            }

            char address[24] = {};
            format_address(
                devices[index].address, address, sizeof(address));
            const char* name = devices[index].name[0]
                ? devices[index].name : "(name unknown)";
            char row[96] = {};
            std::snprintf(
                row, sizeof(row),
                "%2d %-18.18s %-8s %-17s %s",
                index + 1,
                name,
                bluetooth::profile_name(devices[index].profile),
                address,
                devices[index].connected ? "ON" : "OFF"
            );
            draw_menu_option(
                first_row + row_index,
                row,
                index == scroll.selected
            );
        }

        char position[64] = {};
        std::snprintf(
            position, sizeof(position),
            "%d-%d / %u   Stored Pairings",
            scroll.offset + 1,
            menu_scroll::last_exclusive(
                scroll, static_cast<int>(count), visible),
            static_cast<unsigned>(count)
        );
        draw_menu_message(first_row + visible + 1, position);

        const int key = poll_menu_key(500);
        if (handle_menu_scroll_key(
                key, scroll, static_cast<int>(count), visible)) {
            continue;
        }
        if (key == kKeyEscape || key == 0x1b || key == kKeyHome) return;

        auto& device = devices[scroll.selected];

        if (key == kKeyDelete) {
            if (confirm_forget(device)) {
                forget(device);
                scroll.selected = 0;
                scroll.offset = 0;
            }
            continue;
        }

        if (!key_is_enter(key)) continue;

        char address[24] = {};
        format_address(device.address, address, sizeof(address));
        int action = 0;
        while (true) {
            service_background();
            const auto current_count = bluetooth_manager::paired_devices(current, capacity);
            device.connected = false;
            for (std::size_t i = 0; i < current_count; ++i) {
                if (current[i].address_type == device.address_type &&
                    std::memcmp(current[i].address, device.address, sizeof(device.address)) == 0) {
                    device.connected = current[i].connected;
                    break;
                }
            }
            draw_menu_header(
                "BLUETOOTH DEVICE",
                "UP/DOWN SELECT  ENTER ACTION  ESC BACK"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            char name_row[80] = {};
            std::snprintf(
                name_row, sizeof(name_row),
                "Name   : %s",
                device.name[0] ? device.name : "(name unknown)"
            );
            draw_menu_message(top, name_row);
            char address_row[80] = {};
            std::snprintf(
                address_row, sizeof(address_row),
                "Address: %s", address
            );
            draw_menu_message(top + 1, address_row);
            draw_menu_message(
                top + 2,
                device.connected ? "Connection: ON" :
                                   "Connection: OFF"
            );
            char profile_row[48] = {};
            std::snprintf(
                profile_row, sizeof(profile_row),
                "Profile: %s", bluetooth::profile_name(device.profile));
            draw_menu_message(top + 3, profile_row);
            if (!device.connected) {
                draw_menu_message(
                    top + 4,
                    "CPB will connect to this keyboard."
                );
                draw_menu_option(
                    top + 6,
                    "Connect Keyboard",
                    action == 0);
                draw_menu_option(
                    top + 7, "Forget this device", action == 1);
                draw_menu_option(top + 8, "Back", action == 2);
            } else {
                draw_menu_option(
                    top + 6, "Disconnect", action == 0);
                draw_menu_option(
                    top + 7, "Forget this device", action == 1);
                draw_menu_option(top + 8, "Back", action == 2);
            }

            const int action_count = 3;
            const int action_key = poll_menu_key(500);
            if (action_key == kKeyUp && action > 0) {
                --action;
                continue;
            }
            if (action_key == kKeyDown && action + 1 < action_count) {
                ++action;
                continue;
            }
            if (action_key == kKeyEscape || action_key == 0x1b ||
                action_key == kKeyHome) {
                break;
            }
            if (!key_is_enter(action_key)) continue;

            if (device.connected) {
                if (action == 0) {
                    if (device.profile == bluetooth::Profile::BleKeyboard) {
                        bluetooth_hid_ble::disconnect();
                    } else if (device.profile == bluetooth::Profile::HidKeyboard) {
                        bluetooth_hid::disconnect();
                    }
                    break;
                }
                if (action == 1) {
                    if (confirm_forget(device)) {
                        forget(device);
                        scroll.selected = 0;
                        scroll.offset = 0;
                    }
                    break;
                }
                break;
            }

            if (action == 0) {
                wait_for_connect(device);
                break;
            }
            if (action == 1) {
                if (confirm_forget(device)) {
                    forget(device);
                    scroll.selected = 0;
                    scroll.offset = 0;
                }
                break;
            }
            break;
        }
    }
}


void Repl::menu_bluetooth_keyboard() {
    if (!bluetooth_manager::enabled()) {
        draw_menu_header("PAIR KEYBOARD", "ENTER / ESC BACK");
        const int row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 5;
        draw_menu_message(row, "Bluetooth is OFF.");
        draw_menu_message(row + 1, "Enable Bluetooth first.");
        (void)platform::get_char();
        return;
    }

    auto format_address = [](const std::uint8_t address[6],
                             char* output, std::size_t capacity) {
        std::snprintf(
            output, capacity,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            static_cast<unsigned>(address[0]),
            static_cast<unsigned>(address[1]),
            static_cast<unsigned>(address[2]),
            static_cast<unsigned>(address[3]),
            static_cast<unsigned>(address[4]),
            static_cast<unsigned>(address[5])
        );
    };

    auto wait_for_keyboard = [&]() {
        while (true) {
            service_background();
            draw_menu_header(
                "PAIRING KEYBOARD",
                "TYPE CODE ON KEYBOARD / ESC CANCEL"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            char row[96] = {};
            std::snprintf(
                row, sizeof(row), "Status: %s", bluetooth_hid::status());
            draw_menu_message(top, row);
            const char* name = bluetooth_hid::connected_name();
            std::snprintf(
                row, sizeof(row), "Device: %s",
                name && *name ? name : "(discovering)"
            );
            draw_menu_message(top + 1, row);

            if (bluetooth_hid::pairing_code_available()) {
                std::snprintf(
                    row, sizeof(row),
                    "Enter this code on keyboard: %06lu",
                    static_cast<unsigned long>(
                        bluetooth_hid::pairing_code())
                );
                draw_menu_message(top + 3, row);
                draw_menu_message(top + 4, "Then press ENTER on the keyboard.");
            } else {
                draw_menu_message(top + 3, bluetooth_hid::last_error());
            }

            if (bluetooth_hid::connected()) {
                draw_menu_message(top + 6, "KEYBOARD CONNECTED");
                platform::sleep_millis(700);
                return true;
            }
            if (!bluetooth_hid::connect_active()) {
                draw_menu_message(top + 6, "CONNECTION ENDED");
                draw_menu_message(top + 7, bluetooth_hid::last_error());
                (void)platform::get_char();
                return false;
            }

            const int key = poll_menu_key(200);
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
                bluetooth_hid::cancel_connect();
                return false;
            }
        }
    };

    // 0: back/cancel, 1: connected, 2: rescan after a completed failure.
    auto wait_for_ble_keyboard = [&]() {
        while (true) {
            service_background();
            draw_menu_header(
                "PAIRING BLE KEYBOARD",
                bluetooth_hid_ble::pairing_code_available()
                    ? "TYPE CODE ON KEYBOARD / ESC CANCEL" : "ESC CANCEL"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
            char row[96] = {};
            std::snprintf(
                row, sizeof(row), "Status: %s",
                bluetooth_hid_ble::status());
            draw_menu_message(top, row);
            const char* name = bluetooth_hid_ble::connected_name();
            std::snprintf(
                row, sizeof(row), "Device: %s",
                name && *name ? name : "(selected BLE device)"
            );
            draw_menu_message(top + 1, row);

            if (bluetooth_hid_ble::pairing_code_available()) {
                std::snprintf(
                    row, sizeof(row),
                    "Enter this code on keyboard: %06lu",
                    static_cast<unsigned long>(
                        bluetooth_hid_ble::pairing_code())
                );
                draw_menu_message(top + 3, row);
                draw_menu_message(top + 4, "Then press ENTER on the keyboard.");
            } else {
                draw_menu_message(
                    top + 3, bluetooth_hid_ble::last_error());
            }

            if (bluetooth_hid_ble::connected()) {
                draw_menu_message(top + 6, "BLE KEYBOARD CONNECTED");
                platform::sleep_millis(700);
                return 1;
            }
            if (!bluetooth_hid_ble::connect_active()) {
                draw_menu_message(top + 6, "CONNECTION ENDED");
                draw_menu_message(
                    top + 7, bluetooth_hid_ble::last_error());
                draw_menu_message(top + 9, "Put keyboard back in pairing mode.");
                draw_menu_message(top + 10, "No advertisement? Restart keyboard.");
                draw_menu_message(top + 12, "ENTER RESCAN / ESC BACK");
                while (true) {
                    service_background();
                    const int key = poll_menu_key(150);
                    if (key_is_enter(key)) return 2;
                    if (key == kKeyEscape || key == 0x1b ||
                        key == kKeyHome) return 0;
                }
            }

            const int key = poll_menu_key(200);
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
                bluetooth_hid_ble::cancel_connect();
                return 0;
            }
        }
    };

    auto scan_and_pair = [&]() {
        const bool ble_scan = bluetooth_hid_ble::start_scan();
        const bool classic_scan = bluetooth_hid::start_scan();
        if (!ble_scan && !classic_scan) {
            draw_menu_header("PAIR KEYBOARD", "ENTER / ESC BACK");
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 5;
            draw_menu_message(top, "COULD NOT START KEYBOARD SCAN");
            draw_menu_message(
                top + 2, bluetooth_hid_ble::last_error());
            (void)platform::get_char();
            return;
        }

        auto same_identity = [](
            const bluetooth_hid_ble::DiscoveredDevice& lhs,
            const bluetooth_hid_ble::DiscoveredDevice& rhs
        ) {
            return lhs.address_type == rhs.address_type &&
                   std::memcmp(lhs.address, rhs.address, 6) == 0;
        };
        auto same_display_data = [&](
            const bluetooth_hid_ble::DiscoveredDevice& lhs,
            const bluetooth_hid_ble::DiscoveredDevice& rhs
        ) {
            // RSSI changes on nearly every advertisement. Do not repaint a
            // row for RSSI alone; repaint when its name or HID classification
            // becomes known.
            return same_identity(lhs, rhs) &&
                   lhs.hid_hint == rhs.hid_hint &&
                   std::strcmp(lhs.name, rhs.name) == 0;
        };
        auto draw_device_row = [&](
            int row,
            const bluetooth_hid_ble::DiscoveredDevice* device,
            bool selected
        ) {
            if (!device) {
                draw_menu_option(row, "", false);
                return;
            }
            char address[24] = {};
            format_address(device->address, address, sizeof(address));
            char text[96] = {};
            std::snprintf(
                text, sizeof(text),
                "%-20.20s %-6s %4d %s %s",
                device->name[0]
                    ? device->name : "(name unknown)",
                device->address_type == 0xff ? "CLASSIC" : "BLE",
                static_cast<int>(device->rssi),
                device->address_type == 0xff ? "BT" : device->address_type == 0 ? "PUB" : "RND",
                address
            );
            draw_menu_option(row, text, selected);
        };

        constexpr std::size_t combined_capacity =
            bluetooth_hid_ble::kMaxDiscoveredDevices + bluetooth_hid::kMaxDiscoveredDevices;
        static bluetooth_hid::DiscoveredDevice classic_found[bluetooth_hid::kMaxDiscoveredDevices];
        auto collect = [&](bluetooth_hid_ble::DiscoveredDevice* output) {
            std::size_t count = bluetooth_hid_ble::discovered_devices(output, bluetooth_hid_ble::kMaxDiscoveredDevices);
            const auto classic_count = bluetooth_hid::discovered_devices(classic_found, bluetooth_hid::kMaxDiscoveredDevices);
            for (std::size_t i = 0; i < classic_count && count < combined_capacity; ++i) {
                auto& entry = output[count++];
                entry = bluetooth_hid_ble::DiscoveredDevice{};
                std::memcpy(entry.address, classic_found[i].address, sizeof(entry.address));
                entry.address_type = 0xff;
                std::snprintf(entry.name, sizeof(entry.name), "%s", classic_found[i].name);
                entry.hid_hint = classic_found[i].keyboard_hint;
                entry.rssi = classic_found[i].rssi;
            }
            return count;
        };
        auto stop_scans = [&]() {
            bluetooth_hid_ble::cancel_scan();
            bluetooth_hid::cancel_scan();
        };
        menu_scroll::State scroll;
        constexpr int visible = 18;
        static bluetooth_hid_ble::DiscoveredDevice
            shown[combined_capacity];
        static bluetooth_hid_ble::DiscoveredDevice
            found[combined_capacity];
        std::memset(shown, 0, sizeof(shown));
        std::memset(found, 0, sizeof(found));
        std::size_t shown_count = 0;
        int shown_selected = -1;
        int shown_offset = -1;
        bool shown_full = false;
        bool first_draw = true;

        draw_menu_header(
            "KEYBOARDS / LIVE SCAN",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER CONNECT"
        );
        const int first =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;

        while (bluetooth_hid_ble::scanning() || bluetooth_hid::scanning()) {
            service_background();

            // Keep selection on the same address/type when the visible list changes.
            bluetooth_hid_ble::DiscoveredDevice selected_before = {};
            const bool had_selection =
                shown_count > 0 &&
                scroll.selected >= 0 &&
                scroll.selected < static_cast<int>(shown_count);
            if (had_selection) {
                selected_before = shown[scroll.selected];
            }

            std::memset(found, 0, sizeof(found));
            const std::size_t count = collect(found);
            const bool capacity_reached =
                bluetooth_hid_ble::discovery_capacity_reached();

            if (had_selection) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (same_identity(selected_before, found[i])) {
                        scroll.selected = static_cast<int>(i);
                        break;
                    }
                }
            }
            menu_scroll::normalize(
                scroll, static_cast<int>(count), visible);

            bool full_redraw =
                first_draw ||
                shown_count != count ||
                shown_offset != scroll.offset;
            if (!full_redraw) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (!same_identity(shown[i], found[i])) {
                        full_redraw = true;
                        break;
                    }
                }
            }

            for (int row_index = 0; row_index < visible; ++row_index) {
                const int index = scroll.offset + row_index;
                const bool selected =
                    index < static_cast<int>(count) &&
                    index == scroll.selected;
                bool redraw = full_redraw ||
                    (shown_selected != scroll.selected &&
                     (index == shown_selected || index == scroll.selected));
                if (!redraw &&
                    index < static_cast<int>(count) &&
                    index < static_cast<int>(shown_count)) {
                    redraw = !same_display_data(
                        shown[index], found[index]);
                }
                if (!redraw) continue;

                if (index < static_cast<int>(count)) {
                    draw_device_row(
                        first + row_index, &found[index], selected);
                } else if (count == 0 && row_index == 0) {
                    draw_menu_option(
                        first + row_index,
                        "(scanning... no keyboards yet)",
                        false
                    );
                } else {
                    draw_device_row(
                        first + row_index, nullptr, false);
                }
            }

            if (first_draw ||
                shown_count != count ||
                shown_offset != scroll.offset ||
                shown_full != capacity_reached) {
                char status[80] = {};
                if (count > static_cast<std::size_t>(visible)) {
                    std::snprintf(
                        status, sizeof(status),
                        "Scanning: %u%s keyboards  %d-%d / %u",
                        static_cast<unsigned>(count),
                        capacity_reached ? "+" : "",
                        scroll.offset + 1,
                        menu_scroll::last_exclusive(
                            scroll,
                            static_cast<int>(count),
                            visible
                        ),
                        static_cast<unsigned>(count)
                    );
                } else {
                    std::snprintf(
                        status, sizeof(status),
                        "Scanning: %u%s keyboard(s)",
                        static_cast<unsigned>(count),
                        capacity_reached ? "+" : ""
                    );
                }
                draw_menu_message(first + visible, status);
                draw_menu_message(
                    first + visible + 1,
                    capacity_reached
                        ? "BLE list full; Classic scan continues."
                        : "Put keyboard in pairing mode before ENTER."
                );
            }

            std::memcpy(shown, found, sizeof(shown));
            shown_count = count;
            shown_selected =
                count > 0 ? scroll.selected : -1;
            shown_offset = scroll.offset;
            shown_full = capacity_reached;
            first_draw = false;

            const int key = poll_menu_key(150);
            if (handle_menu_scroll_key(
                    key, scroll, static_cast<int>(count), visible)) {
                continue;
            }
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
                stop_scans();
                return;
            }
            if (!key_is_enter(key) || count == 0) continue;

            const auto selected_device = found[scroll.selected];
            stop_scans();
            if (selected_device.address_type == 0xff) {
                if (bluetooth_hid::connect_keyboard(selected_device.address)) wait_for_keyboard();
                return;
            }
            if (bluetooth_hid_ble::connect_keyboard(
                    selected_device.address,
                    selected_device.address_type,
                    selected_device.name)) {
                const int outcome = wait_for_ble_keyboard();
                if (outcome != 2) return;
                // Failure UI is reached only after the pending ACL teardown.
                const bool restarted_ble = bluetooth_hid_ble::start_scan();
                const bool restarted_classic = bluetooth_hid::start_scan();
                if (!restarted_ble && !restarted_classic) {
                    draw_menu_message(first + visible,
                                      bluetooth_hid_ble::last_error());
                    (void)platform::get_char();
                    return;
                }
                shown_count = 0;
                shown_selected = -1;
                shown_offset = -1;
                first_draw = true;
                draw_menu_header(
                    "KEYBOARDS / LIVE SCAN",
                    "UP/DOWN SELECT  ENTER CONNECT  ESC STOP");
                continue;
            }
            draw_menu_message(first + visible,
                              bluetooth_hid_ble::last_error());
            (void)platform::get_char();
            return;
        }
    };

    scan_and_pair();
}

void Repl::menu_bluetooth() {
    auto confirm_forget_pairings = [&]() {
        int confirm_selected = 0;
        while (true) {
            service_background();
            draw_menu_header(
                "FORGET ALL BLUETOOTH PAIRINGS?",
                "UP/DOWN SELECT  ENTER  ESC CANCEL"
            );
            const int top =
                (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
            draw_menu_message(top, "Deletes all saved Bluetooth link keys.");
            draw_menu_message(top + 1, "Each device must be paired again.");
            draw_menu_option(top + 4, "Cancel", confirm_selected == 0);
            draw_menu_option(
                top + 5, "Forget all pairings", confirm_selected == 1);

            const int key = platform::get_char();
            if (key == kKeyEscape || key == 0x1b || key == kKeyHome)
                return false;
            if (key == kKeyUp || key == kKeyDown) {
                confirm_selected = confirm_selected == 0 ? 1 : 0;
                continue;
            }
            if (key_is_enter(key)) return confirm_selected == 1;
        }
    };

    int selected = 0;
    while (true) {
        service_background();

        draw_menu_header(
            "BLUETOOTH",
            "UP/DOWN SELECT  ENTER ACTION  ESC BACK"
        );
        const int first =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        char row[96] = {};
        std::snprintf(
            row, sizeof(row), "Enabled     %s",
            bluetooth_manager::enabled() ? "ON" : "OFF");
        draw_menu_message(first, row);
        std::snprintf(
            row, sizeof(row), "Keyboard C  %s",
            bluetooth_hid::status());
        draw_menu_message(first + 1, row);
        std::snprintf(
            row, sizeof(row), "Keyboard LE %s",
            bluetooth_hid_ble::status());
        draw_menu_message(first + 2, row);

        draw_menu_option(
            first + 5,
            bluetooth_manager::enabled()
                ? "Disable Bluetooth" : "Enable Bluetooth",
            selected == 0
        );
        draw_menu_option(
            first + 6, "Pair Keyboard...", selected == 1);
        draw_menu_option(
            first + 7, "Paired Devices...", selected == 2);
        std::snprintf(
            row, sizeof(row), "Keyboard Layout: %s",
            bluetooth_hid::keyboard_layout_name(
                settings_.bluetooth_keyboard_layout)
        );
        draw_menu_option(first + 8, row, selected == 3);
        draw_menu_option(
            first + 9, "Forget All Paired Devices", selected == 4);
        draw_menu_option(first + 10, "Back", selected == 5);

        const int key = platform::get_char();
        if (key < 0) continue;
        if (key == kKeyUp && selected > 0) --selected;
        else if (key == kKeyDown && selected < 5) ++selected;
        else if (key == kKeyEscape || key == 0x1b ||
                 (key_is_enter(key) && selected == 5)) {
            return;
        } else if (key_is_enter(key) && selected == 0) {
            if (bluetooth_manager::enabled()) {
                bluetooth_manager::disable();
            } else {
                bluetooth_manager::enable();
            }
        } else if (key_is_enter(key) && selected == 1) {
            menu_bluetooth_keyboard();
        } else if (key_is_enter(key) && selected == 2) {
            menu_bluetooth_devices();
        } else if (key_is_enter(key) && selected == 3) {
            settings_.bluetooth_keyboard_layout =
                settings_.bluetooth_keyboard_layout ==
                    bluetooth_hid::KeyboardLayout::Jis
                ? bluetooth_hid::KeyboardLayout::Us
                : bluetooth_hid::KeyboardLayout::Jis;
            bluetooth_hid::set_layout(
                settings_.bluetooth_keyboard_layout);
            bluetooth_hid_ble::set_layout(
                settings_.bluetooth_keyboard_layout);
        } else if (key_is_enter(key) && selected == 4) {
            if (confirm_forget_pairings()) {
                bluetooth_manager::forget_paired_devices();
            }
        }
    }
}
void Repl::show_system_menu() {
    platform::set_console_mode(platform::ConsoleMode::Both);

    static const char* items[] = {
        "Files",
        "Editor",
        "Save Program",
        "Save Program As...",
        "Quick Load Keys",
        "Display",
        "Console",
        "Date / Time",
        "Audio",
        "Wireless LAN",
        "Bluetooth",
        "Wi-Fi File Server",
        "File Transfer",
        "USB Storage",
        "SD Card",
        "Firmware",
        "Power / CPU",
        "Board LED",
        "System Information",
        "PSRAM Diagnostics",
        "Program Storage",
        "Exit"
    };

    constexpr int item_count =
        static_cast<int>(sizeof(items) / sizeof(items[0]));
    constexpr int visible = 27;
    menu_scroll::State scroll;
    menu_scroll::normalize(scroll, item_count, visible);

    while (true) {
        draw_menu_header(
            "CALA'S POKECOM BASIC CONTROL CENTER",
            "UP/DN SELECT  SHIFT+UP/DN PAGE  ENTER OPEN"
        );

        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        menu_scroll::normalize(scroll, item_count, visible);

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int index = scroll.offset + row_index;
            if (index >= item_count) {
                draw_menu_option(first_row + row_index, "", false);
                continue;
            }
            char row[80] = {};
            std::snprintf(row, sizeof(row), "  %s", items[index]);
            draw_menu_option(
                first_row + row_index,
                row,
                index == scroll.selected
            );
        }

        if (item_count > visible) {
            char position[48] = {};
            std::snprintf(
                position, sizeof(position),
                "%d-%d / %d",
                scroll.offset + 1,
                menu_scroll::last_exclusive(
                    scroll, item_count, visible),
                item_count
            );
            draw_menu_message(first_row + visible, position);
        } else {
            draw_menu_message(first_row + visible, "");
        }

        const int key = platform::get_char();

        if (handle_menu_scroll_key(
                key, scroll, item_count, visible)) {
            continue;
        }
        if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
            break;
        }
        if (!key_is_enter(key)) {
            continue;
        }

        const int selected = scroll.selected;
        if (selected == 0) {
            if (menu_files()) {
                platform::set_console_mode(settings_.console_mode);
                return;
            }
        } else if (selected == 1) {
            open_full_screen_editor();
        } else if (selected == 2) {
            menu_save_program(false);
        } else if (selected == 3) {
            menu_save_program(true);
        } else if (selected == 4) {
            menu_quick_keys();
        } else if (selected == 5) {
            menu_display();
        } else if (selected == 6) {
            menu_console();
        } else if (selected == 7) {
            menu_datetime();
        } else if (selected == 8) {
            menu_audio();
        } else if (selected == 9) {
            menu_wifi();
        } else if (selected == 10) {
            menu_bluetooth();
        } else if (selected == 11) {
            menu_file_server();
        } else if (selected == 12) {
            menu_file_transfer();
        } else if (selected == 13) {
            menu_usb_storage();
        } else if (selected == 14) {
            menu_sd();
        } else if (selected == 15) {
            menu_firmware();
        } else if (selected == 16) {
            menu_power();
        } else if (selected == 17) {
            menu_board_led();
        } else if (selected == 18) {
            menu_system_info();
        } else if (selected == 19) {
            menu_psram_diagnostics();
        } else if (selected == 20) {
            menu_program_storage();
        } else {
            break;
        }
    }

    save_settings();
    leave_menu_screen();
    platform::set_console_mode(settings_.console_mode);
}

void Repl::open_full_screen_editor() {
    void* memory = std::malloc(sizeof(FullScreenEditor));
    if (!memory) {
        platform::put_string("?OUT OF MEMORY\r\n");
        return;
    }
    auto* editor = new(memory) FullScreenEditor(
        program_, current_filename_, sizeof(current_filename_), program_dirty_);
    full_screen_editor_active_ = true;
    const bool ok = editor->run();
    full_screen_editor_active_ = false;
    editor->~FullScreenEditor();
    std::free(memory);
    if (!ok) {
        platform::put_char('?');
        platform::put_string(program_.error());
        platform::put_string("\r\n");
    }
    leave_menu_screen();
}

void Repl::menu_file_transfer() {
    static const char* items[] = {
        "Receive via YMODEM",
        "Send via YMODEM",
        "Receive via XMODEM",
        "Send via XMODEM",
        "Back"
    };
    auto route_label = [](platform::SerialTransferRoute route) {
        switch (route) {
        case platform::SerialTransferRoute::Auto: return "AUTO";
        case platform::SerialTransferRoute::Usb: return "USB CDC";
        case platform::SerialTransferRoute::Uart: return "UART0";
        }
        return "AUTO";
    };
    auto cycle_route = [](platform::SerialTransferRoute route, int direction) {
        int value = static_cast<int>(route);
        value = (value + direction + 3) % 3;
        return static_cast<platform::SerialTransferRoute>(value);
    };

    // This selection intentionally lives only for this menu invocation.
    platform::SerialTransferRoute transfer_route =
        platform::SerialTransferRoute::Auto;
    int selected = 0;
    while (true) {
        draw_menu_header(
            "FILE TRANSFER",
            "UP/DOWN SELECT  LEFT/RIGHT CHANGE  ENTER OPEN"
        );
        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 4;
        char transport[48];
        std::snprintf(
            transport,
            sizeof(transport),
            "Transport: %s",
            route_label(transfer_route)
        );
        draw_menu_option(first_row, transport, selected == 0);
        for (int i = 0; i < 5; ++i) {
            draw_menu_option(first_row + i + 1, items[i], selected == i + 1);
        }
        draw_menu_message(first_row + 8, "YMODEM: exact size / recommended");
        draw_menu_message(first_row + 9, "XMODEM: compatibility / emergency");

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) { --selected; continue; }
        if (key == kKeyDown && selected < 5) { ++selected; continue; }
        if (selected == 0 && (key == kKeyLeft || key == kKeyRight)) {
            transfer_route = cycle_route(
                transfer_route,
                key == kKeyLeft ? -1 : 1
            );
            continue;
        }
        if (key == kKeyEscape || key == 0x1b || key == kKeyHome ||
            (key_is_enter(key) && selected == 5)) return;
        if (!key_is_enter(key)) continue;
        if (selected == 0) {
            transfer_route = cycle_route(transfer_route, 1);
            continue;
        }

        char filename[kFilenameSize] = {};
        if (selected == 2 || selected == 4) {
            if (!pick_transfer_file(filename, sizeof(filename),
                    selected == 2 ? "YMODEM SEND" : "XMODEM SEND")) continue;
        } else if (selected == 3) {
            if (!prompt_text(
                    "XMODEM RECEIVE\r\nFilename: ",
                    filename,
                    sizeof(filename))) continue;
        }

        leave_menu_screen();
        if (selected < 3) {
            run_ymodem_transfer(
                filename,
                selected == 1,
                true,
                transfer_route
            );
        } else {
            run_xmodem_transfer(
                filename,
                selected == 3,
                transfer_route
            );
        }
        platform::put_string("\r\nENTER: CONTROL CENTER\r\n");
        while (true) {
            const int done = platform::get_char();
            if (key_is_enter(done) || done == kKeyEscape ||
                done == 0x1b || done == kKeyHome) break;
        }
        draw_menu_header(nullptr, nullptr);
    }
}

void Repl::command_xmodem(char* argument, bool receive) {
    char filename[kFilenameSize] = {};
    if (!parse_filename_argument(argument, filename, sizeof(filename))) {
        platform::put_string("?BAD FILENAME\r\n");
        return;
    }
    run_xmodem_transfer(filename, receive);
}

void Repl::run_xmodem_transfer(
    const char* filename,
    bool receive,
    platform::SerialTransferRoute route
) {
    if (!storage::init()) {
        print_storage_error();
        return;
    }
    if (!storage::try_lock()) { print_storage_error(); return; }
    struct StorageLease { ~StorageLease() { storage::unlock(); } } storage_lease;
    TransferFile file;
    if (!file.open(filename, receive)) {
        platform::put_string(file.error());
        platform::put_string("\r\n");
        return;
    }
    platform::put_string(receive ? "XMODEM RECEIVE: " : "XMODEM SEND: ");
    platform::put_string(filename);
    platform::put_string(receive ? "\r\nWAITING FOR SENDER...\r\n" :
                                   "\r\nWAITING FOR RECEIVER...\r\n");
    platform::put_string(receive ?
        "TERA TERM: FILE > TRANSFER > XMODEM > SEND\r\n" :
        "TERA TERM: FILE > TRANSFER > XMODEM > RECEIVE\r\n");
    platform::put_string("ESC: CANCEL\r\n");
    platform::put_string("TRANSPORT: ");
    platform::put_string(platform::serial_transfer_route_name(route));
    platform::put_string("\r\n");
    if (!platform::begin_serial_transfer(route)) {
        platform::put_string(platform::serial_transfer_error());
        platform::put_string("\r\n");
        return;
    }
    SerialTransferLease transfer_lease;
    xmodem::IO io {
        &file,
        [](void*, unsigned ms) { return platform::serial_transfer_read(ms); },
        [](void*, const std::uint8_t* p, std::size_t n) { return platform::serial_transfer_write(p, n); },
        [](void* p, std::uint8_t* b, std::size_t n) { return static_cast<TransferFile*>(p)->read(b, n); },
        [](void* p, const std::uint8_t* b, std::size_t n) { return static_cast<TransferFile*>(p)->write(b, n); },
        [](void* p) { return static_cast<TransferFile*>(p)->commit(); }
    };
    auto result = receive ? xmodem::receive(io) : xmodem::send(io);
    file.abort();
    transfer_lease.close();
    // No textual output or status callbacks occur on the XMODEM transport.
    platform::put_string("\r\n");
    platform::put_string(result.error == xmodem::Error::Write ? file.error() :
                         result.error == xmodem::Error::Disconnected ? platform::serial_transfer_error() :
                         xmodem::error_text(result.error));
    platform::put_string("\r\n");
    if (result.error == xmodem::Error::None) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s %lu BYTES\r\n",
            receive ? "RECEIVED" : "SENT",
            static_cast<unsigned long>(receive ? file.bytes() : result.bytes));
        platform::put_string(text);
        if (receive && std::strcmp(file.error(), "TRANSFER COMPLETE") != 0) {
            platform::put_string(file.error());
            platform::put_string("\r\n");
        }
    }
}

void Repl::command_ymodem(char* argument,bool receive) {
    char filename[kFilenameSize]={};
    if(!receive&&!parse_filename_argument(argument,filename,sizeof(filename))){
        platform::put_string("?BAD FILENAME\r\n");return;
    }
    run_ymodem_transfer(filename,receive);
}

void Repl::run_ymodem_transfer(
    const char* filename,
    bool receive,
    bool menu_ui,
    platform::SerialTransferRoute route
) {
    if(!storage::init()){
        print_storage_error();return;
    }
    if(!storage::try_lock()){print_storage_error();return;}
    struct StorageLease{~StorageLease(){storage::unlock();}} storage_lease;
    struct Context {
        TransferFile file;
        std::uint32_t expected=0;
        bool menu_ui=false;
        int info_row=0;
    } context;
    context.menu_ui=menu_ui;
    context.info_row=(settings_.status_enabled ? console_layout::status_rows : 0)+10;
    if(!receive&&!context.file.open(filename,false,"/","YMODEM")){
        platform::put_string(context.file.error());platform::put_string("\r\n");return;
    }
    platform::put_string(receive?"YMODEM RECEIVE\r\nWAITING FOR SENDER...\r\n":"YMODEM SEND: ");
    if(!receive){
        platform::put_string(filename);char size[64];
        std::snprintf(size,sizeof(size),"\r\nSIZE: %lu BYTES\r\nWAITING FOR RECEIVER...\r\n",
            static_cast<unsigned long>(context.file.size()));platform::put_string(size);
    }
    platform::put_string(receive?
        "TERA TERM: FILE > TRANSFER > YMODEM > SEND\r\n":
        "TERA TERM: FILE > TRANSFER > YMODEM > RECEIVE\r\n");
    platform::put_string("ESC: CANCEL\r\n");
    platform::put_string("TRANSPORT: ");platform::put_string(platform::serial_transfer_route_name(route));platform::put_string("\r\n");
    if(!platform::begin_serial_transfer(route)){
        platform::put_string(platform::serial_transfer_error());platform::put_string("\r\n");return;
    }
    SerialTransferLease transfer_lease;
    ymodem::IO io{
        &context,
        [](void*,unsigned ms){return platform::serial_transfer_read(ms);},
        [](void*,const std::uint8_t* p,std::size_t n){return platform::serial_transfer_write(p,n);},
        [](void* p,std::uint8_t* b,std::size_t n){return static_cast<Context*>(p)->file.read(b,n);},
        [](void* p,const std::uint8_t* b,std::size_t n){
            return static_cast<Context*>(p)->file.write_exact(b,n);
        },
        [](void* p){
            auto* c=static_cast<Context*>(p);
            return c->file.commit_exact(c->expected);
        },
        [](void* p,const char* name,std::uint32_t size){
            auto* c=static_cast<Context*>(p);
            c->expected=size;
            if(c->menu_ui){
                char line[96];
                std::snprintf(line,sizeof(line),"File: %.46s",name);
                platform::draw_text_row(c->info_row,line,0xffff80,0x000000);
                std::snprintf(line,sizeof(line),"Size: %lu bytes",static_cast<unsigned long>(size));
                platform::draw_text_row(c->info_row+1,line,0xffff80,0x000000);
            }
            return c->file.open(name,true,"/","YMODEM");
        }
    };
    auto result=receive?ymodem::receive(io):ymodem::send(io,filename,context.file.size());
    context.file.abort();transfer_lease.close();
    platform::put_string("\r\n");
    if(receive&&result.filename[0]){
        platform::put_string(result.files>1?"LAST FILE: ":"FILE: ");platform::put_string(result.filename);
        char size[48];std::snprintf(size,sizeof(size),"\r\nSIZE: %lu BYTES\r\n",static_cast<unsigned long>(context.expected));
        platform::put_string(size);
    }
    if(result.error==ymodem::Error::None){
        char text[96];
        if(receive&&result.files>1)
            std::snprintf(text,sizeof(text),
                "RECEIVED %lu FILES\r\nTOTAL %lu BYTES\r\n",
                static_cast<unsigned long>(result.files),
                static_cast<unsigned long>(result.bytes));
        else
            std::snprintf(text,sizeof(text),"%s %lu BYTES\r\n",
                receive?"RECEIVED":"SENT",
                static_cast<unsigned long>(result.bytes));
        platform::put_string(text);
        platform::put_string("TRANSFER COMPLETE\r\n");
        if(receive&&std::strcmp(context.file.error(),"TRANSFER COMPLETE")!=0){
            platform::put_string(context.file.error());platform::put_string("\r\n");
        }
    }else{
        platform::put_string(result.error==ymodem::Error::Write?context.file.error():
            result.error==ymodem::Error::Disconnected?platform::serial_transfer_error():ymodem::error_text(result.error));
        platform::put_string("\r\n");
    }
}

void Repl::command_sd(char* argument) {
    argument = skip_spaces(argument);

    if (*argument == '\0' || command_equals(argument, "STATUS")) {
        char line[128] = {};
        std::snprintf(
            line,
            sizeof(line),
            "SD: CARD=%s MOUNTED=%s STATUS=%s\r\n",
            storage::card_present() ? "YES" : "NO",
            storage::available() ? "YES" : "NO",
            storage::last_error()
        );
        platform::put_string(line);
        return;
    }

    if (command_equals(argument, "REMOUNT")) {
        if (storage::remount()) {
            platform::put_string("SD: READY\r\n");
        } else {
            print_storage_error();
        }
        return;
    }

    platform::put_string("?SD STATUS/REMOUNT\r\n");
}

void Repl::command_date(char* argument) {
    argument = skip_spaces(argument);
    platform::DateTime dt;

    if (*argument == '\0') {
        if (!platform::get_datetime(dt)) {
            platform::put_string("?RTC NOT AVAILABLE\r\n");
            return;
        }

        char line[32] = {};
        std::snprintf(
            line,
            sizeof(line),
            "%04d-%02d-%02d\r\n",
            dt.year,
            dt.month,
            dt.day
        );
        platform::put_string(line);
        return;
    }

    if (!platform::get_datetime(dt)) {
        platform::put_string(
            "?RTC NOT INITIALIZED - USE DATETIME YYYYMMDDHHMMSS\r\n"
        );
        return;
    }

    if (!parse_date_value(argument, dt)) {
        platform::put_string("?BAD DATE\r\n");
        return;
    }

    const bool hardware_ok = platform::set_datetime(dt);
    if (hardware_ok) {
        platform::put_string("OK\r\n");
    } else {
        platform::put_string("OK - SOFTWARE CLOCK; ");
        platform::put_string(platform::datetime_last_error());
        platform::put_string("\r\n");
    }
}

void Repl::command_time(char* argument) {
    argument = skip_spaces(argument);
    platform::DateTime dt;

    if (*argument == '\0') {
        if (!platform::get_datetime(dt)) {
            platform::put_string("?RTC NOT AVAILABLE\r\n");
            return;
        }

        char line[32] = {};
        std::snprintf(
            line,
            sizeof(line),
            "%02d:%02d:%02d\r\n",
            dt.hour,
            dt.minute,
            dt.second
        );
        platform::put_string(line);
        return;
    }

    if (!platform::get_datetime(dt)) {
        platform::put_string(
            "?RTC NOT INITIALIZED - USE DATETIME YYYYMMDDHHMMSS\r\n"
        );
        return;
    }

    if (!parse_time_value(argument, dt)) {
        platform::put_string("?BAD TIME\r\n");
        return;
    }

    const bool hardware_ok = platform::set_datetime(dt);
    if (hardware_ok) {
        platform::put_string("OK\r\n");
    } else {
        platform::put_string("OK - SOFTWARE CLOCK; ");
        platform::put_string(platform::datetime_last_error());
        platform::put_string("\r\n");
    }
}

void Repl::command_datetime(char* argument) {
    argument = skip_spaces(argument);
    platform::DateTime dt;

    if (*argument == '\0') {
        if (!platform::get_datetime(dt)) {
            platform::put_string("?RTC NOT AVAILABLE\r\n");
            return;
        }

        char line[48] = {};
        std::snprintf(
            line,
            sizeof(line),
            "%04d-%02d-%02d %02d:%02d:%02d\r\n",
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute,
            dt.second
        );
        platform::put_string(line);
        return;
    }

    if (!parse_datetime_value(argument, dt)) {
        platform::put_string("?BAD DATETIME - USE YYYYMMDDHHMMSS\r\n");
        return;
    }

    const bool hardware_ok = platform::set_datetime(dt);
    if (hardware_ok) {
        platform::put_string("OK\r\n");
    } else {
        platform::put_string("OK - SOFTWARE CLOCK; ");
        platform::put_string(platform::datetime_last_error());
        platform::put_string("\r\n");
    }
}

void Repl::run() {
    platform::set_status_refresh_callback(
        [](void* context) {
            auto* repl = static_cast<Repl*>(context);
            if (repl->full_screen_editor_active_) return;
            repl->render_status();
            repl->render_function_keys();
        },
        this
    );
    platform::set_screenshot_callback(
        [](void* context) {
            static_cast<Repl*>(context)->capture_hotkey_screenshot();
        },
        this
    );
    platform::set_background_service_callback(
        [](void* context) { static_cast<Repl*>(context)->service_background(); }, this
    );
    platform::set_sleep_prepare_callback(
        [](void*) {
            platform::audio_stop();
            network::file_server_stop();
            bluetooth_manager::disable();
        },
        nullptr
    );

    bluetooth_manager::init(); // Lazy: leaves CYW43 Bluetooth powered OFF.
    storage::init();
    load_settings();
    apply_settings();

    platform::clear_lcd();
    print_banner();
    if (!program_.initialize(settings_.storage_mode)) {
        print_program_error();
    } else {
        set_current_filename(
            *program_.filename() ? program_.filename() : "UNTITLED",
            program_.is_dirty()
        );
        if (show_restored_editing_session(
                program_.session_recovered(), program_.is_dirty())) {
            platform::put_string("[RESTORED EDITING SESSION]\r\n");
        }
    }
    apply_network_settings();
    // Unsaved recovered edits take precedence over AUTORUN.BAS.
    if (!(program_.session_recovered() && program_.is_dirty())) run_autorun();
    if (settings_.startup_wav) {
        // Start after ProgramStore/AUTORUN.BAS have finished using SD, but
        // before the first READY prompt. Missing/invalid WAV never stops boot.
        platform::audio_wavplay("AUTORUN.WAV");
    }

    char input[kInputBufferSize] = {};

    while (true) {
        print_prompt();
        LineEditor::read(input, sizeof(input), &command_history_);

        const int special = LineEditor::last_special_key();
        if (special == kKeyHome) {
            show_system_menu();
            continue;
        }

        if (function_key_index(special) >= 0) {
            handle_quick_key(special);
            continue;
        }

        process_line(input);
    }
}

} // namespace rmb

