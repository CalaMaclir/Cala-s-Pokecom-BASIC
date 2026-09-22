#include "vm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "line_editor.hpp"
#include "pico/stdlib.h"
#include "platform.hpp"
#include "storage.hpp"

namespace rmb {

namespace {

constexpr std::size_t kStackSize = 192;
constexpr std::size_t kReturnStackSize = 64;
constexpr std::size_t kForStackSize = 32;
constexpr std::size_t kScratchCount = 12;
constexpr std::size_t kScratchSize = 192;

struct Value {
    bool is_string = false;
    union {
        BasicNumber number;
        const char* string;
    };

    Value() : number(0.0) {}

    static Value num(BasicNumber value) {
        Value v;
        v.number = value;
        return v;
    }

    static Value str(const char* value) {
        Value v;
        v.is_string = true;
        v.string = value ? value : "";
        return v;
    }
};

// Firmware uses 32-bit pointers. Host regressions may use a 64-bit ABI.
static_assert(sizeof(void*) != 4 || sizeof(Value) == 8,
              "Fastfloat VM Value must stay compact on 32-bit targets");

struct ForFrame {
    int slot = -1;
    BasicNumber end = 0.0;
    BasicNumber step = 1.0;
    std::int32_t check_pc = 0;
    std::int32_t body_pc = 0;
};

std::int32_t source_line_for_pc(
    const CompiledProgram& program,
    std::int32_t pc
) {
    std::int32_t line = 0;

    for (std::size_t i = 0; i < program.line_count; ++i) {
        if (program.line_at(i).pc > pc) break;
        line = program.line_at(i).line;
    }

    return line;
}

VmResult make_error(
    const CompiledProgram& program,
    std::int32_t pc,
    const char* message
) {
    VmResult result;
    result.ok = false;
    result.pc = pc;

    const std::int32_t line = source_line_for_pc(program, pc);

    if (line > 0) {
        std::snprintf(
            result.message,
            sizeof(result.message),
            "%s IN %ld",
            message,
            static_cast<long>(line)
        );
    } else {
        std::snprintf(
            result.message,
            sizeof(result.message),
            "%s",
            message
        );
    }

    return result;
}

std::uint32_t palette(int n) {
    static const std::uint32_t colors[16] = {
        0x000000, 0x000080, 0x008000, 0x008080,
        0x800000, 0x800080, 0x808000, 0xc0c0c0,
        0x808080, 0x0000ff, 0x00ff00, 0x00ffff,
        0xff0000, 0xff00ff, 0xffff00, 0xffffff
    };

    if (n < 0) n = 0;
    if (n > 15) n = 15;
    return colors[n];
}

void format_number(BasicNumber value, char* out, std::size_t size) {
    const BasicNumber nearest = std::round(value);

    if (std::isfinite(value) &&
        std::fabs(value - nearest) < 1e-6f &&
        std::fabs(nearest) < 1.0e7f) {
        std::snprintf(out, size, "%.0f", nearest);
        return;
    }

    std::snprintf(out, size, "%.7g", value);
}

const char* opcode_name(OpCode code) {
#define RMB_OPCODE_NAME(name) case OpCode::name: return #name
    switch (code) {
        RMB_OPCODE_NAME(PUSH_NUM);
        RMB_OPCODE_NAME(PUSH_STR);
        RMB_OPCODE_NAME(LOAD);
        RMB_OPCODE_NAME(STORE);
        RMB_OPCODE_NAME(LOAD_NUM);
        RMB_OPCODE_NAME(LOAD_STR);
        RMB_OPCODE_NAME(STORE_NUM);
        RMB_OPCODE_NAME(STORE_STR);
        RMB_OPCODE_NAME(LOAD_ARR);
        RMB_OPCODE_NAME(STORE_ARR);
        RMB_OPCODE_NAME(DIM_ARR);
        RMB_OPCODE_NAME(LOAD_IND);
        RMB_OPCODE_NAME(STORE_IND);
        RMB_OPCODE_NAME(LOAD_ARR_IND);
        RMB_OPCODE_NAME(STORE_ARR_IND);
        RMB_OPCODE_NAME(ADD);
        RMB_OPCODE_NAME(SUB);
        RMB_OPCODE_NAME(MUL);
        RMB_OPCODE_NAME(DIV);
        RMB_OPCODE_NAME(POW);
        RMB_OPCODE_NAME(NEG);
        RMB_OPCODE_NAME(MOD);
        RMB_OPCODE_NAME(ADD_NUM);
        RMB_OPCODE_NAME(SUB_NUM);
        RMB_OPCODE_NAME(MUL_NUM);
        RMB_OPCODE_NAME(DIV_NUM);
        RMB_OPCODE_NAME(POW_NUM);
        RMB_OPCODE_NAME(NEG_NUM);
        RMB_OPCODE_NAME(MOD_NUM);
        RMB_OPCODE_NAME(CEQ);
        RMB_OPCODE_NAME(CNE);
        RMB_OPCODE_NAME(CLT);
        RMB_OPCODE_NAME(CLE);
        RMB_OPCODE_NAME(CGT);
        RMB_OPCODE_NAME(CGE);
        RMB_OPCODE_NAME(CEQ_NUM);
        RMB_OPCODE_NAME(CNE_NUM);
        RMB_OPCODE_NAME(CLT_NUM);
        RMB_OPCODE_NAME(CLE_NUM);
        RMB_OPCODE_NAME(CGT_NUM);
        RMB_OPCODE_NAME(CGE_NUM);
        RMB_OPCODE_NAME(CEQ_NUM_JZ);
        RMB_OPCODE_NAME(CNE_NUM_JZ);
        RMB_OPCODE_NAME(CLT_NUM_JZ);
        RMB_OPCODE_NAME(CLE_NUM_JZ);
        RMB_OPCODE_NAME(CGT_NUM_JZ);
        RMB_OPCODE_NAME(CGE_NUM_JZ);
        RMB_OPCODE_NAME(NOT);
        RMB_OPCODE_NAME(AND);
        RMB_OPCODE_NAME(OR);
        RMB_OPCODE_NAME(NOT_NUM);
        RMB_OPCODE_NAME(AND_NUM);
        RMB_OPCODE_NAME(OR_NUM);
        RMB_OPCODE_NAME(NOT_NUM_JZ);
        RMB_OPCODE_NAME(AND_NUM_JZ);
        RMB_OPCODE_NAME(OR_NUM_JZ);
        RMB_OPCODE_NAME(CALLFN);
        RMB_OPCODE_NAME(PRINT);
        RMB_OPCODE_NAME(PRINT_SPC);
        RMB_OPCODE_NAME(PRINT_SUPPRESS_NL);
        RMB_OPCODE_NAME(PRINT_NL);
        RMB_OPCODE_NAME(JMP);
        RMB_OPCODE_NAME(JZ);
        RMB_OPCODE_NAME(FOR_INIT);
        RMB_OPCODE_NAME(FOR_CHECK);
        RMB_OPCODE_NAME(FOR_INCR);
        RMB_OPCODE_NAME(GOSUB);
        RMB_OPCODE_NAME(RETSUB);
        RMB_OPCODE_NAME(ON_GOTO);
        RMB_OPCODE_NAME(ON_GOSUB);
        RMB_OPCODE_NAME(DUP);
        RMB_OPCODE_NAME(DROP);
        RMB_OPCODE_NAME(SWAP);
        RMB_OPCODE_NAME(OVER);
        RMB_OPCODE_NAME(ROT);
        RMB_OPCODE_NAME(PRINT_STACK);
        RMB_OPCODE_NAME(PRINT_CR);
        RMB_OPCODE_NAME(EMIT_CHAR);
        RMB_OPCODE_NAME(BAND);
        RMB_OPCODE_NAME(BOR);
        RMB_OPCODE_NAME(BXOR);
        RMB_OPCODE_NAME(MOV_NUM);
        RMB_OPCODE_NAME(ADD_VV_PUSH);
        RMB_OPCODE_NAME(SUB_VV_PUSH);
        RMB_OPCODE_NAME(MUL_VV_PUSH);
        RMB_OPCODE_NAME(MUL_CV_PUSH);
        RMB_OPCODE_NAME(ADD_VV_STORE);
        RMB_OPCODE_NAME(SUB_VV_STORE);
        RMB_OPCODE_NAME(MUL_VV_STORE);
        RMB_OPCODE_NAME(RGB_PSET_VV);
        RMB_OPCODE_NAME(MULADD_VVV_STORE);
        RMB_OPCODE_NAME(MULSUBADD_VVVVV_STORE);
        RMB_OPCODE_NAME(MUL_CVV_ADD_V_STORE);
        RMB_OPCODE_NAME(SQ2_GT_CONST_OR_JZ);
        RMB_OPCODE_NAME(SUMSQ_GT_CONST_JZ);
        RMB_OPCODE_NAME(COMPLEX_ITER_OR_JZ);
        RMB_OPCODE_NAME(COMPLEX_ITER_SUMSQ_JZ);
        RMB_OPCODE_NAME(GRAY_PSET_MUL_INT);
        RMB_OPCODE_NAME(GRAY_PSET_INT_STACK);
        RMB_OPCODE_NAME(RGB_PSET_MINMUL);
        RMB_OPCODE_NAME(FN0_NUM);
        RMB_OPCODE_NAME(FN1_NUM);
        RMB_OPCODE_NAME(FN2_NUM);
        RMB_OPCODE_NAME(FN3_NUM);
        RMB_OPCODE_NAME(STORE_CONST_NUM);
        RMB_OPCODE_NAME(ADD_VC_PUSH);
        RMB_OPCODE_NAME(SUB_VC_PUSH);
        RMB_OPCODE_NAME(DIV_VC_PUSH);
        RMB_OPCODE_NAME(HALT);
    }
#undef RMB_OPCODE_NAME
    return "?";
}

struct ProfileOpTimer {
    std::uint64_t* accumulator = nullptr;
    std::uint64_t start_us = 0;

    explicit ProfileOpTimer(std::uint64_t* target)
        : accumulator(target),
          start_us(target ? time_us_64() : 0) {}

    ~ProfileOpTimer() {
        if (accumulator) {
            *accumulator += time_us_64() - start_us;
        }
    }
};

struct ProfileRunTimer {
    VmProfileReport* report = nullptr;
    const std::uint32_t* logical_ops = nullptr;
    std::uint64_t start_us = 0;

    ProfileRunTimer(
        VmProfileReport* target,
        const std::uint32_t* logical
    ) : report(target),
        logical_ops(logical),
        start_us(target ? time_us_64() : 0) {}

