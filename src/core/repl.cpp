#include "repl.hpp"
#include "session_notice.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "basic_compiler.hpp"
#include "bluetooth_serial.hpp"
#include "console_layout.hpp"
#include "line_editor.hpp"
#include "network.hpp"
#include "file_server.hpp"
#include "platform.hpp"
#include "storage.hpp"
#include "storage_recovery.hpp"
#include "vm.hpp"
#include "serial_transfer.hpp"
#include "transfer_file.hpp"
#include "usb_device.hpp"
#include "usb_msc.hpp"
#include "usb_storage_menu.hpp"
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
constexpr std::size_t kMenuFileCount = 48;
constexpr std::size_t kMenuFilenameSize = 80;

constexpr int kKeyEnter = 0x0a;
constexpr int kKeyCarriageReturn = 0x0d;
constexpr int kKeyEscape = 0xb1;
constexpr int kKeyLeft = 0xb4;
constexpr int kKeyUp = 0xb5;
constexpr int kKeyDown = 0xb6;
constexpr int kKeyRight = 0xb7;
constexpr int kKeyHome = 0xd2;
constexpr int kKeyDelete = 0xd4;
constexpr int kKeyPageUp = 0xd6;
constexpr int kKeyPageDown = 0xd7;

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
            settings_.cpu_mhz = 150;
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
        "theme=%u\n"
        "console_fg=%06lX\n"
        "console_bg=%06lX\n"
        "console=%s\n"
        "\n[rtc]\n"
        "rtc_source=%s\n"
        "rtc_address=0x%02X\n"
        "\n[audio]\n"
        "audio_volume=%u\n"
        "key_click=%s\n"
        "startup_wav=%s\n"
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
        static_cast<unsigned>(settings_.theme),
        static_cast<unsigned long>(settings_.console_foreground),
        static_cast<unsigned long>(settings_.console_background),
        console_name(settings_.console_mode),
        platform::rtc_source_name(),
        static_cast<unsigned>(platform::rtc_address()),
        static_cast<unsigned>(settings_.audio_volume),
        audio::key_click_name(settings_.key_click),
        settings_.startup_wav ? "on" : "off",
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
    platform::audio_set_key_click(settings_.key_click);
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
    const char* profile = cpu_mhz == 150 ? "FULL" :
                          cpu_mhz == 100 ? "NORMAL" :
                          cpu_mhz == 75 ? "ECO" : "?";
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
        platform::put_string("LIST / NEW / RUN / CLS / MENU / STANDBY\r\n");
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
        platform::put_string("Bluetooth Console: Control Center -> Bluetooth -> Console ON\r\n");
        platform::put_string("Bluetooth X/YMODEM: enter XRECV/XSEND/YRECV/YSEND over SPP\r\n");
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

    char files[kMenuFileCount][kMenuFilenameSize] = {};
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

    int selected = 0;
    int offset = 0;
    constexpr int visible = 27;

    while (true) {
        draw_menu_header(
            "SELECT BASIC FILE",
            "UP/DOWN SELECT  ENTER OK  ESC CANCEL"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        if (selected < offset) offset = selected;
        if (selected >= offset + visible) {
            offset = selected - visible + 1;
        }

        for (int i = 0; i < visible; ++i) {
            const int index = offset + i;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + i, "", false);
                continue;
            }

            char row[96] = {};
            std::snprintf(
                row,
                sizeof(row),
                "%2d  %-46.46s",
                index + 1,
                files[index]
            );
            draw_menu_option(
                first_row + i,
                row,
                index == selected
            );
        }

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown &&
                   selected + 1 < static_cast<int>(count)) {
            ++selected;
        } else if (key == kKeyPageUp) {
            selected -= visible;
            if (selected < 0) selected = 0;
        } else if (key == kKeyPageDown) {
            selected += visible;
            if (selected >= static_cast<int>(count)) {
                selected = static_cast<int>(count) - 1;
            }
        } else if (key_is_enter(key)) {
            std::snprintf(output, capacity, "%s", files[selected]);
            return true;
        } else if (key == kKeyEscape || key == 0x1b) {
            return false;
        }
    }
}

