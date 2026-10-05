#pragma once
// Native process resources are evidence, not RP2350 heap/PSRAM measurements.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
namespace cpb_soak {
inline unsigned iterations() {
    const char* value = std::getenv("CPB_SOAK_ITERATIONS");
    if (!value) return 200;
    char* end = nullptr;
    const auto count = std::strtoul(value, &end, 10);
    assert(end != value && *end == 0 && count >= 1 && count <= 2000);
    return static_cast<unsigned>(count);
}
struct Resources { long handles = -1, heap = -1; };
inline Resources resources() {
    Resources result;
#if defined(__linux__)
    result.handles = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry; ++result.handles;
    }
#endif
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
    result.heap = static_cast<long>(mallinfo2().uordblks);
#endif
    return result;
}
inline void record(const char* scope, unsigned cycle, const Resources& baseline) {
    const auto now = resources();
    // The iterator's temporary descriptor is included consistently in both samples.
    assert(baseline.handles < 0 || now.handles == baseline.handles);
    std::printf("SOAK scope=%s cycle=%u handles=%ld baseline_handles=%ld host_heap=%ld baseline_heap=%ld\n",
        scope, cycle, now.handles, baseline.handles, now.heap, baseline.heap);
}
}
