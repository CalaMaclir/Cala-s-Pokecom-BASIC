#pragma once
#include "il.hpp"
#ifdef RMB_STAGE5_INTEGER_FOR_EXPERIMENT
#include "integer_numeric.hpp"
#include <limits>
#endif

namespace rmb {
// Identical production layout. Experimental state never enters CompiledProgram
// or its serialized cache, and disappears completely from a normal build.
struct ForFrame {
    int slot = -1;
    BasicNumber end = 0.0;
    BasicNumber step = 1.0;
    std::int32_t check_pc = 0;
    std::int32_t body_pc = 0;
#ifdef RMB_STAGE5_INTEGER_FOR_EXPERIMENT
    std::int32_t integer_current = 0, integer_end = 0, integer_step = 1;
    bool integer_active = false;
#endif
};

#ifdef RMB_STAGE5_INTEGER_FOR_EXPERIMENT
inline void initialize_integer_for(ForFrame& frame, BasicNumber start) {
    frame.integer_active = basic_to_int32(start, frame.integer_current) &&
        basic_to_int32(frame.end, frame.integer_end) &&
        basic_to_int32(frame.step, frame.integer_step) && frame.integer_step != 0 &&
        start == static_cast<BasicNumber>(frame.integer_current) &&
        frame.end == static_cast<BasicNumber>(frame.integer_end) &&
        frame.step == static_cast<BasicNumber>(frame.integer_step);
}

inline bool integer_for_continues(const ForFrame& frame, BasicNumber current) {
    if (frame.integer_active)
        return frame.integer_step > 0 ? frame.integer_current <= frame.integer_end
                                      : frame.integer_current >= frame.integer_end;
    return frame.step >= 0 ? current <= frame.end : current >= frame.end;
}

// Overflow is reported at the original NEXT PC. No signed overflow or unsafe
// float conversion occurs. Body assignments to the counter revert to float.
inline bool increment_integer_for(ForFrame& frame, BasicNumber& slot) {
    if (frame.integer_active && slot != static_cast<BasicNumber>(frame.integer_current))
        frame.integer_active = false;
    if (!frame.integer_active) { slot += frame.step; return true; }
    const std::int64_t next = static_cast<std::int64_t>(frame.integer_current) + frame.integer_step;
    if (next < std::numeric_limits<std::int32_t>::min() ||
        next > std::numeric_limits<std::int32_t>::max()) return false;
    frame.integer_current = static_cast<std::int32_t>(next);
    slot = static_cast<BasicNumber>(frame.integer_current);
    return true;
}
#endif
} // namespace rmb