    ~ProfileRunTimer() {
        if (!report) return;
        report->wall_us = time_us_64() - start_us;
        report->logical_ops = logical_ops ? *logical_ops : 0;
        report->valid = true;
    }
};

} // namespace

VmResult VM::run(const CompiledProgram& program) {
    return run_impl(program, false);
}

VmResult VM::run_direct(const CompiledProgram& program) {
    return run_impl(program, true);
}

void VM::clear_direct_state() {
    std::memset(direct_state_, 0, sizeof(direct_state_));
}

void VM::set_profile_mode(VmProfileMode mode) {
    profile_mode_ = mode;
}

VmProfileMode VM::profile_mode() const {
    return profile_mode_;
}

void VM::reset_profile() {
    profile_ = VmProfileReport{};
}

const VmProfileReport& VM::profile_report() const {
    return profile_;
}

void VM::print_profile_report() const {
    if (!profile_.valid) {
        platform::put_string("[PROFILE] NO DATA\r\n");
        return;
    }

    char line[128] = {};

    const char* mode = profile_.timed ? "TIME" : "COUNT";
    const unsigned long long wall_ms =
        static_cast<unsigned long long>(profile_.wall_us / 1000u);
    const unsigned long wall_us =
        static_cast<unsigned long>(profile_.wall_us % 1000u);

    std::snprintf(
        line,
        sizeof(line),
        "[PROFILE] %s  VM=%llu.%03lums  %s\r\n",
        mode,
        wall_ms,
        wall_us,
        profile_.run_ok ? "OK" : "ERROR/BREAK"
    );
    platform::put_string(line);

    const std::uint64_t fusion_x100 =
        profile_.dispatches > 0
            ? (static_cast<std::uint64_t>(profile_.logical_ops) * 100u) /
                  profile_.dispatches
            : 0;

    std::snprintf(
        line,
        sizeof(line),
        "DISPATCH=%lu  IL-EQUIV=%lu  FUSION=%llu.%02llux\r\n",
        static_cast<unsigned long>(profile_.dispatches),
        static_cast<unsigned long>(profile_.logical_ops),
        static_cast<unsigned long long>(fusion_x100 / 100u),
        static_cast<unsigned long long>(fusion_x100 % 100u)
    );
    platform::put_string(line);

    std::snprintf(
        line,
        sizeof(line),
        "MAX STACK=%u  MAX FOR=%u\r\n",
        static_cast<unsigned>(profile_.max_stack),
        static_cast<unsigned>(profile_.max_for_depth)
    );
    platform::put_string(line);

    constexpr int kTop = 20;
    int top[kTop] = {};
    for (int& item : top) item = -1;

    auto metric = [&](std::size_t index) -> std::uint64_t {
        return profile_.timed
            ? profile_.op_time_us[index]
            : static_cast<std::uint64_t>(profile_.op_count[index]);
    };

    for (std::size_t i = 0; i < kVmProfileOpcodeSlots; ++i) {
        if (profile_.op_count[i] == 0) continue;

        const std::uint64_t value = metric(i);
        int insert_at = -1;

        for (int j = 0; j < kTop; ++j) {
            if (top[j] < 0 ||
                value > metric(static_cast<std::size_t>(top[j]))) {
                insert_at = j;
                break;
            }
        }

        if (insert_at < 0) continue;

        for (int j = kTop - 1; j > insert_at; --j) {
            top[j] = top[j - 1];
        }
        top[insert_at] = static_cast<int>(i);
    }

    platform::put_string(
        profile_.timed
            ? "OPCODE                 COUNT    SHARE    TIME      SHARE\r\n"
            : "OPCODE                 COUNT    SHARE\r\n"
    );

    for (int rank = 0; rank < kTop; ++rank) {
        const int index = top[rank];
        if (index < 0) break;

        const std::uint32_t count =
            profile_.op_count[static_cast<std::size_t>(index)];
        const std::uint64_t count_pct10 =
            profile_.dispatches > 0
                ? (static_cast<std::uint64_t>(count) * 1000u) /
                      profile_.dispatches
                : 0;

        if (profile_.timed) {
            const std::uint64_t time =
                profile_.op_time_us[static_cast<std::size_t>(index)];
            const std::uint64_t time_pct10 =
                profile_.wall_us > 0
                    ? (time * 1000u) / profile_.wall_us
                    : 0;

            std::snprintf(
                line,
                sizeof(line),
                "%-21.21s %8lu %3llu.%1llu%% %6llu.%03llums %3llu.%1llu%%\r\n",
                opcode_name(static_cast<OpCode>(index)),
                static_cast<unsigned long>(count),
                static_cast<unsigned long long>(count_pct10 / 10u),
                static_cast<unsigned long long>(count_pct10 % 10u),
                static_cast<unsigned long long>(time / 1000u),
                static_cast<unsigned long long>(time % 1000u),
                static_cast<unsigned long long>(time_pct10 / 10u),
                static_cast<unsigned long long>(time_pct10 % 10u)
            );
        } else {
            std::snprintf(
                line,
                sizeof(line),
                "%-21.21s %8lu %3llu.%1llu%%\r\n",
                opcode_name(static_cast<OpCode>(index)),
                static_cast<unsigned long>(count),
                static_cast<unsigned long long>(count_pct10 / 10u),
                static_cast<unsigned long long>(count_pct10 % 10u)
            );
        }

        platform::put_string(line);
    }
}

void VM::restore_direct_scalars(const CompiledProgram& program) {
    for (std::size_t i = 0; i < program.symbol_count; ++i) {
        const Symbol& sym = program.symbols[i];

        for (std::size_t j = 0; j < kMaxSymbols; ++j) {
            const DirectScalar& saved = direct_state_[j];
            if (!saved.used) continue;
            if (saved.is_string != sym.is_string) continue;
            if (std::strcmp(saved.name, sym.name) != 0) continue;

            if (sym.is_string) {
                std::strncpy(strings_[i], saved.string, kRuntimeStringLength - 1);
                strings_[i][kRuntimeStringLength - 1] = '\0';
            } else {
                numbers_[i] = saved.number;
            }
            break;
        }
    }
}

void VM::save_direct_scalars(const CompiledProgram& program) {
    for (std::size_t i = 0; i < program.symbol_count; ++i) {
        const Symbol& sym = program.symbols[i];
        DirectScalar* exact = nullptr;
        DirectScalar* free_slot = nullptr;

        for (std::size_t j = 0; j < kMaxSymbols; ++j) {
            DirectScalar& saved = direct_state_[j];

            if (!saved.used && !free_slot) {
                free_slot = &saved;
            }

            if (saved.used &&
                saved.is_string == sym.is_string &&
                std::strcmp(saved.name, sym.name) == 0) {
                exact = &saved;
                break;
            }
        }

        DirectScalar* target = exact ? exact : free_slot;
        if (!target) continue;

        target->used = true;
        target->is_string = sym.is_string;
        std::strncpy(target->name, sym.name, kSymbolNameLength - 1);
        target->name[kSymbolNameLength - 1] = '\0';

        if (sym.is_string) {
            std::strncpy(target->string, strings_[i], kRuntimeStringLength - 1);
            target->string[kRuntimeStringLength - 1] = '\0';
        } else {
            target->number = numbers_[i];
        }
    }
}

VmResult VM::run_impl(
    const CompiledProgram& program,
    bool direct_mode
) {
    std::memset(numbers_, 0, sizeof(numbers_));
    std::memset(strings_, 0, sizeof(strings_));
    std::memset(arrays_, 0, sizeof(arrays_));
    std::memset(array_pool_, 0, sizeof(array_pool_));
    std::memset(string_array_pool_, 0, sizeof(string_array_pool_));
    array_used_ = 0;
    string_array_used_ = 0;

    if (direct_mode) {
        restore_direct_scalars(program);
    }

    Value stack[kStackSize] = {};
    std::size_t sp = 0;

    std::int32_t return_stack[kReturnStackSize] = {};
    std::size_t return_sp = 0;

    ForFrame for_stack[kForStackSize] = {};
    std::size_t for_sp = 0;

    char scratch[kScratchCount][kScratchSize] = {};
    std::size_t scratch_index = 0;

    auto scratch_buffer = [&]() -> char* {
        char* result = scratch[scratch_index++ % kScratchCount];
        result[0] = '\0';
        return result;
    };

    auto push = [&](const Value& value) -> bool {
        if (sp >= kStackSize) return false;
        stack[sp++] = value;
        return true;
    };

    auto pop = [&](Value& value) -> bool {
        if (sp == 0) return false;
        value = stack[--sp];
        return true;
    };

    auto value_text = [&](const Value& value, char* buffer, std::size_t size) -> const char* {
        if (value.is_string) return value.string ? value.string : "";
        format_number(value.number, buffer, size);
        return buffer;
    };

    auto truth = [&](bool value) -> Value {
        return Value::num(value ? -1.0 : 0.0);
    };

    auto next_random = [&]() -> std::uint32_t {
        std::uint32_t x = random_state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        random_state_ = x ? x : 0x4d595df4u;
        return random_state_;
    };

    auto random_unit = [&]() -> BasicNumber {
        return static_cast<BasicNumber>(next_random()) /
               static_cast<BasicNumber>(0xffffffffu);
    };

    auto push_string = [&](const char* text) -> bool {
        return push(Value::str(text));
    };

    auto require_number = [&](const Value& value, BasicNumber& number) -> bool {
        if (value.is_string) return false;
        number = value.number;
        return true;
    };

    int virtual_screen_width = 320;
    int virtual_screen_height = 320;
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 320;
    int viewport_height = 320;
    BasicNumber viewport_scale_x = 1.0;
    BasicNumber viewport_scale_y = 1.0;
    bool viewport_identity = true;

    auto configure_virtual_screen = [&](int w, int h) {
        if (w <= 0) w = 320;
        if (h <= 0) h = 320;

        virtual_screen_width = w;
        virtual_screen_height = h;

        const BasicNumber sx = 320.0 / static_cast<BasicNumber>(w);
        const BasicNumber sy = 320.0 / static_cast<BasicNumber>(h);
        const BasicNumber scale = sx < sy ? sx : sy;

        viewport_width =
            static_cast<int>(std::round(static_cast<BasicNumber>(w) * scale));
        viewport_height =
            static_cast<int>(std::round(static_cast<BasicNumber>(h) * scale));

        if (viewport_width < 1) viewport_width = 1;
        if (viewport_height < 1) viewport_height = 1;
        if (viewport_width > 320) viewport_width = 320;
        if (viewport_height > 320) viewport_height = 320;

        viewport_x = (320 - viewport_width) / 2;
        viewport_y = (320 - viewport_height) / 2;

        viewport_scale_x =
            (virtual_screen_width > 1 && viewport_width > 1)
                ? static_cast<BasicNumber>(viewport_width - 1) /
                      static_cast<BasicNumber>(virtual_screen_width - 1)
                : 0.0;
        viewport_scale_y =
            (virtual_screen_height > 1 && viewport_height > 1)
                ? static_cast<BasicNumber>(viewport_height - 1) /
                      static_cast<BasicNumber>(virtual_screen_height - 1)
                : 0.0;

        viewport_identity =
            viewport_x == 0 && viewport_y == 0 &&
            virtual_screen_width == 320 &&
            virtual_screen_height == 320 &&
            viewport_width == 320 &&
            viewport_height == 320;
    };

    auto map_graphics_x = [&](BasicNumber x) -> int {
        if (virtual_screen_width <= 1 || viewport_width <= 1)
            return viewport_x;
        if (viewport_identity)
            return static_cast<int>(std::round(x));

        return viewport_x +
               static_cast<int>(std::round(x * viewport_scale_x));
    };

    auto map_graphics_y = [&](BasicNumber y) -> int {
        if (virtual_screen_height <= 1 || viewport_height <= 1)
            return viewport_y;
        if (viewport_identity)
            return static_cast<int>(std::round(y));

        return viewport_y +
               static_cast<int>(std::round(y * viewport_scale_y));
    };

    auto map_graphics_radius = [&](BasicNumber radius) -> int {
        BasicNumber sx = 1.0;
        BasicNumber sy = 1.0;

        if (virtual_screen_width > 1 && viewport_width > 1) {
            sx = viewport_scale_x;
        }
        if (virtual_screen_height > 1 && viewport_height > 1) {
            sy = viewport_scale_y;
        }

        const BasicNumber scale = sx < sy ? sx : sy;
        int result = static_cast<int>(std::round(std::fabs(radius) * scale));
        if (result < 0) result = 0;
        return result;
    };

    std::int32_t pc = 0;
    std::uint32_t dispatch_count = 0;

    const bool profile_run =
        !direct_mode && profile_mode_ != VmProfileMode::Off;

    if (profile_run) {
        reset_profile();
        profile_.timed = profile_mode_ == VmProfileMode::Timed;
    }

    ProfileRunTimer profile_run_timer(
        profile_run ? &profile_ : nullptr,
        &dispatch_count
    );

    while (pc >= 0 && static_cast<std::size_t>(pc) < program.code_count) {
        if ((dispatch_count++ & 0xfffu) == 0u &&
            platform::break_requested()) {
            return make_error(program, pc, "BREAK");
        }

        const std::int32_t op_pc = pc;
        const Op& op = program.code[pc++];

        const std::size_t profile_index =
            static_cast<std::size_t>(op.code);
        std::uint64_t* profile_time = nullptr;

        if (profile_run && profile_index < kVmProfileOpcodeSlots) {
            ++profile_.dispatches;
            ++profile_.op_count[profile_index];

            if (profile_.timed) {
                profile_time = &profile_.op_time_us[profile_index];
            }
        }

        ProfileOpTimer profile_op_timer(profile_time);

        switch (op.code) {
            case OpCode::PUSH_NUM: {
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.number_count) {
                    return make_error(program, op_pc, "BAD NUMBER REFERENCE");
                }
                const BasicNumber constant = program.number_pool[op.a];

                // Runtime fallback superinstruction for patterns not folded by
                // the compiler (for example when a protected jump target lies
                // inside the sequence).
                if (static_cast<std::size_t>(pc + 1) < program.code_count &&
                    program.code[pc].code == OpCode::LOAD &&
                    program.code[pc + 1].code == OpCode::MUL) {
                    const int slot = program.code[pc].a;
                    if (slot >= 0 &&
                        static_cast<std::size_t>(slot) < program.symbol_count &&
                        !program.symbols[slot].is_string) {
                        if (!push(Value::num(constant * numbers_[slot])))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        dispatch_count += 2;
                        pc += 2;
                        break;
                    }
                }

                if (!push(Value::num(constant)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::PUSH_STR:
                if (op.s >= program.string_used)
                    return make_error(program, op_pc, "BAD STRING REFERENCE");
                if (!push_string(program.string_pool + op.s))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;

            case OpCode::STORE_CONST_NUM: {
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count ||
                    op.b < 0 ||
                    static_cast<std::size_t>(op.b) >= program.number_count) {
                    return make_error(program, op_pc, "BAD CONSTANT STORE");
                }

                numbers_[op.a] = program.number_pool[op.b];
                ++dispatch_count;
                ++pc;
                break;
            }

            case OpCode::ADD_VC_PUSH:
            case OpCode::SUB_VC_PUSH:
            case OpCode::DIV_VC_PUSH: {
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count ||
                    op.b < 0 ||
                    static_cast<std::size_t>(op.b) >= program.number_count) {
                    return make_error(program, op_pc, "BAD FUSED OPCODE");
                }

                const BasicNumber lhs = numbers_[op.a];
                const BasicNumber rhs = program.number_pool[op.b];
                BasicNumber result = 0.0;

                if (op.code == OpCode::ADD_VC_PUSH) {
                    result = lhs + rhs;
                } else if (op.code == OpCode::SUB_VC_PUSH) {
                    result = lhs - rhs;
                } else {
                    if (rhs == 0.0)
                        return make_error(
                            program,
                            op_pc,
                            "DIVISION BY ZERO"
                        );
                    result = lhs / rhs;
                }

                dispatch_count += 2;
                pc += 2;

                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::STORE_NUM) {
                    const int dst = program.code[pc].a;
                    if (dst >= 0 &&
                        static_cast<std::size_t>(dst) <
                            program.symbol_count) {
                        numbers_[dst] = result;
                        ++dispatch_count;
                        ++pc;
                        break;
                    }
                }

                if (!push(Value::num(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::MOV_NUM: {
                if (static_cast<std::size_t>(op_pc + 2) > program.code_count)
                    return make_error(program, op_pc, "BAD FUSED OPCODE");

                const int dst = op.a;
                const int src = op.b;
                if (dst < 0 || src < 0 ||
                    static_cast<std::size_t>(dst) >= program.symbol_count ||
                    static_cast<std::size_t>(src) >= program.symbol_count ||
                    program.symbols[dst].is_string ||
                    program.symbols[src].is_string) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }

                numbers_[dst] = numbers_[src];
                ++dispatch_count;
                ++pc; // skip the covered STORE
                break;
            }

            case OpCode::ADD_VV_PUSH:
            case OpCode::SUB_VV_PUSH:
            case OpCode::MUL_VV_PUSH: {
                const BasicNumber lhs = numbers_[op.a];
                const BasicNumber rhs = numbers_[op.b];
                BasicNumber result = 0.0;

                if (op.code == OpCode::ADD_VV_PUSH) result = lhs + rhs;
                else if (op.code == OpCode::SUB_VV_PUSH) result = lhs - rhs;
                else result = lhs * rhs;

                if (!push(Value::num(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");

                dispatch_count += 2;

                pc += 2; // skip LOAD rhs and arithmetic opcode
                break;
            }

            case OpCode::MUL_CV_PUSH: {
                const BasicNumber result =
                    program.number_pool[op.a] * numbers_[op.b];

                if (!push(Value::num(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");

                dispatch_count += 2;

                pc += 2; // skip the second operand and MUL
                break;
            }

            case OpCode::RGB_PSET_VV: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int rs = static_cast<int>(packed & 0x3fu);
                const int gs = static_cast<int>((packed >> 6) & 0x3fu);
                const int bs = static_cast<int>((packed >> 12) & 0x3fu);
                const int xs = static_cast<int>((packed >> 18) & 0x3fu);
                const int ys = static_cast<int>((packed >> 24) & 0x3fu);

                int red = static_cast<int>(numbers_[rs]);
                int green = static_cast<int>(numbers_[gs]);
                int blue = static_cast<int>(numbers_[bs]);
                if (red < 0) red = 0; else if (red > 255) red = 255;
                if (green < 0) green = 0; else if (green > 255) green = 255;
                if (blue < 0) blue = 0; else if (blue > 255) blue = 255;

                platform::set_graphics_color(
                    (static_cast<std::uint32_t>(red) << 16) |
                    (static_cast<std::uint32_t>(green) << 8) |
                    static_cast<std::uint32_t>(blue)
                );
                platform::graphics_pixel(
                    map_graphics_x(numbers_[xs]),
                    map_graphics_y(numbers_[ys])
                );

                dispatch_count += 6;

                pc += 6; // skip G,B,COLOR,X,Y,PSET tail operations
                break;
            }

            case OpCode::MULADD_VVV_STORE: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int dst = static_cast<int>(packed & 0x3fu);
                const int addend = static_cast<int>((packed >> 6) & 0x3fu);
                const int lhs = static_cast<int>((packed >> 12) & 0x3fu);
                const int rhs = static_cast<int>((packed >> 18) & 0x3fu);

                const BasicNumber product = numbers_[lhs] * numbers_[rhs];
                numbers_[dst] =
                    (op.flags & 1u) != 0
                        ? product + numbers_[addend]
                        : numbers_[addend] + product;

                dispatch_count += 5;
                pc += 5;
                break;
            }

            case OpCode::MULSUBADD_VVVVV_STORE: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int dst = static_cast<int>(packed & 0x3fu);
                const int a = static_cast<int>((packed >> 6) & 0x3fu);
                const int b = static_cast<int>((packed >> 12) & 0x3fu);
                const int c = static_cast<int>((packed >> 18) & 0x3fu);
                const int d = static_cast<int>((packed >> 24) & 0x3fu);
                const int e = op.b & 0x3f;

                const BasicNumber ab = numbers_[a] * numbers_[b];
                const BasicNumber cd = numbers_[c] * numbers_[d];
                numbers_[dst] = (ab - cd) + numbers_[e];

                dispatch_count += 9;
                pc += 9;
                break;
            }

            case OpCode::MUL_CVV_ADD_V_STORE: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int dst = static_cast<int>(packed & 0x3fu);
                const int a = static_cast<int>((packed >> 6) & 0x3fu);
                const int b = static_cast<int>((packed >> 12) & 0x3fu);
                const int c = static_cast<int>((packed >> 18) & 0x3fu);

                const BasicNumber left =
                    program.number_pool[op.b] * numbers_[a];
                numbers_[dst] = (left * numbers_[b]) + numbers_[c];

                dispatch_count += 7;
                pc += 7;
                break;
            }

            case OpCode::SQ2_GT_CONST_OR_JZ: {
                const int x = op.s & 0x3f;
                const int y = (op.s >> 6) & 0x3f;
                const BasicNumber constant = program.number_pool[op.b];

                const BasicNumber xx = numbers_[x] * numbers_[x];
                const BasicNumber yy = numbers_[y] * numbers_[y];
                const bool left = xx > constant;
                const bool right = yy > constant;
                const bool result = left || right;

                dispatch_count += 11;
                if (!result) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                } else {
                    pc += 11;
                }
                break;
            }

            case OpCode::SUMSQ_GT_CONST_JZ: {
                const int x = op.s & 0x3f;
                const int y = (op.s >> 6) & 0x3f;
                const BasicNumber constant = program.number_pool[op.b];

                const BasicNumber xx = numbers_[x] * numbers_[x];
                const BasicNumber yy = numbers_[y] * numbers_[y];
                const bool result = (xx + yy) > constant;

                dispatch_count += 9;
                if (!result) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                } else {
                    pc += 9;
                }
                break;
            }

            case OpCode::GRAY_PSET_MUL_INT: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int gray = static_cast<int>(packed & 0x3fu);
                const int lhs = static_cast<int>((packed >> 6) & 0x3fu);
                const int rhs = static_cast<int>((packed >> 12) & 0x3fu);
                const int xs = static_cast<int>((packed >> 18) & 0x3fu);
                const int ys = static_cast<int>((packed >> 24) & 0x3fu);

                const BasicNumber value =
                    std::floor(numbers_[lhs] * numbers_[rhs]);
                numbers_[gray] = value;

                int g = static_cast<int>(value);
                if (g < 0) g = 0;
                else if (g > 255) g = 255;

                const std::uint32_t color =
                    (static_cast<std::uint32_t>(g) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) |
                    static_cast<std::uint32_t>(g);

                platform::set_graphics_color(color);
                platform::graphics_pixel(
                    map_graphics_x(numbers_[xs]),
                    map_graphics_y(numbers_[ys])
                );

                dispatch_count += 11;
                pc += 11;
                break;
            }

            case OpCode::GRAY_PSET_INT_STACK: {
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (stack[sp - 1].is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int gray = static_cast<int>(packed & 0x3fu);
                const int xs = static_cast<int>((packed >> 6) & 0x3fu);
                const int ys = static_cast<int>((packed >> 12) & 0x3fu);

                const BasicNumber value =
                    std::floor(stack[sp - 1].number);
                --sp;
                numbers_[gray] = value;

                int g = static_cast<int>(value);
                if (g < 0) g = 0;
                else if (g > 255) g = 255;

                const std::uint32_t color =
                    (static_cast<std::uint32_t>(g) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) |
                    static_cast<std::uint32_t>(g);

                platform::set_graphics_color(color);
                platform::graphics_pixel(
                    map_graphics_x(numbers_[xs]),
                    map_graphics_y(numbers_[ys])
                );

                // Covered tail: STORE G, LOAD G*3, COLOR, LOAD X/Y, PSET.
                dispatch_count += 8;
                pc += 8;
                break;
            }

            case OpCode::RGB_PSET_MINMUL: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int src = static_cast<int>(packed & 0x3fu);
                const int rs = static_cast<int>((packed >> 6) & 0x3fu);
                const int gs = static_cast<int>((packed >> 12) & 0x3fu);
                const int bs = static_cast<int>((packed >> 18) & 0x3fu);
                const int xs = static_cast<int>((packed >> 24) & 0x3fu);
                const int ys = op.s & 0x3f;

                const std::uint32_t params =
                    static_cast<std::uint32_t>(op.b);
                const int kr = static_cast<int>(params & 0xffu);
                const int kg = static_cast<int>((params >> 8) & 0xffu);
                const int kb = static_cast<int>((params >> 16) & 0xffu);
                const int cap = static_cast<int>((params >> 24) & 0xffu);

                const BasicNumber source = numbers_[src];
                BasicNumber rv = source * static_cast<BasicNumber>(kr);
                BasicNumber gv = source * static_cast<BasicNumber>(kg);
                BasicNumber bv = source * static_cast<BasicNumber>(kb);

                if (rv > static_cast<BasicNumber>(cap)) rv = cap;
                if (gv > static_cast<BasicNumber>(cap)) gv = cap;
                if (bv > static_cast<BasicNumber>(cap)) bv = cap;

                numbers_[rs] = rv;
                numbers_[gs] = gv;
                numbers_[bs] = bv;

                int red = static_cast<int>(rv);
                int green = static_cast<int>(gv);
                int blue = static_cast<int>(bv);
                if (red < 0) red = 0; else if (red > 255) red = 255;
                if (green < 0) green = 0; else if (green > 255) green = 255;
                if (blue < 0) blue = 0; else if (blue > 255) blue = 255;

                platform::set_graphics_color(
                    (static_cast<std::uint32_t>(red) << 16) |
                    (static_cast<std::uint32_t>(green) << 8) |
                    static_cast<std::uint32_t>(blue)
                );
                platform::graphics_pixel(
                    map_graphics_x(numbers_[xs]),
                    map_graphics_y(numbers_[ys])
                );

                dispatch_count += 24;
                pc += 24;
                break;
            }

            case OpCode::COMPLEX_ITER_OR_JZ:
            case OpCode::COMPLEX_ITER_SUMSQ_JZ: {
                const std::uint32_t packed =
                    static_cast<std::uint32_t>(op.a);
                const int x = static_cast<int>(packed & 0x3fu);
                const int y = static_cast<int>((packed >> 6) & 0x3fu);
                const int real_add =
                    static_cast<int>((packed >> 12) & 0x3fu);
                const int imag_add =
                    static_cast<int>((packed >> 18) & 0x3fu);
                const int temp =
                    static_cast<int>((packed >> 24) & 0x3fu);

                const int false_target = op.b & 0x0fff;
                const int threshold_index = (op.b >> 12) & 0x0fff;
                const bool has_extra_move = (op.flags & 1u) != 0;
                const bool fuse_next = (op.flags & 2u) != 0;

                // Preserve the exact Stage-3 floating-point evaluation order:
                //   T = (X*X - Y*Y) + CR
                //   Y = ((2*X)*Y) + CI
                // The Y expression must use the old X/Y values.
                const BasicNumber old_x = numbers_[x];
                const BasicNumber old_y = numbers_[y];

                const BasicNumber xx0 = old_x * old_x;
                const BasicNumber yy0 = old_y * old_y;
                const BasicNumber new_x =
                    (xx0 - yy0) + numbers_[real_add];

                const BasicNumber twice_x = 2.0 * old_x;
                const BasicNumber new_y =
                    (twice_x * old_y) + numbers_[imag_add];

                numbers_[temp] = new_x;
                numbers_[y] = new_y;
                numbers_[x] = new_x;

                if (has_extra_move) {
                    const int move_dst = op.s & 0x3f;
                    const int move_src = (op.s >> 6) & 0x3f;
                    numbers_[move_dst] = numbers_[move_src];
                }

                const BasicNumber xx = new_x * new_x;
                const BasicNumber yy = new_y * new_y;
                const BasicNumber threshold =
                    program.number_pool[threshold_index];

                const bool escape =
                    op.code == OpCode::COMPLEX_ITER_OR_JZ
                        ? (xx > threshold || yy > threshold)
                        : ((xx + yy) > threshold);

                const std::int32_t condition_slots =
                    op.code == OpCode::COMPLEX_ITER_OR_JZ ? 12 : 10;
                const std::int32_t total_slots =
                    20 + (has_extra_move ? 2 : 0) + condition_slots;

                dispatch_count +=
                    static_cast<std::uint32_t>(total_slots - 1);

                if (!escape) {
                    if (false_target < 0 ||
                        static_cast<std::size_t>(false_target) >=
                            program.code_count) {
                        return make_error(
                            program,
                            op_pc,
                            "BAD JUMP TARGET"
                        );
                    }
                    if (fuse_next &&
                        program.code[false_target].code ==
                            OpCode::FOR_INCR) {
                        const Op& next_op = program.code[false_target];

                        if (for_sp == 0) {
                            return make_error(
                                program,
                                false_target,
                                "NEXT WITHOUT FOR"
                            );
                        }

                        if (next_op.a >= 0) {
                            while (for_sp > 0 &&
                                   for_stack[for_sp - 1].slot !=
                                       next_op.a) {
                                --for_sp;
                            }

                            if (for_sp == 0) {
                                return make_error(
                                    program,
                                    false_target,
                                    "NEXT WITHOUT FOR"
                                );
                            }
                        }

                        ForFrame& frame = for_stack[for_sp - 1];
                        numbers_[frame.slot] += frame.step;

                        const BasicNumber current = numbers_[frame.slot];
                        const bool cont =
                            frame.step >= 0.0
                                ? current <= frame.end
                                : current >= frame.end;

                        ++dispatch_count; // account for covered FOR_INCR

                        if (cont) {
                            pc = frame.body_pc;
                        } else {
                            --for_sp;
                            pc = false_target + 1;
                        }
                    } else {
                        pc = false_target;
                    }
                } else {
                    // Land on the statement immediately after the original
                    // condition/JZ sequence. For IF ... THEN GOTO this is the
                    // existing JMP, which remains untouched.
                    pc += total_slots - 1;
                }
                break;
            }

            case OpCode::ADD_VV_STORE:
            case OpCode::SUB_VV_STORE:
            case OpCode::MUL_VV_STORE: {
                if (static_cast<std::size_t>(op_pc + 4) > program.code_count)
                    return make_error(program, op_pc, "BAD FUSED OPCODE");

                const int dst = op.a;
                const int lhs = op.b & 0xff;
                const int rhs = (op.b >> 8) & 0xff;

                if (dst < 0 ||
                    static_cast<std::size_t>(dst) >= program.symbol_count ||
                    static_cast<std::size_t>(lhs) >= program.symbol_count ||
                    static_cast<std::size_t>(rhs) >= program.symbol_count ||
                    program.symbols[dst].is_string ||
                    program.symbols[lhs].is_string ||
                    program.symbols[rhs].is_string) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }

                if (op.code == OpCode::ADD_VV_STORE) {
                    numbers_[dst] = numbers_[lhs] + numbers_[rhs];
                } else if (op.code == OpCode::SUB_VV_STORE) {
                    numbers_[dst] = numbers_[lhs] - numbers_[rhs];
                } else {
                    numbers_[dst] = numbers_[lhs] * numbers_[rhs];
                }

                dispatch_count += 3;

                pc += 3; // skip LOAD rhs, arithmetic opcode, STORE
                break;
            }

            case OpCode::LOAD_NUM:
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count ||
                    sp >= kStackSize) {
                    return make_error(
                        program, op_pc,
                        sp >= kStackSize ? "STACK OVERFLOW" : "BAD VARIABLE SLOT"
                    );
                }
                stack[sp].is_string = false;
                stack[sp].number = numbers_[op.a];
                ++sp;
                break;

            case OpCode::LOAD_STR:
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count ||
                    sp >= kStackSize) {
                    return make_error(
                        program, op_pc,
                        sp >= kStackSize ? "STACK OVERFLOW" : "BAD VARIABLE SLOT"
                    );
                }
                stack[sp].is_string = true;
                stack[sp].string = strings_[op.a];
                ++sp;
                break;

            case OpCode::STORE_NUM:
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }
                numbers_[op.a] = stack[--sp].number;
                break;