bool Repl::pick_transfer_file(
    char* output,
    std::size_t capacity,
    const char* title
) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';

    char files[kMenuFileCount][kMenuFilenameSize] = {};
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
    int selected = 0;
    int offset = 0;
    constexpr int visible = 27;
    while (true) {
        draw_menu_header(title, "UP/DOWN SELECT  ENTER SEND  ESC CANCEL");
        const int first_row =
            (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        if (selected < offset) offset = selected;
        if (selected >= offset + visible) offset = selected - visible + 1;

        for (int i = 0; i < visible; ++i) {
            const int index = offset + i;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + i, "", false);
                continue;
            }
            char row[96] = {};
            std::snprintf(row, sizeof(row), "%2d  %-46.46s", index + 1, files[index]);
            draw_menu_option(first_row + i, row, index == selected);
        }

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) --selected;
        else if (key == kKeyDown && selected + 1 < static_cast<int>(count)) ++selected;
        else if (key == kKeyPageUp) {
            selected -= visible;
            if (selected < 0) selected = 0;
        } else if (key == kKeyPageDown) {
            selected += visible;
            if (selected >= static_cast<int>(count)) selected = static_cast<int>(count) - 1;
        } else if (key_is_enter(key)) {
            std::snprintf(output, capacity, "%s", files[selected]);
            return true;
        } else if (key == kKeyEscape || key == 0x1b) {
            return false;
        }
    }
}

