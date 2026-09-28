#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rmb::psram::detail {

inline bool range_valid(
    std::uint32_t capacity,
    std::uint32_t address,
    std::size_t length
) {
    return address <= capacity && length <= capacity - address;
}

inline std::size_t chunk_length(
    std::size_t remaining,
    std::size_t maximum
) {
    return std::min(remaining, maximum);
}

} // namespace rmb::psram::detail
