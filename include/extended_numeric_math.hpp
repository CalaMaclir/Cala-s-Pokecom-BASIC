#pragma once

#include "il.hpp"
#include <cmath>

namespace rmb {
// Used by both numeric fast paths and generic CALLFN.
[[gnu::noinline]] inline bool evaluate_extended_unary(int id, BasicNumber x, BasicNumber& result) {
    if (!std::isfinite(x)) return false;
    if (id == FnId::LOG || id == FnId::LN) {
        if (x <= 0.0f) return false;
        result = id == FnId::LOG ? std::log10(x) : std::log(x);
    } else {
        if (x < -1.0f || x > 1.0f) return false;
        result = id == FnId::ASIN ? std::asin(x) : std::acos(x);
    }
    return true;
}

inline bool evaluate_atan2(BasicNumber y, BasicNumber x, BasicNumber& result) {
    if (!std::isfinite(y) || !std::isfinite(x)) return false;
    result = std::atan2(y, x);
    return true;
}
} // namespace rmb