bool Repl::menu_files() {
    char files[kMenuFileCount][kMenuFilenameSize] = {};
    const std::size_t count = storage::collect_program_files(
        &files[0][0],
        kMenuFileCount,
        kMenuFilenameSize
    );

    if (count == 0) {
        draw_menu_header("FILES", "ESC BACK");
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

    int selected = 0;
    int offset = 0;
    constexpr int visible = 27;

    while (true) {
        draw_menu_header(
            "FILES",
            "ENTER LOAD  R RUN  F1-F10 ASSIGN  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        if (selected < offset) offset = selected;
        if (selected >= offset + visible) {
            offset = selected - visible + 1;
        }

        for (int i = 0; i < visible; ++i) {
            const int index = offset + i;
            if (index >= static_cast<int>(count)) {
                draw_menu_option(first_row + i, "", false);
                continue;
            }

            char row[96] = {};
            std::snprintf(
                row,
                sizeof(row),
                "%2d  %-46.46s",
                index + 1,
                files[index]
            );
            draw_menu_option(
                first_row + i,
                row,
                index == selected
            );
        }

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown &&
                   selected + 1 < static_cast<int>(count)) {
            ++selected;
        } else if (key == kKeyPageUp) {
            selected -= visible;
            if (selected < 0) selected = 0;
        } else if (key == kKeyPageDown) {
            selected += visible;
            if (selected >= static_cast<int>(count)) {
                selected = static_cast<int>(count) - 1;
            }
        } else if (key_is_enter(key)) {
            leave_menu_screen();
            load_named_program(files[selected], false);
            return true;
        } else if (key == 'r' || key == 'R') {
            leave_menu_screen();
            load_named_program(files[selected], true);
            return true;
        } else if (key == kKeyEscape || key == 0x1b) {
            return false;
        } else {
            const int quick = function_key_index(key);
            if (quick >= 0) {
                bool run = true;
                if (choose_quick_mode(run)) {
                    assign_quick_key(quick, files[selected], run);
                }
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
    static const std::uint16_t profiles[] = {150, 100, 75};
    static const char* names[] = {
        "FULL    150 MHz",
        "NORMAL  100 MHz",
        "ECO      75 MHz"
    };

    int selected = 0;
    for (int i = 0; i < 3; ++i) {
        if (settings_.cpu_mhz == profiles[i]) {
            selected = i;
            break;
        }
    }

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

        for (int i = 0; i < 3; ++i) {
            char option[64] = {};
            std::snprintf(
                option,
                sizeof(option),
                "%s%s",
                names[i],
                settings_.cpu_mhz == profiles[i] ? "  *" : ""
            );
            draw_menu_option(
                first_row + 2 + i,
                option,
                selected == i
            );
        }

        draw_menu_option(
            first_row + 6,
            "STANDBY NOW",
            selected == 3
        );

        draw_menu_message(
            first_row + 10,
            "CPU profile changes clk_sys only; peripheral clocks stay stable."
        );
        draw_menu_message(
            first_row + 11,
            "STANDBY preserves RAM and uses RP2350 low-power sleep."
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 3) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return;
        } else if (key_is_enter(key)) {
            if (selected < 3) {
                if (platform::set_cpu_clock_mhz(profiles[selected])) {
                    settings_.cpu_mhz = profiles[selected];
                } else {
                    draw_menu_message(
                        first_row + 13,
                        "CPU CLOCK CHANGE FAILED"
                    );
                    platform::sleep_millis(700);
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
    std::size_t capacity
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
    const std::size_t length = LineEditor::read(output, capacity);
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

    int selected = 0;
    constexpr int visible = 18;

    while (true) {
        draw_menu_header(
            "SELECT WIRELESS NETWORK",
            "UP/DOWN SELECT  ENTER USE  ESC BACK"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;
        int first = selected - visible + 1;
        if (first < 0) first = 0;
        if (first + visible > count) {
            first = count - visible;
            if (first < 0) first = 0;
        }

        for (int row_index = 0; row_index < visible; ++row_index) {
            const int i = first + row_index;
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
                i == selected
            );
        }

        draw_menu_message(
            first_row + visible + 1,
            "* = secured network"
        );

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected + 1 < count) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b) {
            return false;
        } else if (key_is_enter(key)) {
            std::snprintf(
                ssid,
                capacity,
                "%s",
                access_points[selected].ssid
            );
            secure = access_points[selected].secure;
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
 int selected=0; while(true){platform::DateTime dt;platform::get_datetime(dt);draw_menu_header("RTC SETTINGS","UP/DOWN SELECT  ENTER CHANGE  ESC BACK");const int first=(settings_.status_enabled?console_layout::status_rows:0)+3;char row[80]={};std::snprintf(row,sizeof(row),"Source        %s",platform::rtc_source_name());draw_menu_option(first,row,selected==0);draw_menu_message(first+1,"Type          PCF8563");std::snprintf(row,sizeof(row),"Address       0x%02X (target)",static_cast<unsigned>(platform::rtc_address()));draw_menu_option(first+2,row,selected==1);draw_menu_message(first+4,"External SDA  GP4");draw_menu_message(first+5,"External SCL  GP5");draw_menu_message(first+6,"I2C Speed     100 kHz");draw_menu_option(first+8,"Probe RTC",selected==2);draw_menu_option(first+9,"Set date & time",selected==3);draw_menu_option(first+10,"Back",selected==4);const int key=platform::get_char();if(key==kKeyUp&&selected>0){--selected;continue;}if(key==kKeyDown&&selected<4){++selected;continue;}if(key==kKeyEscape||key==0x1b||(key_is_enter(key)&&selected==4))return;if(!key_is_enter(key))continue;if(selected==0){const int current=platform::rtc_source()==platform::RtcSource::Auto?0:platform::rtc_source()==platform::RtcSource::External?1:platform::rtc_source()==platform::RtcSource::Internal?2:3;platform::set_rtc_source(static_cast<platform::RtcSource>((current+1)%4));save_settings();}else if(selected==1){char input[16]={};if(prompt_text("RTC target address (08-77 hex): ",input,sizeof(input))){char* end=nullptr;const long address=std::strtol(input,&end,16);if(!platform::set_rtc_address(static_cast<std::uint8_t>(address)))platform::put_string("BAD I2C ADDRESS\r\n");else save_settings();}}else if(selected==2){const bool found=platform::probe_rtc();draw_menu_header("RTC PROBE","PRESS ANY KEY");if(found){std::snprintf(row,sizeof(row),"PCF8563 FOUND %s 0x%02X",platform::rtc_location(),static_cast<unsigned>(platform::rtc_address()));draw_menu_message(first+4,row);}else draw_menu_message(first+4,"RTC NOT FOUND");platform::get_char();}else {char input[48]={};if(prompt_text("SET YYYYMMDDHHMMSS: ",input,sizeof(input))){platform::DateTime value;if(parse_datetime_value(input,value)&&platform::set_datetime(value))platform::put_string("CLOCK UPDATED\r\n");else platform::put_string("INVALID DATE/TIME\r\n");}}}
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
        std::snprintf(
            row, sizeof(row), "Volume       %u%%",
            static_cast<unsigned>(settings_.audio_volume)
        );
        draw_menu_option(first, row, selected == 0);
        std::snprintf(
            row, sizeof(row), "Startup WAV  %s",
            settings_.startup_wav ? "ON" : "OFF"
        );
        draw_menu_option(first + 1, row, selected == 1);
        std::snprintf(row, sizeof(row), "Key Click    %s",
                      audio::key_click_name(settings_.key_click));
        draw_menu_option(first + 2, row, selected == 2);
        draw_menu_option(first + 4, "Back", selected == 3);

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) {
            --selected;
        } else if (key == kKeyDown && selected < 3) {
            ++selected;
        } else if (key == kKeyEscape || key == 0x1b ||
                   (key_is_enter(key) && selected == 3)) {
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
            settings_.startup_wav = !settings_.startup_wav;
            save_settings();
        } else if (key_is_enter(key) && selected == 2) {
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
    network::file_server_poll();
    bluetooth_serial::service();

    const bool console_link=bluetooth_serial::console_enabled()&&bluetooth_serial::connected()&&!bluetooth_serial::test_terminal_active();
    if(console_link&&!bluetooth_console_link_active_)bluetooth_serial::write_console_text("\r\nREADY\r\nBASIC> \x1b[s");
    bluetooth_console_link_active_=console_link;

    // Stage 1 terminal behavior is deliberately outside the transport core.
    if (bluetooth_test_active_) {
        if (!bluetooth_serial::connected()) {
            bluetooth_test_banner_sent_ = false;
        } else {
            static constexpr char banner[] =
                "CPB Bluetooth SPP Stage 1\r\n";
            if (!bluetooth_test_banner_sent_ &&
                bluetooth_serial::write_text(banner)) {
                bluetooth_test_banner_sent_ = true;
            }
            for (int drained = 0; drained < 64; ++drained) {
                const int value = bluetooth_serial::read_test();
                if (value < 0) break;
                ++bluetooth_test_rx_count_;
                if (value >= 32 && value <= 126) {
                    std::snprintf(bluetooth_test_last_rx_, sizeof(bluetooth_test_last_rx_),
                                  "'%c' 0x%02X", value, static_cast<unsigned>(value));
                } else {
                    std::snprintf(bluetooth_test_last_rx_, sizeof(bluetooth_test_last_rx_),
                                  "0x%02X", static_cast<unsigned>(value));
                }
                if (bluetooth_test_echo_) {
                    const std::uint8_t byte = static_cast<std::uint8_t>(value);
                    bluetooth_serial::write(&byte, 1);
                }
            }
            bluetooth_serial::service();
        }
    }

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


void Repl::menu_bluetooth_test() {
    if (!bluetooth_serial::enabled()) {
        draw_menu_header("BLUETOOTH TEST TERMINAL", "PRESS ANY KEY");
        const int row = (settings_.status_enabled ? console_layout::status_rows : 0) + 8;
        draw_menu_message(row, "ENABLE BLUETOOTH FIRST");
        platform::get_char();
        return;
    }

    bluetooth_serial::set_test_terminal_active(true);
    bluetooth_test_active_ = true;
    bluetooth_test_echo_ = true;
    bluetooth_test_banner_sent_ = false;
    bluetooth_test_rx_count_ = 0;
    std::snprintf(bluetooth_test_last_rx_, sizeof(bluetooth_test_last_rx_), "-");

    int selected = 0;
    while (true) {
        service_background();
        draw_menu_header("BLUETOOTH SPP TEST TERMINAL",
                         "UP/DOWN SELECT  ENTER  ESC BACK");
        const int first = (settings_.status_enabled ? console_layout::status_rows : 0) + 3;
        char row[96] = {};
        std::snprintf(row, sizeof(row), "Status      %s", bluetooth_serial::status());
        draw_menu_message(first, row);
        std::snprintf(row, sizeof(row), "RX/Q/S  %lu / %lu / %lu",
                      static_cast<unsigned long>(bluetooth_test_rx_count_),
                      static_cast<unsigned long>(bluetooth_serial::tx_queued_bytes()),
                      static_cast<unsigned long>(bluetooth_serial::tx_sent_bytes()));
        draw_menu_message(first + 1, row);
        std::snprintf(row, sizeof(row), "Last RX     %s", bluetooth_test_last_rx_);
        draw_menu_message(first + 2, row);
        draw_menu_message(first + 4,
                          "PC input is echoed; press any PicoCalc key to refresh.");

        draw_menu_option(first + 7, "Send Test Message", selected == 0);
        std::snprintf(row, sizeof(row), "Echo              %s",
                      bluetooth_test_echo_ ? "ON" : "OFF");
        draw_menu_option(first + 8, row, selected == 1);
        draw_menu_option(first + 9, "Back", selected == 2);

        const int key = platform::get_char();
        if (key == kKeyUp && selected > 0) --selected;
        else if (key == kKeyDown && selected < 2) ++selected;
        else if (key == kKeyEscape || key == 0x1b ||
                 (key_is_enter(key) && selected == 2)) break;
        else if (key_is_enter(key) && selected == 0) {
            static constexpr char message[] = "CPB-PicoCalc test message\r\n";
            if (bluetooth_serial::write_text(message)) {
                bluetooth_serial::service();
            }
        } else if (key_is_enter(key) && selected == 1) {
            bluetooth_test_echo_ = !bluetooth_test_echo_;
        }
    }
    bluetooth_test_active_ = false;
    bluetooth_test_banner_sent_ = false;
    bluetooth_serial::set_test_terminal_active(false);
}

void Repl::menu_bluetooth() {
 int selected=0;
 while(true){
  service_background();draw_menu_header("BLUETOOTH CLASSIC SPP","UP/DOWN SELECT  ENTER ACTION  ESC BACK");
  const int first=(settings_.status_enabled?console_layout::status_rows:0)+3;char row[96]={};
  std::snprintf(row,sizeof(row),"Bluetooth   %s",bluetooth_serial::enabled()?"ON":"OFF");draw_menu_message(first,row);
  std::snprintf(row,sizeof(row),"Status      %s",bluetooth_serial::status());draw_menu_message(first+1,row);
  std::snprintf(row,sizeof(row),"Device      %s",bluetooth_serial::device_name());draw_menu_message(first+2,row);
  std::snprintf(row,sizeof(row),"Console     %s",bluetooth_serial::console_enabled()?"ON":"OFF");draw_menu_message(first+3,row);
  std::snprintf(row,sizeof(row),"Overflow    RX:%lu TX:%lu",static_cast<unsigned long>(bluetooth_serial::rx_overflow_count()),static_cast<unsigned long>(bluetooth_serial::tx_overflow_count()));draw_menu_message(first+4,row);
  std::snprintf(row,sizeof(row),"Last status %.72s",bluetooth_serial::last_error());draw_menu_message(first+5,row);
  draw_menu_option(first+8,bluetooth_serial::enabled()?"Disable Bluetooth":"Enable Bluetooth",selected==0);
  draw_menu_option(first+9,bluetooth_serial::console_enabled()?"Console: ON":"Console: OFF",selected==1);
  draw_menu_option(first+10,"Test Terminal",selected==2);draw_menu_option(first+11,"Back",selected==3);
  const int key=platform::get_char();
  if(key==kKeyUp&&selected>0)--selected;else if(key==kKeyDown&&selected<3)++selected;
  else if(key==kKeyEscape||key==0x1b||(key_is_enter(key)&&selected==3))return;
  else if(key_is_enter(key)&&selected==0){if(bluetooth_serial::enabled()){bluetooth_serial::set_console_enabled(false);bluetooth_serial::disable();bluetooth_console_link_active_=false;}else bluetooth_serial::enable();}
  else if(key_is_enter(key)&&selected==1){if(bluetooth_serial::enabled()){bluetooth_serial::set_console_enabled(!bluetooth_serial::console_enabled());bluetooth_console_link_active_=false;}}
  else if(key_is_enter(key)&&selected==2)menu_bluetooth_test();
 }
}

void Repl::show_system_menu() {
    // Keep both channels available while inside the control center. The
    // configured routing is restored when the menu closes.
    platform::set_console_mode(platform::ConsoleMode::Both);

    static const char* items[] = {
        "Files",
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
        "System Information",
        "Program Storage",
        "Exit"
    };

    constexpr int item_count =
        static_cast<int>(sizeof(items) / sizeof(items[0]));

    int selected = 0;

    while (true) {
        draw_menu_header(
            "CALA\'S POKECOM BASIC CONTROL CENTER",
            "UP/DOWN SELECT  ENTER OPEN  ESC/HOME EXIT"
        );

        const int top = settings_.status_enabled ? console_layout::status_rows : 0;
        const int first_row = top + 3;

        for (int i = 0; i < item_count; ++i) {
            char row[80] = {};
            std::snprintf(row, sizeof(row), "  %s", items[i]);
            draw_menu_option(first_row + i, row, i == selected);
        }

        const int key = platform::get_char();

        if (key == kKeyUp && selected > 0) {
            --selected;
            continue;
        }
        if (key == kKeyDown && selected + 1 < item_count) {
            ++selected;
            continue;
        }
        if (key == kKeyEscape || key == 0x1b || key == kKeyHome) {
            break;
        }
        if (!key_is_enter(key)) {
            continue;
        }

        if (selected == 0) {
            if (menu_files()) {
                platform::set_console_mode(settings_.console_mode);
                return;
            }
        } else if (selected == 1) {
            menu_save_program(false);
        } else if (selected == 2) {
            menu_save_program(true);
        } else if (selected == 3) {
            menu_quick_keys();
        } else if (selected == 4) {
            menu_display();
        } else if (selected == 5) {
            menu_console();
        } else if (selected == 6) {
            menu_datetime();
        } else if (selected == 7) {
            menu_audio();
        } else if (selected == 8) {
            menu_wifi();
        } else if (selected == 9) {
            menu_bluetooth();
        } else if (selected == 10) {
            menu_file_server();
        } else if (selected == 11) {
            menu_file_transfer();
        } else if (selected == 12) {
            menu_usb_storage();
        } else if (selected == 13) {
            menu_sd();
        } else if (selected == 14) {
            menu_firmware();
        } else if (selected == 15) {
            menu_power();
        } else if (selected == 16) {
            menu_system_info();
        } else if (selected == 17) {
            menu_program_storage();
        } else {
            break;
        }
    }

    save_settings();
    leave_menu_screen();
    platform::set_console_mode(settings_.console_mode);
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
        case platform::SerialTransferRoute::Bluetooth: return "Bluetooth SPP";
        }
        return "AUTO";
    };
    auto cycle_route = [](platform::SerialTransferRoute route, int direction) {
        int value = static_cast<int>(route);
        value = (value + direction + 4) % 4;
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
            bluetooth_serial::disable();
        },
        nullptr
    );

    bluetooth_serial::init(); // Lazy: leaves CYW43 Bluetooth powered OFF.
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
        LineEditor::read(input, sizeof(input));

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
