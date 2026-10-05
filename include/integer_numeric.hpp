#pragma once

#include "il.hpp"
#include <cmath>
#include <cstdint>
#include <limits>

namespace rmb {
// Check in float before casting. INT32_MAX rounds UP to 2^31 in BasicNumber,
// so comparing with float(INT32_MAX) would admit an undefined conversion.
inline bool basic_to_int32(BasicNumber value, std::int32_t& result) {
    if (!std::isfinite(value) || value < -2147483648.0f || value >= 2147483648.0f)
        return false;
    result = static_cast<std::int32_t>(std::trunc(value));
    return true;
}

inline std::int32_t signed_bit_pattern(std::uint32_t bits) {
    const std::int64_t value = bits <= 0x7fffffffu
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - 4294967296LL;
    return static_cast<std::int32_t>(value);
}

// Null indicates success; errors are BASIC diagnostics, never C++ UB.
[[gnu::noinline]] inline const char* evaluate_integer_numeric(OpCode op, BasicNumber lhs,
                                            BasicNumber rhs, BasicNumber& result) {
    std::int32_t a = 0, b = 0;
    if (!basic_to_int32(lhs, a) || !basic_to_int32(rhs, b))
        return "INTEGER RANGE ERROR";
    std::int32_t value = 0;
    if (op == OpCode::IDIV_NUM) {
        if (b == 0) return "DIVISION BY ZERO";
        const std::int64_t quotient = static_cast<std::int64_t>(a) / b;
        if (quotient < std::numeric_limits<std::int32_t>::min() ||
            quotient > std::numeric_limits<std::int32_t>::max())
            return "INTEGER OVERFLOW";
        value = static_cast<std::int32_t>(quotient);
    } else if (op == OpCode::BXOR_NUM) {
        value = signed_bit_pattern(static_cast<std::uint32_t>(a) ^
                                   static_cast<std::uint32_t>(b));
    } else {
        if (b < 0 || b > 31) return "SHIFT COUNT ERROR";
        std::uint32_t bits = static_cast<std::uint32_t>(a);
        if (op == OpCode::SHL_NUM) bits <<= b;
        else if (b != 0) {
            bits >>= b;
            if (a < 0) bits |= 0xffffffffu << (32 - b);
        }
        value = signed_bit_pattern(bits);
    }
    result = static_cast<BasicNumber>(value);
    return nullptr;
}
} // namespace rmb
