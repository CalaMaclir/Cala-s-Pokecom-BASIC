#pragma once
#include <cstdint>
inline std::uint64_t time_us_64() { static std::uint64_t t = 0; return ++t; }
inline void sleep_ms(std::uint32_t) {}
inline std::uint64_t get_absolute_time() { return time_us_64(); }
inline std::uint32_t to_ms_since_boot(std::uint64_t t) { return t/1000; }
inline std::uint64_t to_us_since_boot(std::uint64_t t) { return t; }
