#pragma once

#include <cstdint>

namespace rmb::system_controls {

enum class CpuProfile : std::uint16_t {
    Unsupported = 0,
    Eco = 75,
    Normal = 100,
    Full = 150,
    Experimental = 200
};

constexpr CpuProfile cpu_profile_for_mhz(std::uint32_t mhz) {
    return mhz == 75u ? CpuProfile::Eco :
           mhz == 100u ? CpuProfile::Normal :
           mhz == 150u ? CpuProfile::Full :
           mhz == 200u ? CpuProfile::Experimental :
           CpuProfile::Unsupported;
}

constexpr bool cpu_profile_supported(std::uint32_t mhz) {
    return cpu_profile_for_mhz(mhz) != CpuProfile::Unsupported;
}

constexpr bool cpu_profile_experimental(std::uint32_t mhz) {
    return cpu_profile_for_mhz(mhz) == CpuProfile::Experimental;
}

// Persisted CPU values are intentionally ignored. Every reset begins at the
// RP2350-rated 150 MHz and overclocking is enabled only for the live session.
constexpr std::uint16_t safe_boot_cpu_mhz(std::uint32_t = 0) {
    return 150u;
}

constexpr const char* cpu_profile_status_name(std::uint32_t mhz) {
    return mhz == 75u ? "ECO" :
           mhz == 100u ? "NORMAL" :
           mhz == 150u ? "FULL" :
           mhz == 200u ? "EXP" : "?";
}

enum class BoardLedMode : std::uint8_t {
    Off,
    On,
    Heartbeat
};

constexpr char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool setting_equals(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        if (ascii_lower(*left++) != ascii_lower(*right++)) return false;
    }
    return *left == '\0' && *right == '\0';
}

inline BoardLedMode board_led_mode_from_setting(const char* value) {
    if (setting_equals(value, "on")) return BoardLedMode::On;
    if (setting_equals(value, "heartbeat")) return BoardLedMode::Heartbeat;
    return BoardLedMode::Off;
}

constexpr const char* board_led_mode_name(BoardLedMode mode) {
    return mode == BoardLedMode::On ? "ON" :
           mode == BoardLedMode::Heartbeat ? "HEARTBEAT" : "OFF";
}

constexpr const char* board_led_mode_setting(BoardLedMode mode) {
    return mode == BoardLedMode::On ? "on" :
           mode == BoardLedMode::Heartbeat ? "heartbeat" : "off";
}

constexpr bool board_led_output(BoardLedMode mode, std::uint32_t now_ms) {
    return mode == BoardLedMode::On ||
           (mode == BoardLedMode::Heartbeat && (now_ms % 1000u) < 120u);
}

} // namespace rmb::system_controls