            case OpCode::STORE_STR:
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }
                --sp;
                std::snprintf(
                    strings_[op.a],
                    sizeof(strings_[op.a]),
                    "%s",
                    stack[sp].string ? stack[sp].string : ""
                );
                break;

            case OpCode::LOAD:
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }

                if (!program.symbols[op.a].is_string) {
                    // Graphics hot-path superinstruction:
                    //   LOAD R, LOAD G, LOAD B, COLOR,
                    //   LOAD X, LOAD Y, PSET
                    // becomes one VM dispatch. This pattern dominates the
                    // inner pixel loop of Mandelbrot/Julia programs.
                    if (static_cast<std::size_t>(pc + 5) <
                            program.code_count &&
                        program.code[pc].code == OpCode::LOAD &&
                        program.code[pc + 1].code == OpCode::LOAD &&
                        program.code[pc + 2].code == OpCode::CALLFN &&
                        program.code[pc + 2].a == FnId::GCOLOR &&
                        program.code[pc + 2].b == 3 &&
                        program.code[pc + 3].code == OpCode::LOAD &&
                        program.code[pc + 4].code == OpCode::LOAD &&
                        program.code[pc + 5].code == OpCode::CALLFN &&
                        program.code[pc + 5].a == FnId::GPSET &&
                        program.code[pc + 5].b == 2) {
                        const int gs = program.code[pc].a;
                        const int bs = program.code[pc + 1].a;
                        const int xs = program.code[pc + 3].a;
                        const int ys = program.code[pc + 4].a;

                        const bool slots_ok =
                            gs >= 0 && bs >= 0 && xs >= 0 && ys >= 0 &&
                            static_cast<std::size_t>(gs) <
                                program.symbol_count &&
                            static_cast<std::size_t>(bs) <
                                program.symbol_count &&
                            static_cast<std::size_t>(xs) <
                                program.symbol_count &&
                            static_cast<std::size_t>(ys) <
                                program.symbol_count &&
                            !program.symbols[gs].is_string &&
                            !program.symbols[bs].is_string &&
                            !program.symbols[xs].is_string &&
                            !program.symbols[ys].is_string;

                        if (slots_ok) {
                            int r = static_cast<int>(numbers_[op.a]);
                            int g = static_cast<int>(numbers_[gs]);
                            int b = static_cast<int>(numbers_[bs]);
                            if (r < 0) r = 0; else if (r > 255) r = 255;
                            if (g < 0) g = 0; else if (g > 255) g = 255;
                            if (b < 0) b = 0; else if (b > 255) b = 255;

                            platform::set_graphics_color(
                                (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g) << 8) |
                                static_cast<std::uint32_t>(b)
                            );
                            platform::graphics_pixel(
                                map_graphics_x(numbers_[xs]),
                                map_graphics_y(numbers_[ys])
                            );

                            dispatch_count += 6;

                            pc += 6;
                            break;
                        }
                    }

                    // Runtime superinstructions for the most common numeric
                    // expression patterns. Three IL operations become one VM
                    // dispatch while bytecode PCs remain unchanged.
                    if (static_cast<std::size_t>(pc + 1) < program.code_count &&
                        program.code[pc].code == OpCode::LOAD) {
                        const int rhs_slot = program.code[pc].a;
                        const OpCode fused_op = program.code[pc + 1].code;

                        if (rhs_slot >= 0 &&
                            static_cast<std::size_t>(rhs_slot) <
                                program.symbol_count &&
                            !program.symbols[rhs_slot].is_string &&
                            (fused_op == OpCode::MUL ||
                             fused_op == OpCode::ADD ||
                             fused_op == OpCode::SUB)) {
                            const BasicNumber lhs = numbers_[op.a];
                            const BasicNumber rhs = numbers_[rhs_slot];
                            BasicNumber result = 0.0;

                            if (fused_op == OpCode::MUL) result = lhs * rhs;
                            else if (fused_op == OpCode::ADD) result = lhs + rhs;
                            else result = lhs - rhs;

                            if (!push(Value::num(result)))
                                return make_error(
                                    program, op_pc, "STACK OVERFLOW"
                                );

                            dispatch_count += 2;

                            pc += 2;
                            break;
                        }
                    }

                    // LOAD src, STORE dst is a common assignment/copy pattern.
                    if (static_cast<std::size_t>(pc) < program.code_count &&
                        program.code[pc].code == OpCode::STORE) {
                        const int dst = program.code[pc].a;
                        if (dst >= 0 &&
                            static_cast<std::size_t>(dst) <
                                program.symbol_count &&
                            !program.symbols[dst].is_string) {
                            numbers_[dst] = numbers_[op.a];
                            ++dispatch_count;
                            ++pc;
                            break;
                        }
                    }

                    if (!push(Value::num(numbers_[op.a])))
                        return make_error(program, op_pc, "STACK OVERFLOW");
                } else {
                    if (!push_string(strings_[op.a]))
                        return make_error(program, op_pc, "STACK OVERFLOW");
                }
                break;

            case OpCode::STORE: {
                Value value;
                if (!pop(value))
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD VARIABLE SLOT");
                }

                if (program.symbols[op.a].is_string) {
                    if (!value.is_string)
                        return make_error(program, op_pc, "TYPE MISMATCH");

                    std::snprintf(
                        strings_[op.a],
                        sizeof(strings_[op.a]),
                        "%s",
                        value.string ? value.string : ""
                    );
                } else {
                    if (value.is_string)
                        return make_error(program, op_pc, "TYPE MISMATCH");

                    numbers_[op.a] = value.number;
                }
                break;
            }

            case OpCode::DIM_ARR: {
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD ARRAY");
                }

                int n1 = 0;
                int n2 = 0;

                if (op.b == 2) {
                    Value v2;
                    if (!pop(v2) || v2.is_string)
                        return make_error(program, op_pc, "BAD DIMENSION");
                    n2 = static_cast<int>(v2.number);
                }

                Value v1;
                if (!pop(v1) || v1.is_string)
                    return make_error(program, op_pc, "BAD DIMENSION");
                n1 = static_cast<int>(v1.number);

                if (n1 < 0 || n2 < 0 || op.b < 1 || op.b > 2)
                    return make_error(program, op_pc, "BAD DIMENSION");

                // RetroMiniBASIC compatibility: DIM A(10) has indices 0..10.
                const std::size_t d1 = static_cast<std::size_t>(n1) + 1;
                const std::size_t d2 =
                    op.b == 2 ? static_cast<std::size_t>(n2) + 1 : 1;
                const std::size_t cells = d1 * d2;
                const bool string_array = program.symbols[op.a].is_string;

                ArrayMeta& meta = arrays_[op.a];
                meta.defined = true;
                meta.dims = static_cast<std::uint8_t>(op.b);
                meta.n1 = static_cast<std::int32_t>(d1);
                meta.n2 = static_cast<std::int32_t>(d2);

                if (string_array) {
                    if (cells == 0 ||
                        string_array_used_ + cells > kStringArrayCells) {
                        return make_error(
                            program,
                            op_pc,
                            "STRING ARRAY MEMORY FULL"
                        );
                    }

                    meta.offset = string_array_used_;
                    std::memset(
                        &string_array_pool_[string_array_used_][0],
                        0,
                        cells * kRuntimeStringLength
                    );
                    string_array_used_ += cells;
                } else {
                    if (cells == 0 || array_used_ + cells > kArrayCells)
                        return make_error(
                            program,
                            op_pc,
                            "ARRAY MEMORY FULL"
                        );

                    meta.offset = array_used_;
                    std::memset(
                        array_pool_ + array_used_,
                        0,
                        cells * sizeof(BasicNumber)
                    );
                    array_used_ += cells;
                }
                break;
            }

            case OpCode::LOAD_ARR:
            case OpCode::STORE_ARR: {
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.symbol_count) {
                    return make_error(program, op_pc, "BAD ARRAY");
                }

                ArrayMeta& meta = arrays_[op.a];
                if (!meta.defined || meta.dims != op.b)
                    return make_error(program, op_pc, "UNDEF'D ARRAY");

                const bool string_array = program.symbols[op.a].is_string;

                Value value;
                if (op.code == OpCode::STORE_ARR) {
                    if (!pop(value))
                        return make_error(program, op_pc, "STACK UNDERFLOW");
                    if (value.is_string != string_array)
                        return make_error(program, op_pc, "TYPE MISMATCH");
                }

                int i = 0;
                int j = 0;

                if (op.b == 2) {
                    Value index2;
                    if (!pop(index2) || index2.is_string)
                        return make_error(program, op_pc, "BAD SUBSCRIPT");
                    j = static_cast<int>(index2.number);
                }

                Value index1;
                if (!pop(index1) || index1.is_string)
                    return make_error(program, op_pc, "BAD SUBSCRIPT");
                i = static_cast<int>(index1.number);

                if (i < 0 || i >= meta.n1 ||
                    j < 0 || j >= meta.n2) {
                    return make_error(
                        program,
                        op_pc,
                        "SUBSCRIPT OUT OF RANGE"
                    );
                }

                const std::size_t relative =
                    static_cast<std::size_t>(i) *
                        static_cast<std::size_t>(meta.n2) +
                    static_cast<std::size_t>(j);
                const std::size_t index = meta.offset + relative;

                if (string_array) {
                    if (index >= kStringArrayCells)
                        return make_error(program, op_pc, "BAD ARRAY");

                    if (op.code == OpCode::LOAD_ARR) {
                        if (!push_string(string_array_pool_[index]))
                            return make_error(
                                program,
                                op_pc,
                                "STACK OVERFLOW"
                            );
                    } else {
                        std::snprintf(
                            string_array_pool_[index],
                            kRuntimeStringLength,
                            "%s",
                            value.string ? value.string : ""
                        );
                    }
                } else {
                    if (index >= kArrayCells)
                        return make_error(program, op_pc, "BAD ARRAY");

                    if (op.code == OpCode::LOAD_ARR) {
                        if (!push(Value::num(array_pool_[index])))
                            return make_error(
                                program,
                                op_pc,
                                "STACK OVERFLOW"
                            );
                    } else {
                        array_pool_[index] = value.number;
                    }
                }
                break;
            }

            case OpCode::ADD_NUM:
            case OpCode::SUB_NUM:
            case OpCode::MUL_NUM:
            case OpCode::DIV_NUM:
            case OpCode::MOD_NUM:
            case OpCode::POW_NUM: {
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                const BasicNumber rhs = stack[sp - 1].number;
                const BasicNumber lhs = stack[sp - 2].number;
                BasicNumber result = 0.0;

                if (op.code == OpCode::ADD_NUM) {
                    result = lhs + rhs;
                } else if (op.code == OpCode::SUB_NUM) {
                    result = lhs - rhs;
                } else if (op.code == OpCode::MUL_NUM) {
                    result = lhs * rhs;
                } else if (op.code == OpCode::POW_NUM) {
                    result = std::pow(lhs, rhs);
                } else if (op.code == OpCode::MOD_NUM) {
                    if (rhs == 0.0)
                        return make_error(program, op_pc, "DIVISION BY ZERO");
                    result = std::fmod(lhs, rhs);
                } else {
                    if (rhs == 0.0)
                        return make_error(program, op_pc, "DIVISION BY ZERO");
                    result = lhs / rhs;
                }

                // Assignment is the overwhelmingly common terminal use of a
                // numeric expression. Consume STORE_NUM here without rebuilding
                // a temporary stack value.
                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::STORE_NUM) {
                    const int dst = program.code[pc].a;
                    if (dst >= 0 &&
                        static_cast<std::size_t>(dst) < program.symbol_count) {
                        numbers_[dst] = result;
                        sp -= 2;
                        ++dispatch_count;
                        ++pc;
                        break;
                    }
                }

                stack[sp - 2].is_string = false;
                stack[sp - 2].number = result;
                --sp;
                break;
            }

            case OpCode::NEG_NUM:
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                stack[sp - 1].number = -stack[sp - 1].number;
                break;

            case OpCode::ADD: {
                Value b;
                Value a;
                if (!pop(b) || !pop(a))
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                if (a.is_string || b.is_string) {
                    char an[48] = {};
                    char bn[48] = {};
                    const char* as = value_text(a, an, sizeof(an));
                    const char* bs = value_text(b, bn, sizeof(bn));
                    char* dst = scratch_buffer();

                    std::snprintf(dst, kScratchSize, "%s%s", as, bs);

                    if (!push_string(dst))
                        return make_error(program, op_pc, "STACK OVERFLOW");
                } else {
                    const BasicNumber result = a.number + b.number;

                    if (static_cast<std::size_t>(pc) < program.code_count &&
                        program.code[pc].code == OpCode::STORE) {
                        const int dst = program.code[pc].a;
                        if (dst >= 0 &&
                            static_cast<std::size_t>(dst) <
                                program.symbol_count &&
                            !program.symbols[dst].is_string) {
                            numbers_[dst] = result;
                            ++dispatch_count;
                            ++pc;
                            break;
                        }
                    }

                    if (!push(Value::num(result)))
                        return make_error(program, op_pc, "STACK OVERFLOW");
                }
                break;
            }

            case OpCode::SUB:
            case OpCode::MUL:
            case OpCode::DIV:
            case OpCode::MOD:
            case OpCode::POW: {
                Value b;
                Value a;

                if (!pop(b) || !pop(a))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (a.is_string || b.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                BasicNumber result = 0.0;

                if (op.code == OpCode::SUB) {
                    result = a.number - b.number;
                } else if (op.code == OpCode::MUL) {
                    result = a.number * b.number;
                } else if (op.code == OpCode::POW) {
                    result = std::pow(a.number, b.number);
                } else if (op.code == OpCode::MOD) {
                    if (b.number == 0.0)
                        return make_error(program, op_pc, "DIVISION BY ZERO");
                    result = std::fmod(a.number, b.number);
                } else {
                    if (b.number == 0.0)
                        return make_error(program, op_pc, "DIVISION BY ZERO");
                    result = a.number / b.number;
                }

                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::STORE) {
                    const int dst = program.code[pc].a;
                    if (dst >= 0 &&
                        static_cast<std::size_t>(dst) <
                            program.symbol_count &&
                        !program.symbols[dst].is_string) {
                        numbers_[dst] = result;
                        ++dispatch_count;
                        ++pc;
                        break;
                    }
                }

                if (!push(Value::num(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::NEG: {
                Value value;
                if (!pop(value))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (value.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");
                if (!push(Value::num(-value.number)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::CEQ_NUM:
            case OpCode::CNE_NUM:
            case OpCode::CLT_NUM:
            case OpCode::CLE_NUM:
            case OpCode::CGT_NUM:
            case OpCode::CGE_NUM: {
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                const BasicNumber lhs = stack[sp - 2].number;
                const BasicNumber rhs = stack[sp - 1].number;
                bool result = false;

                if (op.code == OpCode::CEQ_NUM) result = lhs == rhs;
                else if (op.code == OpCode::CNE_NUM) result = lhs != rhs;
                else if (op.code == OpCode::CLT_NUM) result = lhs < rhs;
                else if (op.code == OpCode::CLE_NUM) result = lhs <= rhs;
                else if (op.code == OpCode::CGT_NUM) result = lhs > rhs;
                else result = lhs >= rhs;

                stack[sp - 2].is_string = false;
                stack[sp - 2].number = result ? -1.0 : 0.0;
                --sp;
                break;
            }

            case OpCode::CEQ_NUM_JZ:
            case OpCode::CNE_NUM_JZ:
            case OpCode::CLT_NUM_JZ:
            case OpCode::CLE_NUM_JZ:
            case OpCode::CGT_NUM_JZ:
            case OpCode::CGE_NUM_JZ: {
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                const BasicNumber lhs = stack[sp - 2].number;
                const BasicNumber rhs = stack[sp - 1].number;
                bool result = false;

                if (op.code == OpCode::CEQ_NUM_JZ) result = lhs == rhs;
                else if (op.code == OpCode::CNE_NUM_JZ) result = lhs != rhs;
                else if (op.code == OpCode::CLT_NUM_JZ) result = lhs < rhs;
                else if (op.code == OpCode::CLE_NUM_JZ) result = lhs <= rhs;
                else if (op.code == OpCode::CGT_NUM_JZ) result = lhs > rhs;
                else result = lhs >= rhs;

                sp -= 2;
                ++dispatch_count; // account for the covered JZ
                if (!result) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                } else {
                    ++pc;
                }
                break;
            }

            case OpCode::NOT_NUM:
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                stack[sp - 1].is_string = false;
                stack[sp - 1].number =
                    stack[sp - 1].number == 0.0 ? -1.0 : 0.0;
                break;

            case OpCode::AND_NUM:
            case OpCode::OR_NUM:
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                stack[sp - 2].is_string = false;
                stack[sp - 2].number =
                    (op.code == OpCode::AND_NUM
                        ? (stack[sp - 2].number != 0.0 &&
                           stack[sp - 1].number != 0.0)
                        : (stack[sp - 2].number != 0.0 ||
                           stack[sp - 1].number != 0.0))
                    ? -1.0 : 0.0;
                --sp;
                break;

            case OpCode::NOT_NUM_JZ: {
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                const bool result = stack[--sp].number == 0.0;
                ++dispatch_count;
                if (!result) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                } else {
                    ++pc;
                }
                break;
            }

            case OpCode::AND_NUM_JZ:
            case OpCode::OR_NUM_JZ: {
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                const bool lhs = stack[sp - 2].number != 0.0;
                const bool rhs = stack[sp - 1].number != 0.0;
                const bool result =
                    op.code == OpCode::AND_NUM_JZ
                        ? (lhs && rhs)
                        : (lhs || rhs);

                sp -= 2;
                ++dispatch_count;
                if (!result) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                } else {
                    ++pc;
                }
                break;
            }

            case OpCode::CEQ:
            case OpCode::CNE:
            case OpCode::CLT:
            case OpCode::CLE:
            case OpCode::CGT:
            case OpCode::CGE: {
                Value b;
                Value a;

                if (!pop(b) || !pop(a))
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                int cmp = 0;

                if (!a.is_string && !b.is_string) {
                    if (a.number < b.number) cmp = -1;
                    else if (a.number > b.number) cmp = 1;
                } else {
                    char an[48] = {};
                    char bn[48] = {};
                    cmp = std::strcmp(
                        value_text(a, an, sizeof(an)),
                        value_text(b, bn, sizeof(bn))
                    );
                }

                bool result = false;

                if (op.code == OpCode::CEQ) result = cmp == 0;
                else if (op.code == OpCode::CNE) result = cmp != 0;
                else if (op.code == OpCode::CLT) result = cmp < 0;
                else if (op.code == OpCode::CLE) result = cmp <= 0;
                else if (op.code == OpCode::CGT) result = cmp > 0;
                else if (op.code == OpCode::CGE) result = cmp >= 0;

                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::JZ) {
                    const Op& jz = program.code[pc];

                    if (!result) {
                        if (jz.a < 0 ||
                            static_cast<std::size_t>(jz.a) >=
                                program.code_count) {
                            return make_error(
                                program, op_pc, "BAD JUMP TARGET"
                            );
                        }
                        pc = jz.a;
                    } else {
                        ++dispatch_count;
                        ++pc;
                    }
                    break;
                }

                if (!push(truth(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::NOT: {
                Value value;
                if (!pop(value))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (value.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const bool result = value.number == 0.0;

                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::JZ) {
                    const Op& jz = program.code[pc];
                    if (!result) {
                        if (jz.a < 0 ||
                            static_cast<std::size_t>(jz.a) >= program.code_count)
                            return make_error(program, op_pc, "BAD JUMP TARGET");
                        pc = jz.a;
                    } else {
                        ++dispatch_count;
                        ++pc;
                    }
                    break;
                }

                if (!push(truth(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::AND:
            case OpCode::OR: {
                Value b;
                Value a;

                if (!pop(b) || !pop(a))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (a.is_string || b.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const bool av = a.number != 0.0;
                const bool bv = b.number != 0.0;
                const bool result =
                    op.code == OpCode::AND ? (av && bv) : (av || bv);

                if (static_cast<std::size_t>(pc) < program.code_count &&
                    program.code[pc].code == OpCode::JZ) {
                    const Op& jz = program.code[pc];

                    if (!result) {
                        if (jz.a < 0 ||
                            static_cast<std::size_t>(jz.a) >= program.code_count)
                            return make_error(program, op_pc, "BAD JUMP TARGET");
                        pc = jz.a;
                    } else {
                        ++dispatch_count;
                        ++pc;
                    }
                    break;
                }

                if (!push(truth(result)))
                    return make_error(program, op_pc, "STACK OVERFLOW");
                break;
            }

            case OpCode::FN0_NUM: {
                BasicNumber result = 0.0;

                if (op.a == FnId::PI) {
                    result = 3.14159265358979323846;
                } else if (op.a == FnId::RND) {
                    result = random_unit();
                } else if (op.a == FnId::TIMER) {
                    result =
                        static_cast<BasicNumber>(
                            to_ms_since_boot(get_absolute_time())
                        ) / 1000.0;
                } else {
                    return make_error(
                        program,
                        op_pc,
                        "BAD NUMERIC FUNCTION"
                    );
                }

                if (!push(Value::num(result))) {
                    return make_error(program, op_pc, "STACK OVERFLOW");
                }
                break;
            }

            case OpCode::FN1_NUM: {
                if (sp == 0)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (stack[sp - 1].is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const BasicNumber x = stack[sp - 1].number;
                BasicNumber result = 0.0;

                switch (op.a) {
                    case FnId::ABS:
                        result = std::fabs(x);
                        break;
                    case FnId::INT:
                        result = std::floor(x);
                        break;
                    case FnId::SIN:
                        result = std::sin(x);
                        break;
                    case FnId::COS:
                        result = std::cos(x);
                        break;
                    case FnId::TAN:
                        result = std::tan(x);
                        break;
                    case FnId::SQR:
                        if (x < 0.0)
                            return make_error(
                                program,
                                op_pc,
                                "DOMAIN ERROR"
                            );
                        result = std::sqrt(x);
                        break;
                    case FnId::ATN:
                        result = std::atan(x);
                        break;
                    case FnId::LOG:
                        if (x <= 0.0)
                            return make_error(
                                program,
                                op_pc,
                                "DOMAIN ERROR"
                            );
                        result = std::log(x);
                        break;
                    case FnId::EXP:
                        result = std::exp(x);
                        break;
                    case FnId::RAD:
                        result = x * 3.14159265358979323846 / 180.0;
                        break;
                    case FnId::DEG:
                        result = x * 180.0 / 3.14159265358979323846;
                        break;
                    case FnId::SGN:
                        result = x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
                        break;
                    case FnId::RNDI: {
                        int n = static_cast<int>(x);
                        if (n < 0) n = 0;
                        result =
                            n == 0
                                ? 0.0
                                : static_cast<BasicNumber>(
                                      next_random() %
                                      (static_cast<std::uint32_t>(n) + 1u)
                                  );
                        break;
                    }
                    default:
                        return make_error(
                            program,
                            op_pc,
                            "BAD NUMERIC FUNCTION"
                        );
                }

                stack[sp - 1].number = result;
                break;
            }

            case OpCode::FN2_NUM: {
                if (sp < 2)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (stack[sp - 2].is_string || stack[sp - 1].is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const BasicNumber a = stack[sp - 2].number;
                const BasicNumber b = stack[sp - 1].number;
                BasicNumber result = 0.0;

                if (op.a == FnId::MIN) {
                    result = a < b ? a : b;
                } else if (op.a == FnId::MAX) {
                    result = a > b ? a : b;
                } else {
                    return make_error(
                        program,
                        op_pc,
                        "BAD NUMERIC FUNCTION"
                    );
                }

                --sp;
                stack[sp - 1].is_string = false;
                stack[sp - 1].number = result;
                break;
            }

            case OpCode::FN3_NUM: {
                if (sp < 3)
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (stack[sp - 3].is_string ||
                    stack[sp - 2].is_string ||
                    stack[sp - 1].is_string) {
                    return make_error(program, op_pc, "TYPE MISMATCH");
                }

                if (op.a != FnId::CLAMP) {
                    return make_error(
                        program,
                        op_pc,
                        "BAD NUMERIC FUNCTION"
                    );
                }

                BasicNumber x = stack[sp - 3].number;
                const BasicNumber lo = stack[sp - 2].number;
                const BasicNumber hi = stack[sp - 1].number;
                if (x < lo) x = lo;
                if (x > hi) x = hi;

                sp -= 2;
                stack[sp - 1].is_string = false;
                stack[sp - 1].number = x;
                break;
            }

            case OpCode::CALLFN: {
                if (op.a == FnId::INPUT) {
                    const int slot = op.b;

                    if (slot < 0 ||
                        static_cast<std::size_t>(slot) >= program.symbol_count) {
                        return make_error(program, op_pc, "BAD INPUT VARIABLE");
                    }

                    char input[192] = {};
                    LineEditor::read(input, sizeof(input));

                    if (program.symbols[slot].is_string) {
                        std::snprintf(
                            strings_[slot],
                            sizeof(strings_[slot]),
                            "%s",
                            input
                        );
                    } else {
                        char* end = nullptr;
                        const BasicNumber value = std::strtof(input, &end);

                        while (end && (*end == ' ' || *end == '\t')) ++end;

                        if (!end || end == input || *end != '\0') {
                            return make_error(program, op_pc, "REDO FROM START");
                        }

                        numbers_[slot] = value;
                    }
                    break;
                }

                int argc = op.b;
                const bool line_shorthand =
                    op.a == FnId::GLINE && (argc & (1 << 30)) != 0;
                if (line_shorthand) argc &= ~(1 << 30);

                if (argc < 0 || argc > 8)
                    return make_error(program, op_pc, "ARGUMENT COUNT");

                Value args[8] = {};
                for (int i = argc - 1; i >= 0; --i) {
                    if (!pop(args[i]))
                        return make_error(program, op_pc, "STACK UNDERFLOW");
                }

                auto arg_num = [&](int i, BasicNumber& value) -> bool {
                    if (i < 0 || i >= argc || args[i].is_string) return false;
                    value = args[i].number;
                    return true;
                };

                auto arg_text = [&](int i, char* temp, std::size_t size) -> const char* {
                    if (i < 0 || i >= argc) return "";
                    return value_text(args[i], temp, size);
                };

                switch (op.a) {
                    case FnId::ABS:
                    case FnId::INT:
                    case FnId::SIN:
                    case FnId::COS:
                    case FnId::TAN:
                    case FnId::SQR:
                    case FnId::ATN:
                    case FnId::LOG:
                    case FnId::EXP:
                    case FnId::RAD:
                    case FnId::DEG:
                    case FnId::SGN: {
                        if (argc != 1 || args[0].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        const BasicNumber x = args[0].number;
                        BasicNumber result = 0.0;

                        if (op.a == FnId::ABS) result = std::fabs(x);
                        else if (op.a == FnId::INT) result = std::floor(x);
                        else if (op.a == FnId::SIN) result = std::sin(x);
                        else if (op.a == FnId::COS) result = std::cos(x);
                        else if (op.a == FnId::TAN) result = std::tan(x);
                        else if (op.a == FnId::SQR) {
                            if (x < 0.0)
                                return make_error(program, op_pc, "DOMAIN ERROR");
                            result = std::sqrt(x);
                        } else if (op.a == FnId::ATN) result = std::atan(x);
                        else if (op.a == FnId::LOG) {
                            if (x <= 0.0)
                                return make_error(program, op_pc, "DOMAIN ERROR");
                            result = std::log(x);
                        } else if (op.a == FnId::EXP) result = std::exp(x);
                        else if (op.a == FnId::RAD) result = x * 3.14159265358979323846 / 180.0;
                        else if (op.a == FnId::DEG) result = x * 180.0 / 3.14159265358979323846;
                        else result = x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);

                        if (!push(Value::num(result)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::MIN:
                    case FnId::MAX: {
                        if (argc != 2 || args[0].is_string || args[1].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        const BasicNumber result =
                            op.a == FnId::MIN
                                ? (args[0].number < args[1].number ? args[0].number : args[1].number)
                                : (args[0].number > args[1].number ? args[0].number : args[1].number);

                        if (!push(Value::num(result)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::CLAMP: {
                        if (argc != 3 ||
                            args[0].is_string ||
                            args[1].is_string ||
                            args[2].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        BasicNumber x = args[0].number;
                        if (x < args[1].number) x = args[1].number;
                        if (x > args[2].number) x = args[2].number;

                        if (!push(Value::num(x)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::PI:
                        if (argc != 0)
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        if (!push(Value::num(3.14159265358979323846)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;

                    case FnId::RND:
                        if (argc != 0)
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        if (!push(Value::num(random_unit())))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;

                    case FnId::RNDI: {
                        if (argc != 1 || args[0].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        int n = static_cast<int>(args[0].number);
                        if (n < 0) n = 0;

                        const int result =
                            n == 0 ? 0 :
                            static_cast<int>(next_random() % (static_cast<std::uint32_t>(n) + 1u));

                        if (!push(Value::num(result)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::TIMER:
                        if (argc != 0)
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        if (!push(Value::num(
                                static_cast<BasicNumber>(to_ms_since_boot(get_absolute_time())) / 1000.0
                            ))) {
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        }
                        break;

                    case FnId::RANDOMIZE:
                        if (argc > 1 || (argc == 1 && args[0].is_string))
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        if (argc == 1) {
                            random_state_ =
                                static_cast<std::uint32_t>(args[0].number);
                        } else {
                            random_state_ =
                                static_cast<std::uint32_t>(
                                    to_us_since_boot(get_absolute_time())
                                );
                        }

                        if (random_state_ == 0) random_state_ = 1;
                        break;

                    case FnId::STRS: {
                        if (argc != 1)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        char temp[48] = {};
                        const char* text = arg_text(0, temp, sizeof(temp));
                        char* dst = scratch_buffer();
                        std::snprintf(dst, kScratchSize, "%s", text);

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::VAL: {
                        if (argc != 1)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        char temp[48] = {};
                        const char* text = arg_text(0, temp, sizeof(temp));
                        char* end = nullptr;
                        const BasicNumber value = std::strtof(text, &end);

                        if (end == text) {
                            if (!push(Value::num(0.0)))
                                return make_error(program, op_pc, "STACK OVERFLOW");
                        } else if (!push(Value::num(value))) {
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        }
                        break;
                    }

                    case FnId::LEN:
                    case FnId::ASC: {
                        if (argc != 1)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        char temp[48] = {};
                        const char* text = arg_text(0, temp, sizeof(temp));
                        const BasicNumber result =
                            op.a == FnId::LEN
                                ? static_cast<BasicNumber>(std::strlen(text))
                                : static_cast<BasicNumber>(
                                    *text ? static_cast<unsigned char>(*text) : 0
                                );

                        if (!push(Value::num(result)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::CHRS: {
                        if (argc != 1 || args[0].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        char* dst = scratch_buffer();
                        dst[0] = static_cast<char>(
                            static_cast<int>(args[0].number) & 0xff
                        );
                        dst[1] = '\0';

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::LEFTS:
                    case FnId::RIGHTS: {
                        if (argc != 2 || args[1].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        char temp[48] = {};
                        const char* text = arg_text(0, temp, sizeof(temp));
                        const std::size_t len = std::strlen(text);
                        int count = static_cast<int>(args[1].number);
                        if (count < 0) count = 0;
                        if (static_cast<std::size_t>(count) > len)
                            count = static_cast<int>(len);

                        char* dst = scratch_buffer();

                        if (op.a == FnId::LEFTS) {
                            std::memcpy(dst, text, static_cast<std::size_t>(count));
                        } else {
                            std::memcpy(
                                dst,
                                text + len - static_cast<std::size_t>(count),
                                static_cast<std::size_t>(count)
                            );
                        }

                        dst[count] = '\0';

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::MIDS: {
                        if ((argc != 2 && argc != 3) ||
                            args[1].is_string ||
                            (argc == 3 && args[2].is_string)) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        char temp[48] = {};
                        const char* text = arg_text(0, temp, sizeof(temp));
                        const std::size_t len = std::strlen(text);

                        int start = static_cast<int>(args[1].number);
                        if (start < 1) start = 1;

                        int count =
                            argc == 3
                                ? static_cast<int>(args[2].number)
                                : static_cast<int>(len);

                        if (count < 0) count = 0;

                        const std::size_t offset =
                            static_cast<std::size_t>(start - 1);

                        char* dst = scratch_buffer();

                        if (offset >= len) {
                            dst[0] = '\0';
                        } else {
                            std::size_t available = len - offset;
                            std::size_t take = static_cast<std::size_t>(count);
                            if (take > available) take = available;
                            if (take >= kScratchSize) take = kScratchSize - 1;

                            std::memcpy(dst, text + offset, take);
                            dst[take] = '\0';
                        }

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::SPC:
                    case FnId::TAB: {
                        if (argc != 1 || args[0].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        int count = 0;

                        if (op.a == FnId::SPC) {
                            count = static_cast<int>(args[0].number);
                        } else {
                            const int target = static_cast<int>(args[0].number);
                            const int current = platform::cursor_column() + 1;
                            count = target > current ? target - current : 0;
                        }

                        if (count < 0) count = 0;
                        if (count >= static_cast<int>(kScratchSize))
                            count = static_cast<int>(kScratchSize) - 1;

                        char* dst = scratch_buffer();
                        std::memset(dst, ' ', static_cast<std::size_t>(count));
                        dst[count] = '\0';

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }
                    case FnId::STRINGS: {
                        if ((argc != 1 && argc != 2) ||
                            args[0].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        int count = static_cast<int>(args[0].number);
                        if (count < 0) count = 0;
                        if (count >= static_cast<int>(kScratchSize))
                            count = static_cast<int>(kScratchSize) - 1;

                        char ch = ' ';

                        if (argc == 2) {
                            if (args[1].is_string) {
                                ch = (args[1].string && args[1].string[0])
                                    ? args[1].string[0]
                                    : ' ';
                            } else {
                                ch = static_cast<char>(
                                    static_cast<int>(args[1].number) & 0xff
                                );
                            }
                        }

                        char* dst = scratch_buffer();
                        std::memset(dst, ch, static_cast<std::size_t>(count));
                        dst[count] = '\0';

                        if (!push_string(dst))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::INSTR: {
                        if (argc != 2 && argc != 3)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        int start = 1;
                        int hay_index = 0;
                        int needle_index = 1;

                        if (argc == 3) {
                            if (args[0].is_string)
                                return make_error(program, op_pc, "TYPE MISMATCH");
                            start = static_cast<int>(args[0].number);
                            hay_index = 1;
                            needle_index = 2;
                        }

                        char hay_temp[48] = {};
                        char needle_temp[48] = {};
                        const char* hay =
                            arg_text(hay_index, hay_temp, sizeof(hay_temp));
                        const char* needle =
                            arg_text(needle_index, needle_temp, sizeof(needle_temp));

                        if (start < 1) start = 1;

                        const std::size_t hay_len = std::strlen(hay);
                        const std::size_t offset =
                            static_cast<std::size_t>(start - 1);

                        BasicNumber result = 0.0;

                        if (offset <= hay_len) {
                            const char* found = std::strstr(hay + offset, needle);
                            if (found) {
                                result =
                                    static_cast<BasicNumber>(found - hay + 1);
                            }
                        }

                        if (!push(Value::num(result)))
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        break;
                    }

                    case FnId::LOCATE: {
                        if (argc != 2 ||
                            args[0].is_string ||
                            args[1].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        int col = static_cast<int>(args[0].number) - 1;
                        int row = static_cast<int>(args[1].number) - 1;
                        platform::set_cursor_position(col, row);
                        break;
                    }

                    case FnId::SCREEN: {
                        int w = 640;
                        int h = 480;

                        if (argc == 2 &&
                            !args[0].is_string &&
                            !args[1].is_string) {
                            w = static_cast<int>(args[0].number);
                            h = static_cast<int>(args[1].number);
                        } else if (argc != 0) {
                            return make_error(
                                program,
                                op_pc,
                                "SCREEN: ARGUMENT COUNT"
                            );
                        }

                        if (w <= 0 || h <= 0) {
                            return make_error(
                                program,
                                op_pc,
                                "SCREEN: BAD SIZE"
                            );
                        }

                        configure_virtual_screen(w, h);
                        break;
                    }

                    case FnId::GCLS:
                        if (argc != 0)
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        platform::graphics_clear(0x000000);
                        break;

                    case FnId::GCOLOR: {
                        std::uint32_t color = 0xffffff;

                        if (argc == 1 && !args[0].is_string) {
                            color = palette(static_cast<int>(args[0].number));
                        } else if (argc == 3 &&
                                   !args[0].is_string &&
                                   !args[1].is_string &&
                                   !args[2].is_string) {
                            int r = static_cast<int>(args[0].number);
                            int g = static_cast<int>(args[1].number);
                            int b = static_cast<int>(args[2].number);
                            if (r < 0) r = 0; if (r > 255) r = 255;
                            if (g < 0) g = 0; if (g > 255) g = 255;
                            if (b < 0) b = 0; if (b > 255) b = 255;
                            color =
                                (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g) << 8) |
                                static_cast<std::uint32_t>(b);
                        } else {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        platform::set_graphics_color(color);
                        break;
                    }

                    case FnId::GCOLORHSV: {
                        if (argc != 3 ||
                            args[0].is_string ||
                            args[1].is_string ||
                            args[2].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        BasicNumber h = std::fmod(args[0].number, 360.0);
                        if (h < 0.0) h += 360.0;
                        BasicNumber s = args[1].number;
                        BasicNumber v = args[2].number;
                        if (s < 0.0) s = 0.0; if (s > 1.0) s = 1.0;
                        if (v < 0.0) v = 0.0; if (v > 1.0) v = 1.0;

                        const BasicNumber c = v * s;
                        const BasicNumber x =
                            c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
                        const BasicNumber m = v - c;

                        BasicNumber r = 0.0;
                        BasicNumber g = 0.0;
                        BasicNumber b = 0.0;

                        if (h < 60.0) { r = c; g = x; }
                        else if (h < 120.0) { r = x; g = c; }
                        else if (h < 180.0) { g = c; b = x; }
                        else if (h < 240.0) { g = x; b = c; }
                        else if (h < 300.0) { r = x; b = c; }
                        else { r = c; b = x; }

                        const int ri = static_cast<int>(std::round((r + m) * 255.0));
                        const int gi = static_cast<int>(std::round((g + m) * 255.0));
                        const int bi = static_cast<int>(std::round((b + m) * 255.0));

                        platform::set_graphics_color(
                            (static_cast<std::uint32_t>(ri) << 16) |
                            (static_cast<std::uint32_t>(gi) << 8) |
                            static_cast<std::uint32_t>(bi)
                        );
                        break;
                    }

                    case FnId::GPSET: {
                        if ((argc != 2 && argc != 3) ||
                            args[0].is_string ||
                            args[1].is_string ||
                            (argc == 3 && args[2].is_string)) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        const std::uint32_t old = platform::graphics_color();
                        if (argc == 3) {
                            platform::set_graphics_color(
                                palette(static_cast<int>(args[2].number))
                            );
                        }

                        platform::graphics_pixel(
                            map_graphics_x(args[0].number),
                            map_graphics_y(args[1].number)
                        );

                        if (argc == 3) platform::set_graphics_color(old);
                        break;
                    }

                    case FnId::GLINE: {
                        const bool has_color =
                            (!line_shorthand && argc == 5) ||
                            (line_shorthand && argc == 3);

                        if ((!line_shorthand && argc != 4 && argc != 5) ||
                            (line_shorthand && argc != 2 && argc != 3)) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        for (int i = 0; i < argc; ++i) {
                            if (args[i].is_string)
                                return make_error(program, op_pc, "TYPE MISMATCH");
                        }

                        const std::uint32_t old = platform::graphics_color();
                        if (has_color) {
                            platform::set_graphics_color(
                                palette(static_cast<int>(args[argc - 1].number))
                            );
                        }

                        if (line_shorthand) {
                            platform::graphics_line_to(
                                map_graphics_x(args[0].number),
                                map_graphics_y(args[1].number)
                            );
                        } else {
                            platform::graphics_line(
                                map_graphics_x(args[0].number),
                                map_graphics_y(args[1].number),
                                map_graphics_x(args[2].number),
                                map_graphics_y(args[3].number)
                            );
                        }

                        if (has_color) platform::set_graphics_color(old);
                        break;
                    }

                    case FnId::GCIRCLE: {
                        if ((argc != 3 && argc != 4) ||
                            args[0].is_string ||
                            args[1].is_string ||
                            args[2].is_string ||
                            (argc == 4 && args[3].is_string)) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        const std::uint32_t old = platform::graphics_color();
                        if (argc == 4) {
                            platform::set_graphics_color(
                                palette(static_cast<int>(args[3].number))
                            );
                        }

                        platform::graphics_circle(
                            map_graphics_x(args[0].number),
                            map_graphics_y(args[1].number),
                            map_graphics_radius(args[2].number)
                        );

                        if (argc == 4) platform::set_graphics_color(old);
                        break;
                    }

                    case FnId::GBOX: {
                        if (argc < 4 || argc > 6)
                            return make_error(program, op_pc, "ARGUMENT COUNT");

                        for (int i = 0; i < argc; ++i) {
                            if (args[i].is_string)
                                return make_error(program, op_pc, "TYPE MISMATCH");
                        }

                        bool filled = false;
                        bool has_color = false;

                        if (argc >= 5) filled = args[4].number != 0.0;
                        if (argc == 6) has_color = true;

                        const std::uint32_t old = platform::graphics_color();
                        if (has_color) {
                            platform::set_graphics_color(
                                palette(static_cast<int>(args[5].number))
                            );
                        }

                        platform::graphics_box(
                            map_graphics_x(args[0].number),
                            map_graphics_y(args[1].number),
                            map_graphics_x(args[2].number),
                            map_graphics_y(args[3].number),
                            filled
                        );

                        if (has_color) platform::set_graphics_color(old);
                        break;
                    }

                    case FnId::GPAINT: {
                        if ((argc != 2 && argc != 3) ||
                            args[0].is_string ||
                            args[1].is_string ||
                            (argc == 3 && args[2].is_string)) {
                            return make_error(
                                program,
                                op_pc,
                                "PAINT: ARGUMENT COUNT"
                            );
                        }

                        const std::uint32_t old = platform::graphics_color();
                        if (argc == 3) {
                            platform::set_graphics_color(
                                palette(static_cast<int>(args[2].number))
                            );
                        }

                        const bool ok = platform::graphics_paint(
                            map_graphics_x(args[0].number),
                            map_graphics_y(args[1].number)
                        );

                        if (argc == 3) {
                            platform::set_graphics_color(old);
                        }

                        if (!ok) {
                            return make_error(
                                program,
                                op_pc,
                                "PAINT FAILED"
                            );
                        }
                        break;
                    }

                    case FnId::GPOINT: {
                        if (argc != 2 ||
                            args[0].is_string ||
                            args[1].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }

                        if (!push(truth(
                                platform::graphics_point_nonblack(
                                    map_graphics_x(args[0].number),
                                    map_graphics_y(args[1].number)
                                )
                            ))) {
                            return make_error(program, op_pc, "STACK OVERFLOW");
                        }
                        break;
                    }

                    case FnId::GFLUSH:
                        platform::graphics_flush();
                        break;

                    case FnId::GSAVE: {
                        if (argc != 1 && argc != 5) {
                            return make_error(
                                program,
                                op_pc,
                                "SAVE IMAGE: ARGUMENT COUNT"
                            );
                        }
                        if (!args[0].is_string) {
                            return make_error(
                                program,
                                op_pc,
                                "SAVE IMAGE: FILENAME REQUIRED"
                            );
                        }

                        int x1 = 0;
                        int y1 = 0;
                        int x2 = 319;
                        int y2 = 319;

                        if (argc == 5) {
                            for (int i = 1; i < 5; ++i) {
                                if (args[i].is_string) {
                                    return make_error(
                                        program,
                                        op_pc,
                                        "SAVE IMAGE: BAD REGION"
                                    );
                                }
                            }
                            x1 = static_cast<int>(args[1].number);
                            y1 = static_cast<int>(args[2].number);
                            x2 = static_cast<int>(args[3].number);
                            y2 = static_cast<int>(args[4].number);
                        }

                        platform::graphics_flush();

                        if (!storage::save_screenshot(
                                args[0].string,
                                x1, y1, x2, y2
                            )) {
                            return make_error(
                                program,
                                op_pc,
                                storage::last_error()
                            );
                        }
                        break;
                    }

                    case FnId::GSLEEP:
                        if (argc != 1 || args[0].is_string)
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        sleep_ms(
                            args[0].number < 0.0
                                ? 0u
                                : static_cast<std::uint32_t>(args[0].number)
                        );
                        break;

                    case FnId::GLOCATE:
                        if (argc != 2 ||
                            args[0].is_string ||
                            args[1].is_string) {
                            return make_error(program, op_pc, "ARGUMENT COUNT");
                        }
                        platform::set_cursor_position(
                            map_graphics_x(args[0].number) / 6,
                            map_graphics_y(args[1].number) / 8
                        );
                        break;

                    case FnId::GPRINT:
                        for (int i = 0; i < argc; ++i) {
                            char temp[48] = {};
                            platform::put_string(
                                arg_text(i, temp, sizeof(temp))
                            );
                        }
                        break;

                    default:
                        return make_error(program, op_pc, "UNDEF'D FUNCTION");
                }

                break;
            }

            case OpCode::PRINT: {
                Value value;
                if (!pop(value))
                    return make_error(program, op_pc, "STACK UNDERFLOW");

                if (value.is_string) {
                    platform::put_string(value.string ? value.string : "");
                } else {
                    char number[48] = {};
                    format_number(value.number, number, sizeof(number));
                    platform::put_string(number);
                }
                break;
            }

            case OpCode::PRINT_NL:
                platform::put_string("\r\n");
                break;

            case OpCode::PRINT_SPC: {
                const int zone = 14;
                const int col = platform::cursor_column();
                int spaces = zone - (col % zone);
                if (spaces <= 0) spaces = zone;
                for (int i = 0; i < spaces; ++i) {
                    platform::put_char(' ');
                }
                break;
            }

            case OpCode::PRINT_SUPPRESS_NL:
                break;

            case OpCode::JMP:
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.code_count) {
                    return make_error(program, op_pc, "BAD JUMP TARGET");
                }
                pc = op.a;
                break;

            case OpCode::JZ: {
                Value value;
                if (!pop(value))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (value.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                if (value.number == 0.0) {
                    if (op.a < 0 ||
                        static_cast<std::size_t>(op.a) >= program.code_count) {
                        return make_error(program, op_pc, "BAD JUMP TARGET");
                    }
                    pc = op.a;
                }
                break;
            }

            case OpCode::GOSUB:
                if (return_sp >= kReturnStackSize)
                    return make_error(program, op_pc, "GOSUB STACK OVERFLOW");
                if (op.a < 0 ||
                    static_cast<std::size_t>(op.a) >= program.code_count) {
                    return make_error(program, op_pc, "BAD GOSUB TARGET");
                }
                return_stack[return_sp++] = pc;
                pc = op.a;
                break;

            case OpCode::RETSUB:
                if (return_sp == 0)
                    return make_error(program, op_pc, "RETURN WITHOUT GOSUB");
                pc = return_stack[--return_sp];
                break;

            case OpCode::ON_GOTO:
            case OpCode::ON_GOSUB: {
                Value selector;
                if (!pop(selector))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (selector.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");

                const int count = op.a;
                if (count <= 0 ||
                    pc < 0 ||
                    static_cast<std::size_t>(pc + count) >
                        program.code_count) {
                    return make_error(program, op_pc, "BAD ON TABLE");
                }

                const int selected = static_cast<int>(selector.number);
                const std::int32_t after_table = pc + count;

                if (selected < 1 || selected > count) {
                    pc = after_table;
                    break;
                }

                const Op& entry = program.code[
                    pc + static_cast<std::int32_t>(selected - 1)
                ];
                if (entry.code != OpCode::JMP ||
                    entry.a < 0 ||
                    static_cast<std::size_t>(entry.a) >=
                        program.code_count) {
                    return make_error(program, op_pc, "BAD ON TARGET");
                }

                if (op.code == OpCode::ON_GOSUB) {
                    if (return_sp >= kReturnStackSize) {
                        return make_error(
                            program,
                            op_pc,
                            "GOSUB STACK OVERFLOW"
                        );
                    }
                    return_stack[return_sp++] = after_table;
                }

                pc = entry.a;
                break;
            }

            case OpCode::FOR_INIT: {
                Value step;
                Value end;

                if (!pop(step) || !pop(end))
                    return make_error(program, op_pc, "STACK UNDERFLOW");
                if (step.is_string || end.is_string)
                    return make_error(program, op_pc, "TYPE MISMATCH");
                if (step.number == 0.0)
                    return make_error(program, op_pc, "STEP CANNOT BE ZERO");
                if (for_sp >= kForStackSize)
                    return make_error(program, op_pc, "FOR STACK OVERFLOW");

                ForFrame& frame = for_stack[for_sp++];
                frame.slot = op.a;
                frame.end = end.number;
                frame.step = step.number;
                frame.check_pc = pc;
                frame.body_pc = 0;
                break;
            }

            case OpCode::FOR_CHECK: {
                if (for_sp == 0)
                    return make_error(program, op_pc, "FOR STACK UNDERFLOW");

                ForFrame& frame = for_stack[for_sp - 1];
                frame.body_pc = op.b;

                if (frame.slot < 0 ||
                    static_cast<std::size_t>(frame.slot) >= kMaxSymbols) {
                    return make_error(program, op_pc, "BAD FOR VARIABLE");
                }

                const BasicNumber current = numbers_[frame.slot];
                const bool cont =
                    frame.step >= 0.0
                        ? current <= frame.end
                        : current >= frame.end;

                if (cont) {
                    pc = frame.body_pc;
                } else {
                    --for_sp;
                }
                break;
            }

            case OpCode::FOR_INCR: {
                if (for_sp == 0)
                    return make_error(program, op_pc, "NEXT WITHOUT FOR");

                // Match the original RetroMiniBASIC VM semantics:
                // NEXT <var> searches downward for that FOR frame and drops
                // abandoned inner loops. This is important for classic BASIC
                // programs that GOTO out of an inner FOR and later execute
                // NEXT on an outer variable.
                if (op.a >= 0) {
                    while (for_sp > 0 &&
                           for_stack[for_sp - 1].slot != op.a) {
                        --for_sp;
                    }

                    if (for_sp == 0)
                        return make_error(program, op_pc, "NEXT WITHOUT FOR");
                }

                ForFrame& frame = for_stack[for_sp - 1];
                numbers_[frame.slot] += frame.step;

                const BasicNumber current = numbers_[frame.slot];
                const bool cont =
                    frame.step >= 0.0
                        ? current <= frame.end
                        : current >= frame.end;

                if (cont) {
                    // FOR_INCR has already performed the continuation test,
                    // so skip redispatching FOR_CHECK on every subsequent
                    // iteration and jump directly to the loop body.
                    pc = frame.body_pc;
                } else {
                    --for_sp;
                }
                break;
            }

            case OpCode::HALT: {
                platform::graphics_flush();

                if (direct_mode) {
                    save_direct_scalars(program);
                }

                if (profile_run) {
                    profile_.run_ok = true;
                }

                VmResult result;
                result.ok = true;
                result.pc = pc;
                return result;
            }

            default:
                return make_error(program, op_pc, "UNIMPLEMENTED IL OPCODE");
        }

        if (profile_run) {
            if (sp > profile_.max_stack) {
                profile_.max_stack = static_cast<std::uint16_t>(sp);
            }
            if (for_sp > profile_.max_for_depth) {
                profile_.max_for_depth =
                    static_cast<std::uint16_t>(for_sp);
            }
        }
    }

    return make_error(program, pc, "PC OUT OF RANGE");
}

} // namespace rmb
