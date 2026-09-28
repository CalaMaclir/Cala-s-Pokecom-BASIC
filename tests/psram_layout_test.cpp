#include "psram_layout.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

int main() {
    using rmb::psram::detail::chunk_length;
    using rmb::psram::detail::range_valid;
    constexpr std::uint32_t capacity = 8u * 1024u * 1024u;
    assert(range_valid(capacity, 0, 0));
    assert(range_valid(capacity, 0, capacity));
    assert(range_valid(capacity, capacity, 0));
    assert(range_valid(capacity, capacity - 1, 1));
    assert(!range_valid(capacity, capacity, 1));
    assert(!range_valid(capacity, capacity + 1, 0));
    assert(!range_valid(
        capacity, 1, std::numeric_limits<std::size_t>::max()));
    assert(chunk_length(0, 31) == 0);
    assert(chunk_length(1, 31) == 1);
    assert(chunk_length(31, 31) == 31);
    assert(chunk_length(32, 31) == 31);
    assert(chunk_length(2047, 27) == 27);
    std::puts("PSRAM range/chunk policy: PASS");
}
