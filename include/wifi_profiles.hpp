#pragma once

#include "network.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rmb::wifi_profiles {
constexpr int kCount = 5;
enum class ProfileAction { ConnectNow, ToggleEnabled, Delete, Back, Count };
constexpr int kProfileActionCount = static_cast<int>(ProfileAction::Count);
struct WifiProfile {
    bool used = false;
    bool enabled = true;
    char ssid[33] = {};
    char password[64] = {};
};
using Profiles = WifiProfile[kCount];
struct Candidate { int slot; int rssi; };
struct AutoConnectPlan {
    Candidate items[kCount] = {};
    int count = 0;
    int next = 0;
};

inline int count(const Profiles& profiles, bool enabled_only = false) {
    int n = 0;
    for (const auto& p : profiles) n += p.used && (!enabled_only || p.enabled);
    return n;
}
inline int find(const Profiles& profiles, const char* ssid) {
    if (!ssid || !*ssid) return -1;
    for (int i = 0; i < kCount; ++i)
        if (profiles[i].used && std::strcmp(profiles[i].ssid, ssid) == 0) return i;
    return -1;
}
inline int available_slot(const Profiles& profiles, const char* ssid) {
    const int existing = find(profiles, ssid);
    if (existing >= 0) return existing;
    for (int i = 0; i < kCount; ++i) if (!profiles[i].used) return i;
    return -1;
}
inline bool valid_field(const char* value, std::size_t max) {
    return value && std::strlen(value) <= max && !std::strpbrk(value, "\r\n");
}
// No mutation on invalid input or a full list. Duplicate SSIDs preserve enabled.
inline int upsert(Profiles& profiles, const char* ssid, const char* password) {
    if (!valid_field(ssid, 32) || !*ssid || !valid_field(password, 63)) return -1;
    const int slot = available_slot(profiles, ssid);
    if (slot < 0) return -1;
    auto& p = profiles[slot];
    const bool enabled = p.used ? p.enabled : true;
    WifiProfile next;
    next.used = true; next.enabled = enabled;
    std::memcpy(next.ssid, ssid, std::strlen(ssid) + 1);
    std::memcpy(next.password, password, std::strlen(password) + 1);
    p = next;
    return slot;
}
inline void erase(Profiles& profiles, int slot) {
    if (slot < 0 || slot >= kCount) return;
    profiles[slot] = WifiProfile{};
    profiles[slot].enabled = false;
}
inline void normalize(Profiles& profiles) {
    for (int i = 0; i < kCount; ++i) {
        auto& p = profiles[i];
        p.used = p.ssid[0] != '\0';
        if (!p.used) { erase(profiles, i); continue; }
        for (int j = 0; j < i; ++j)
            if (profiles[j].used && std::strcmp(p.ssid, profiles[j].ssid) == 0) {
                erase(profiles, i); break;
            }
    }
}
inline void migrate(Profiles& profiles, bool has_profile_keys,
                    const char* legacy_ssid, const char* legacy_password) {
    normalize(profiles);
    // Even explicit empty new slots are authoritative (all networks deleted).
    if (!has_profile_keys && count(profiles) == 0 && legacy_ssid && *legacy_ssid)
        (void)upsert(profiles, legacy_ssid, legacy_password ? legacy_password : "");
}
// Parse a whole raw config line before the generic reader trims value whitespace.
inline bool parse_line(Profiles& profiles, const char* line) {
    if (!line || std::strncmp(line, "wifi_profile", 12) != 0) return false;
    const int slot = line[12] - '1';
    if (slot < 0 || slot >= kCount || line[13] != '_') return false;
    const char* key = line + 14;
    auto& p = profiles[slot];
    if (std::strncmp(key, "enabled=", 8) == 0) {
        const char* value = key + 8;
        p.enabled = std::strcmp(value, "on") == 0 || std::strcmp(value, "1") == 0 ||
                    std::strcmp(value, "true") == 0 || std::strcmp(value, "yes") == 0;
    } else if (std::strncmp(key, "ssid=", 5) == 0) {
        if (valid_field(key + 5, 32)) std::strcpy(p.ssid, key + 5);
    } else if (std::strncmp(key, "password=", 9) == 0) {
        if (valid_field(key + 9, 63)) std::strcpy(p.password, key + 9);
    } else return false;
    return true;
}
// Upper bound is 850 bytes, including unused slots, plus terminator.
inline int serialize(const Profiles& profiles, char* out, std::size_t capacity) {
    std::size_t used = 0;
    for (int i = 0; i < kCount; ++i) {
        const auto& p = profiles[i];
        const int n = std::snprintf(out + used, capacity - used,
            "wifi_profile%d_enabled=%s\nwifi_profile%d_ssid=%s\nwifi_profile%d_password=%s\n",
            i + 1, p.used && p.enabled ? "on" : "off", i + 1, p.used ? p.ssid : "",
            i + 1, p.used ? p.password : "");
        if (n < 0 || static_cast<std::size_t>(n) >= capacity - used) return -1;
        used += static_cast<std::size_t>(n);
    }
    return static_cast<int>(used);
}
inline int candidates(const Profiles& profiles, const network::AccessPoint* aps,
                      int ap_count, Candidate (&out)[kCount]) {
    int n = 0;
    for (int slot = 0; slot < kCount; ++slot) {
        if (!profiles[slot].used || !profiles[slot].enabled) continue;
        bool found = false; int strongest = -32768;
        for (int i = 0; i < ap_count; ++i)
            if (std::strcmp(profiles[slot].ssid, aps[i].ssid) == 0) {
                found = true; strongest = std::max(strongest, aps[i].rssi);
            }
        if (found) out[n++] = {slot, strongest};
    }
    std::sort(out, out + n, [](const Candidate& a, const Candidate& b) {
        return a.rssi != b.rssi ? a.rssi > b.rssi : a.slot < b.slot;
    });
    return n;
}
inline AutoConnectPlan make_plan(const Profiles& profiles,
                                 const network::AccessPoint* aps,
                                 int ap_count) {
    AutoConnectPlan plan;
    plan.count = candidates(profiles, aps, ap_count, plan.items);
    return plan;
}
inline bool next_candidate(AutoConnectPlan& plan, Candidate& candidate) {
    if (plan.next < 0 || plan.next >= plan.count) return false;
    candidate = plan.items[plan.next++];
    return true;
}
} // namespace rmb::wifi_profiles
