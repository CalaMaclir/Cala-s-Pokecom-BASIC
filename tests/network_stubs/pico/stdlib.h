#pragma once
#include "mock_network.hpp"
inline std::uint64_t time_us_64() { return mock_network::now * 1000; }
inline absolute_time_t get_absolute_time() { return mock_network::now; }
inline std::uint32_t to_ms_since_boot(absolute_time_t t) { return t; }
inline std::uint64_t to_us_since_boot(absolute_time_t t) { return t * 1000; }
