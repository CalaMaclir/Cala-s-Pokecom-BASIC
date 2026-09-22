#pragma once

#include <cstddef>
#include <cstdint>

#include "il.hpp"

namespace rmb {

enum class VmProfileMode : std::uint8_t {
    Off = 0,
    Counts,
    Timed
};

constexpr std::size_t kVmProfileOpcodeSlots = 128;
static_assert(
    static_cast<std::size_t>(OpCode::HALT) < kVmProfileOpcodeSlots,
    "Increase kVmProfileOpcodeSlots for new VM opcodes"
);

struct VmProfileReport {
    bool valid = false;
    bool timed = false;
    bool run_ok = false;
    std::uint64_t wall_us = 0;
    std::uint32_t dispatches = 0;
    std::uint32_t logical_ops = 0;
    std::uint16_t max_stack = 0;
    std::uint16_t max_for_depth = 0;
    std::uint32_t op_count[kVmProfileOpcodeSlots] = {};
    std::uint64_t op_time_us[kVmProfileOpcodeSlots] = {};
};

struct VmResult {
    bool ok = false;
    std::int32_t pc = 0;
    char message[96] = {};
};

class VM {
public:
    VmResult run(const CompiledProgram& program);
    VmResult run_direct(const CompiledProgram& program);
    void clear_direct_state();

    void set_profile_mode(VmProfileMode mode);
    VmProfileMode profile_mode() const;
    void reset_profile();
    const VmProfileReport& profile_report() const;
    void print_profile_report() const;

private:
    static constexpr std::size_t kRuntimeStringLength = 128;
    static constexpr std::size_t kArrayCells = 4096;
    static constexpr std::size_t kStringArrayCells = 512;

    struct DirectScalar {
        bool used = false;
        bool is_string = false;
        char name[kSymbolNameLength] = {};
        BasicNumber number = 0.0;
        char string[kRuntimeStringLength] = {};
    };

    struct ArrayMeta {
        bool defined = false;
        std::uint8_t dims = 0;
        std::int32_t n1 = 0;
        std::int32_t n2 = 0;
        std::size_t offset = 0;
    };

    BasicNumber numbers_[kMaxSymbols] = {};
    char strings_[kMaxSymbols][kRuntimeStringLength] = {};
    ArrayMeta arrays_[kMaxSymbols] = {};
    BasicNumber array_pool_[kArrayCells] = {};
    std::size_t array_used_ = 0;
    char string_array_pool_[kStringArrayCells][kRuntimeStringLength] = {};
    std::size_t string_array_used_ = 0;
    std::uint32_t random_state_ = 0x4d595df4u;
    DirectScalar direct_state_[kMaxSymbols] = {};

    VmProfileMode profile_mode_ = VmProfileMode::Off;
    VmProfileReport profile_ = {};

    VmResult run_impl(const CompiledProgram& program, bool direct_mode);
    void restore_direct_scalars(const CompiledProgram& program);
    void save_direct_scalars(const CompiledProgram& program);
};

} // namespace rmb
