#include "bluetooth_hid_keyboard_core.hpp"

#include <cstring>

namespace rmb::bluetooth_hid {

namespace {
constexpr std::uint8_t kModifierCtrl = 0x11;  // left/right Ctrl
constexpr std::uint8_t kModifierShift = 0x22; // left/right Shift
constexpr std::uint8_t kModifierAlt = 0x44;   // left/right Alt

constexpr int kKeyEscape = 0xb1;
constexpr int kKeyLeft = 0xb4;
constexpr int kKeyUp = 0xb5;
constexpr int kKeyDown = 0xb6;
constexpr int kKeyRight = 0xb7;
constexpr int kKeyHome = 0xd2;
constexpr int kKeyDelete = 0xd4;
constexpr int kKeyEnd = 0xd5;
constexpr int kKeyHotkeyScreenshot = 0xe2;
constexpr int kKeyHotkeySleep = 0xe3;

bool elapsed(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

int jis_symbol(std::uint8_t usage, bool shift) {
    switch (usage) {
    case 0x2d: return shift ? '=' : '-';
    case 0x2e: return shift ? '~' : '^';
    case 0x2f: return shift ? '`' : '@';
    case 0x30: return shift ? '{' : '[';
    case 0x31: return shift ? '}' : ']';
    case 0x32: return shift ? '|' : '\\';
    case 0x33: return shift ? '+' : ';';
    case 0x34: return shift ? '*' : ':';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    // International1 ("ro") and Non-US backslash vary between adapters.
    case 0x64:
    case 0x87: return shift ? '_' : '\\';
    // CPB is ASCII; the JIS Yen key is represented by ASCII backslash.
    case 0x89: return shift ? '|' : '\\';
    default: return -1;
    }
}

int us_symbol(std::uint8_t usage, bool shift) {
    switch (usage) {
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31:
    case 0x64: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return -1;
    }
}
} // namespace

void KeyboardCore::reset() {
    queue_read_ = queue_write_ = 0;
    std::memset(last_keys_, 0, sizeof(last_keys_));
    key_count_ = 0;
    modifiers_ = 0;
    repeat_usage_ = 0;
    repeat_due_ms_ = 0;
    caps_lock_ = false;
}

bool KeyboardCore::enqueue(int code) {
    if (code < 0) return false;
    const std::size_t next = (queue_write_ + 1u) % kQueueCapacity;
    if (next == queue_read_) {
        ++overflow_count_;
        return false;
    }
    queue_[queue_write_] = code;
    queue_write_ = next;
    return true;
}

bool KeyboardCore::was_pressed(std::uint8_t usage) const {
    for (std::size_t i = 0; i < key_count_; ++i) {
        if (last_keys_[i] == usage) return true;
    }
    return false;
}

bool KeyboardCore::is_pressed(std::uint8_t usage) const {
    return was_pressed(usage);
}

bool KeyboardCore::shift_held() const {
    return (modifiers_ & kModifierShift) != 0;
}

int KeyboardCore::translate_usage(
    KeyboardLayout layout,
    std::uint8_t usage,
    std::uint8_t modifiers,
    bool caps_lock
) {
    const bool shift = (modifiers & kModifierShift) != 0;
    const bool ctrl = (modifiers & kModifierCtrl) != 0;
    const bool alt = (modifiers & kModifierAlt) != 0;

    if (usage >= 0x04 && usage <= 0x1d) {
        const int index = usage - 0x04;
        const bool uppercase = caps_lock != shift;
        int code = (uppercase ? 'A' : 'a') + index;
        if (ctrl) code = index + 1;
        if (alt && index == ('s' - 'a')) return kKeyHotkeyScreenshot;
        if (alt && index == ('p' - 'a')) return kKeyHotkeySleep;
        return code;
    }

    if (usage >= 0x1e && usage <= 0x27) {
        static constexpr char plain[] = "1234567890";
        static constexpr char us_shift[] = "!@#$%^&*()";
        static constexpr char jis_shift[] = "!\"#$%&'()0";
        const std::size_t index = usage - 0x1e;
        return shift
            ? (layout == KeyboardLayout::Jis
                ? jis_shift[index] : us_shift[index])
            : plain[index];
    }

    switch (usage) {
    case 0x28: return '\n';
    case 0x29: return kKeyEscape;
    case 0x2a: return 0x08;
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x39: return -1; // Caps Lock is handled as state, not text.
    case 0x3a: return 0x81;
    case 0x3b: return 0x82;
    case 0x3c: return 0x83;
    case 0x3d: return 0x84;
    case 0x3e: return 0x85;
    case 0x3f: return 0x86;
    case 0x40: return 0x87;
    case 0x41: return 0x88;
    case 0x42: return 0x89;
    case 0x43: return 0x90;
    case 0x4a: return kKeyHome;
    case 0x4c: return kKeyDelete;
    case 0x4d: return kKeyEnd;
    case 0x4f: return kKeyRight;
    case 0x50: return kKeyLeft;
    case 0x51: return kKeyDown;
    case 0x52: return kKeyUp;
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case 0x58: return '\n';
    case 0x59: return '1';
    case 0x5a: return '2';
    case 0x5b: return '3';
    case 0x5c: return '4';
    case 0x5d: return '5';
    case 0x5e: return '6';
    case 0x5f: return '7';
    case 0x60: return '8';
    case 0x61: return '9';
    case 0x62: return '0';
    case 0x63: return '.';
    default: break;
    }

    return layout == KeyboardLayout::Jis
        ? jis_symbol(usage, shift)
        : us_symbol(usage, shift);
}

bool KeyboardCore::repeatable(int code) {
    if (code >= 0x20 && code <= 0x7e) return true;
    switch (code) {
    case 0x08:
    case kKeyLeft:
    case kKeyUp:
    case kKeyDown:
    case kKeyRight:
    case kKeyHome:
    case kKeyDelete:
    case kKeyEnd:
        return true;
    default:
        return false;
    }
}

void KeyboardCore::handle_report(
    std::uint8_t modifiers,
    const std::uint8_t* usages,
    std::size_t count,
    std::uint32_t now_ms
) {
    std::uint8_t current[kMaxKeys] = {};
    const std::size_t limited = count < kMaxKeys ? count : kMaxKeys;
    std::size_t current_count = 0;
    for (std::size_t i = 0; i < limited; ++i) {
        const std::uint8_t usage = usages ? usages[i] : 0;
        if (usage == 0 || usage == 1) continue;
        bool duplicate = false;
        for (std::size_t j = 0; j < current_count; ++j) {
            if (current[j] == usage) duplicate = true;
        }
        if (!duplicate) current[current_count++] = usage;
    }

    modifiers_ = modifiers;

    bool repeat_still_pressed = false;
    for (std::size_t i = 0; i < current_count; ++i) {
        const std::uint8_t usage = current[i];
        if (usage == repeat_usage_) repeat_still_pressed = true;
        if (was_pressed(usage)) continue;

        if (usage == 0x39) {
            caps_lock_ = !caps_lock_;
            enqueue(0xc1);
            continue;
        }

        const int code = translate_usage(layout_, usage, modifiers_, caps_lock_);
        if (!enqueue(code)) continue;
        if (repeatable(code)) {
            repeat_usage_ = usage;
            repeat_due_ms_ = now_ms + kRepeatDelayMs;
            repeat_still_pressed = true;
        }
    }

    if (repeat_usage_ != 0 && !repeat_still_pressed) {
        repeat_usage_ = 0;
        repeat_due_ms_ = 0;
    }

    std::memset(last_keys_, 0, sizeof(last_keys_));
    for (std::size_t i = 0; i < current_count; ++i) {
        last_keys_[i] = current[i];
    }
    key_count_ = static_cast<std::uint8_t>(current_count);
}

void KeyboardCore::service_repeat(std::uint32_t now_ms) {
    if (repeat_usage_ == 0 || !is_pressed(repeat_usage_) ||
        !elapsed(now_ms, repeat_due_ms_)) {
        return;
    }

    const int code =
        translate_usage(layout_, repeat_usage_, modifiers_, caps_lock_);
    if (repeatable(code)) enqueue(code);
    repeat_due_ms_ = now_ms + kRepeatIntervalMs;
}

int KeyboardCore::read_key(std::uint32_t now_ms) {
    service_repeat(now_ms);
    if (queue_read_ == queue_write_) return -1;
    const int code = queue_[queue_read_];
    queue_read_ = (queue_read_ + 1u) % kQueueCapacity;
    return code;
}

} // namespace rmb::bluetooth_hid
