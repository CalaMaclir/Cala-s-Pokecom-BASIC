#pragma once
#include "il.hpp"
namespace rmb {
// One active SRAM allocation: header, numeric slots, then 128-byte string slots.
// Stored separately from globals and the native C++ call stack.
struct UserCallFrame {
    UserCallFrame* previous=nullptr;
    std::int32_t return_pc=0, function_id=0, call_pc=0;
    std::size_t expression_base=0, for_base=0, return_base=0;
    std::size_t allocated_bytes=0;
    BasicNumber* numbers() { return reinterpret_cast<BasicNumber*>(this+1); }
    char* strings(const FunctionInfo& fn) {
        return reinterpret_cast<char*>(numbers()+fn.numeric_local_count);
    }
};
} // namespace rmb
