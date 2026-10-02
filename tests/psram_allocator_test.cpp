#include "psram_allocator.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    using rmb::psram::detail::RegionAllocator;
    RegionAllocator<5> allocator(1024);

    std::uint32_t base = 0, bytes = 0;
    assert(allocator.claim(0, 128, base, bytes));
    assert(base == 0 && bytes == 128);

    assert(allocator.claim(1, 100, base, bytes));
    assert(base == 128 && bytes == 112);
    assert(allocator.used_bytes() == 240);
    assert(allocator.active_count() == 2);

    allocator.release(0);
    assert(!allocator.active(0));
    assert(allocator.claim(2, 64, base, bytes));
    assert(base == 0 && bytes == 64);

    allocator.release(1);
    assert(allocator.claim(3, 160, base, bytes));
    assert(base == 64 && bytes == 160);

    // A client has exactly one live reservation. Duplicate claims fail so
    // two objects cannot accidentally alias the same PSRAM region.
    assert(!allocator.claim(3, 128, base, bytes));
    assert(!allocator.claim(3, 256, base, bytes));

    // Fill the remaining space without overlap, then verify exhaustion.
    assert(allocator.claim(4, 800, base, bytes));
    assert(base == 224 && bytes == 800);
    assert(allocator.used_bytes() == 1024);
    assert(!allocator.claim(0, 16, base, bytes));

    allocator.reset(256);
    assert(allocator.used_bytes() == 0);
    assert(allocator.active_count() == 0);
    assert(allocator.claim(0, 1, base, bytes));
    assert(bytes == 16);

    std::puts("PSRAM multi-client region allocator: PASS");
}
