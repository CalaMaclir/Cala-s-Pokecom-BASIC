#pragma once

#include <cstdint>

namespace rmb::editor_perf {

struct Counters {
    std::uint64_t key_events = 0;
    std::uint64_t navigation_steps = 0;
    std::uint64_t model_us = 0;
    std::uint64_t cursor_us = 0;
    std::uint64_t viewport_scans = 0;
    std::uint64_t metadata_reads = 0;
    std::uint64_t body_reads = 0;
    std::uint64_t sd_opens = 0;
    std::uint64_t full_renders = 0;
    std::uint64_t text_row_renders = 0;
    std::uint64_t burst_us = 0;
    std::uint64_t repeat_events = 0;
    std::uint64_t max_step_us = 0;
};

#ifdef RMB_EDITOR_PERF
inline Counters values{};
inline void reset() { values = {}; }
inline const Counters& snapshot() { return values; }
inline void key_event() { ++values.key_events; }
inline void navigation_step() { ++values.navigation_steps; }
inline void model_time(std::uint64_t us) {
    values.model_us += us;
    if (us > values.max_step_us) values.max_step_us = us;
}
inline void cursor_time(std::uint64_t us) { values.cursor_us += us; }
inline void viewport_scan() { ++values.viewport_scans; }
inline void metadata_read() { ++values.metadata_reads; }
inline void body_read() { ++values.body_reads; }
inline void sd_open() { ++values.sd_opens; }
inline void full_render() { ++values.full_renders; }
inline void text_row_render() { ++values.text_row_renders; }
inline void burst_time(std::uint64_t us) { values.burst_us += us; }
inline void repeat_event() { ++values.repeat_events; }
#else
inline void reset() {}
inline Counters snapshot() { return {}; }
inline void key_event() {}
inline void navigation_step() {}
inline void model_time(std::uint64_t) {}
inline void cursor_time(std::uint64_t) {}
inline void viewport_scan() {}
inline void metadata_read() {}
inline void body_read() {}
inline void sd_open() {}
inline void full_render() {}
inline void text_row_render() {}
inline void burst_time(std::uint64_t) {}
inline void repeat_event() {}
#endif

} // namespace rmb::editor_perf
