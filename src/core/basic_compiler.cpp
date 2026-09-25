#include "basic_compiler.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rmb {

namespace {

enum class ExprType {
    Number,
    String,
    Invalid
};

int function_id(const char* name) {
    if (std::strcmp(name, "ABS") == 0) return FnId::ABS;
    if (std::strcmp(name, "INT") == 0) return FnId::INT;
    if (std::strcmp(name, "VAL") == 0) return FnId::VAL;
    if (std::strcmp(name, "STR$") == 0) return FnId::STRS;
    if (std::strcmp(name, "LEN") == 0) return FnId::LEN;
    if (std::strcmp(name, "CHR$") == 0) return FnId::CHRS;
    if (std::strcmp(name, "ASC") == 0) return FnId::ASC;
    if (std::strcmp(name, "LEFT$") == 0) return FnId::LEFTS;
    if (std::strcmp(name, "RIGHT$") == 0) return FnId::RIGHTS;
    if (std::strcmp(name, "MID$") == 0) return FnId::MIDS;
    if (std::strcmp(name, "RND") == 0) return FnId::RND;
    if (std::strcmp(name, "SPC") == 0) return FnId::SPC;
    if (std::strcmp(name, "TAB") == 0) return FnId::TAB;
    if (std::strcmp(name, "INSTR") == 0) return FnId::INSTR;
    if (std::strcmp(name, "STRING$") == 0) return FnId::STRINGS;

    if (std::strcmp(name, "SIN") == 0) return FnId::SIN;
    if (std::strcmp(name, "COS") == 0) return FnId::COS;
    if (std::strcmp(name, "TAN") == 0) return FnId::TAN;
    if (std::strcmp(name, "SQR") == 0) return FnId::SQR;
    if (std::strcmp(name, "ATN") == 0) return FnId::ATN;
    if (std::strcmp(name, "LOG") == 0) return FnId::LOG;
    if (std::strcmp(name, "EXP") == 0) return FnId::EXP;
    if (std::strcmp(name, "PI") == 0) return FnId::PI;
    if (std::strcmp(name, "RAD") == 0) return FnId::RAD;
    if (std::strcmp(name, "DEG") == 0) return FnId::DEG;
    if (std::strcmp(name, "SGN") == 0) return FnId::SGN;
    if (std::strcmp(name, "MIN") == 0) return FnId::MIN;
    if (std::strcmp(name, "MAX") == 0) return FnId::MAX;
    if (std::strcmp(name, "CLAMP") == 0) return FnId::CLAMP;
    if (std::strcmp(name, "RNDI") == 0) return FnId::RNDI;
    if (std::strcmp(name, "TIMER") == 0) return FnId::TIMER;
    if (std::strcmp(name, "INKEY") == 0) return FnId::INKEY;
    if (std::strcmp(name, "I2CREAD") == 0) return FnId::I2CREAD;
    if (std::strcmp(name, "PLAYING") == 0) return FnId::PLAYING;

    if (std::strcmp(name, "POINT") == 0) return FnId::GPOINT;
    return -1;
}

OpCode numeric_compare_opcode(OpCode op) {
    switch (op) {
        case OpCode::CEQ: return OpCode::CEQ_NUM;
        case OpCode::CNE: return OpCode::CNE_NUM;
        case OpCode::CLT: return OpCode::CLT_NUM;
        case OpCode::CLE: return OpCode::CLE_NUM;
        case OpCode::CGT: return OpCode::CGT_NUM;
        case OpCode::CGE: return OpCode::CGE_NUM;
        default: return op;
    }
}

bool function_returns_string(int id) {
    return id == FnId::STRS ||
           id == FnId::CHRS ||
           id == FnId::LEFTS ||
           id == FnId::RIGHTS ||
           id == FnId::MIDS ||
           id == FnId::SPC ||
           id == FnId::TAB ||
           id == FnId::STRINGS;
}

bool zero_arg_function(int id) {
    return id == FnId::RND || id == FnId::PI || id == FnId::TIMER ||
           id == FnId::INKEY;
}

struct CompileControl {
    struct ForEntry {
        int slot = -1;
        std::size_t skip_jump = 0;
    };
    struct WhileEntry {
        std::size_t start_pc = 0;
        std::size_t jz_pc = 0;
    };

    ForEntry for_stack[16] = {};
    std::size_t for_depth = 0;

    WhileEntry while_stack[16] = {};
    std::size_t while_depth = 0;

    std::size_t do_stack[16] = {};
    std::size_t do_depth = 0;
};

bool numeric_slot(const CompiledProgram& program, int slot) {
    return slot >= 0 &&
           static_cast<std::size_t>(slot) < program.symbol_count &&
           !program.symbols[slot].is_string;
}

void optimize_program(CompiledProgram& program) {
    bool target[kMaxOps] = {};

    // Line starts and control-flow destinations must remain independently
    // executable.  Superinstructions may cover only straight-line tails.
    for (std::size_t i = 0; i < program.line_count; ++i) {
        const int pc = program.line_at(i).pc;
        if (pc >= 0 && static_cast<std::size_t>(pc) < program.code_count) {
            target[pc] = true;
        }
    }

    for (std::size_t i = 0; i < program.code_count; ++i) {
        const Op& op = program.code[i];
        int pc = -1;

        if (op.code == OpCode::JMP ||
            op.code == OpCode::JZ ||
            op.code == OpCode::GOSUB) {
            pc = op.a;
        } else if (op.code == OpCode::FOR_CHECK) {
            pc = op.b;
        }

        if (pc >= 0 && static_cast<std::size_t>(pc) < program.code_count) {
            target[pc] = true;
        }
    }

    auto tail_is_straight = [&](std::size_t start, std::size_t length) {
        if (start + length > program.code_count) return false;
        for (std::size_t i = start + 1; i < start + length; ++i) {
            if (target[i]) return false;
        }
        return true;
    };

    for (std::size_t i = 0; i < program.code_count; ++i) {
        Op& first = program.code[i];

        // A*B - C*D + E -> DST
        // This is the dominant ZXNEXT expression in Mandelbrot/Julia loops.
        if (tail_is_straight(i, 10) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::MUL_NUM &&
            program.code[i + 3].code == OpCode::LOAD_NUM &&
            program.code[i + 4].code == OpCode::LOAD_NUM &&
            program.code[i + 5].code == OpCode::MUL_NUM &&
            program.code[i + 6].code == OpCode::SUB_NUM &&
            program.code[i + 7].code == OpCode::LOAD_NUM &&
            program.code[i + 8].code == OpCode::ADD_NUM &&
            program.code[i + 9].code == OpCode::STORE_NUM) {
            const int a = first.a;
            const int b = program.code[i + 1].a;
            const int c = program.code[i + 3].a;
            const int d = program.code[i + 4].a;
            const int e = program.code[i + 7].a;
            const int dst = program.code[i + 9].a;

            if (numeric_slot(program, a) &&
                numeric_slot(program, b) &&
                numeric_slot(program, c) &&
                numeric_slot(program, d) &&
                numeric_slot(program, e) &&
                numeric_slot(program, dst)) {
                first.code = OpCode::MULSUBADD_VVVVV_STORE;
                first.a =
                    (dst & 0x3f) |
                    ((a & 0x3f) << 6) |
                    ((b & 0x3f) << 12) |
                    ((c & 0x3f) << 18) |
                    ((d & 0x3f) << 24);
                first.b = e & 0x3f;
                first.s = 0;
                first.flags = 0;
                i += 9;
                continue;
            }
        }

        // ADDEND + LHS*RHS -> DST
        if (tail_is_straight(i, 6) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::LOAD_NUM &&
            program.code[i + 3].code == OpCode::MUL_NUM &&
            program.code[i + 4].code == OpCode::ADD_NUM &&
            program.code[i + 5].code == OpCode::STORE_NUM) {
            const int addend = first.a;
            const int lhs = program.code[i + 1].a;
            const int rhs = program.code[i + 2].a;
            const int dst = program.code[i + 5].a;

            if (numeric_slot(program, addend) &&
                numeric_slot(program, lhs) &&
                numeric_slot(program, rhs) &&
                numeric_slot(program, dst)) {
                first.code = OpCode::MULADD_VVV_STORE;
                first.a =
                    (dst & 0x3f) |
                    ((addend & 0x3f) << 6) |
                    ((lhs & 0x3f) << 12) |
                    ((rhs & 0x3f) << 18);
                first.b = 0;
                first.s = 0;
                first.flags = 0;
                i += 5;
                continue;
            }
        }

        // LHS*RHS + ADDEND -> DST (same execution opcode).
        if (tail_is_straight(i, 6) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::MUL_NUM &&
            program.code[i + 3].code == OpCode::LOAD_NUM &&
            program.code[i + 4].code == OpCode::ADD_NUM &&
            program.code[i + 5].code == OpCode::STORE_NUM) {
            const int lhs = first.a;
            const int rhs = program.code[i + 1].a;
            const int addend = program.code[i + 3].a;
            const int dst = program.code[i + 5].a;

            if (numeric_slot(program, addend) &&
                numeric_slot(program, lhs) &&
                numeric_slot(program, rhs) &&
                numeric_slot(program, dst)) {
                first.code = OpCode::MULADD_VVV_STORE;
                first.a =
                    (dst & 0x3f) |
                    ((addend & 0x3f) << 6) |
                    ((lhs & 0x3f) << 12) |
                    ((rhs & 0x3f) << 18);
                first.b = 0;
                first.s = 0;
                first.flags = 1; // product + addend evaluation order
                i += 5;
                continue;
            }
        }

        // (CONST*A)*B + C -> DST. Keep left-associative FP evaluation order.
        if (tail_is_straight(i, 8) &&
            first.code == OpCode::PUSH_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::MUL_NUM &&
            program.code[i + 3].code == OpCode::LOAD_NUM &&
            program.code[i + 4].code == OpCode::MUL_NUM &&
            program.code[i + 5].code == OpCode::LOAD_NUM &&
            program.code[i + 6].code == OpCode::ADD_NUM &&
            program.code[i + 7].code == OpCode::STORE_NUM) {
            const int constant = first.a;
            const int a = program.code[i + 1].a;
            const int b = program.code[i + 3].a;
            const int c = program.code[i + 5].a;
            const int dst = program.code[i + 7].a;

            if (constant >= 0 &&
                static_cast<std::size_t>(constant) < program.number_count &&
                numeric_slot(program, a) &&
                numeric_slot(program, b) &&
                numeric_slot(program, c) &&
                numeric_slot(program, dst)) {
                first.code = OpCode::MUL_CVV_ADD_V_STORE;
                first.a =
                    (dst & 0x3f) |
                    ((a & 0x3f) << 6) |
                    ((b & 0x3f) << 12) |
                    ((c & 0x3f) << 18);
                first.b = constant;
                first.s = 0;
                first.flags = 0;
                i += 7;
                continue;
            }
        }

        // X*X>C OR Y*Y>C, immediately consumed by JZ.
        if (tail_is_straight(i, 12) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::MUL_NUM &&
            program.code[i + 3].code == OpCode::PUSH_NUM &&
            program.code[i + 4].code == OpCode::CGT_NUM &&
            program.code[i + 5].code == OpCode::LOAD_NUM &&
            program.code[i + 6].code == OpCode::LOAD_NUM &&
            program.code[i + 7].code == OpCode::MUL_NUM &&
            program.code[i + 8].code == OpCode::PUSH_NUM &&
            program.code[i + 9].code == OpCode::CGT_NUM &&
            program.code[i + 10].code == OpCode::OR_NUM &&
            program.code[i + 11].code == OpCode::JZ) {
            const int x = first.a;
            const int x2 = program.code[i + 1].a;
            const int c1 = program.code[i + 3].a;
            const int y = program.code[i + 5].a;
            const int y2 = program.code[i + 6].a;
            const int c2 = program.code[i + 8].a;

            if (x == x2 && y == y2 && c1 == c2 &&
                numeric_slot(program, x) &&
                numeric_slot(program, y) &&
                c1 >= 0 &&
                static_cast<std::size_t>(c1) < program.number_count) {
                first.code = OpCode::SQ2_GT_CONST_OR_JZ;
                first.a = program.code[i + 11].a;
                first.b = c1;
                first.s = static_cast<std::uint16_t>(
                    (x & 0x3f) | ((y & 0x3f) << 6)
                );
                first.flags = 0;
                i += 11;
                continue;
            }
        }

        // X*X + Y*Y > C, immediately consumed by JZ.
        if (tail_is_straight(i, 10) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::MUL_NUM &&
            program.code[i + 3].code == OpCode::LOAD_NUM &&
            program.code[i + 4].code == OpCode::LOAD_NUM &&
            program.code[i + 5].code == OpCode::MUL_NUM &&
            program.code[i + 6].code == OpCode::ADD_NUM &&
            program.code[i + 7].code == OpCode::PUSH_NUM &&
            program.code[i + 8].code == OpCode::CGT_NUM &&
            program.code[i + 9].code == OpCode::JZ) {
            const int x = first.a;
            const int x2 = program.code[i + 1].a;
            const int y = program.code[i + 3].a;
            const int y2 = program.code[i + 4].a;
            const int constant = program.code[i + 7].a;

            if (x == x2 && y == y2 &&
                numeric_slot(program, x) &&
                numeric_slot(program, y) &&
                constant >= 0 &&
                static_cast<std::size_t>(constant) < program.number_count) {
                first.code = OpCode::SUMSQ_GT_CONST_JZ;
                first.a = program.code[i + 9].a;
                first.b = constant;
                first.s = static_cast<std::uint16_t>(
                    (x & 0x3f) | ((y & 0x3f) << 6)
                );
                first.flags = 0;
                i += 9;
                continue;
            }
        }

        // Numeric compare/logical result consumed immediately by JZ.
        // Keep the JZ slot in place and have the fused opcode skip it.
        if (tail_is_straight(i, 2) &&
            program.code[i + 1].code == OpCode::JZ) {
            OpCode fused = OpCode::HALT;
            switch (first.code) {
                case OpCode::CEQ_NUM: fused = OpCode::CEQ_NUM_JZ; break;
                case OpCode::CNE_NUM: fused = OpCode::CNE_NUM_JZ; break;
                case OpCode::CLT_NUM: fused = OpCode::CLT_NUM_JZ; break;
                case OpCode::CLE_NUM: fused = OpCode::CLE_NUM_JZ; break;
                case OpCode::CGT_NUM: fused = OpCode::CGT_NUM_JZ; break;
                case OpCode::CGE_NUM: fused = OpCode::CGE_NUM_JZ; break;
                case OpCode::NOT_NUM: fused = OpCode::NOT_NUM_JZ; break;
                case OpCode::AND_NUM: fused = OpCode::AND_NUM_JZ; break;
                case OpCode::OR_NUM: fused = OpCode::OR_NUM_JZ; break;
                default: break;
            }

            if (fused != OpCode::HALT) {
                first.code = fused;
                first.a = program.code[i + 1].a;
                first.b = 0;
                first.s = 0;
                first.flags = 0;
                ++i;
                continue;
            }
        }

        // LOAD R,G,B / COLOR / LOAD X,Y / PSET
        // -> one dispatch. Slots fit in 6 bits because kMaxSymbols == 64.
        if (tail_is_straight(i, 7) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::LOAD_NUM &&
            program.code[i + 3].code == OpCode::CALLFN &&
            program.code[i + 3].a == FnId::GCOLOR &&
            program.code[i + 3].b == 3 &&
            program.code[i + 4].code == OpCode::LOAD_NUM &&
            program.code[i + 5].code == OpCode::LOAD_NUM &&
            program.code[i + 6].code == OpCode::CALLFN &&
            program.code[i + 6].a == FnId::GPSET &&
            program.code[i + 6].b == 2) {
            const int rs = first.a;
            const int gs = program.code[i + 1].a;
            const int bs = program.code[i + 2].a;
            const int xs = program.code[i + 4].a;
            const int ys = program.code[i + 5].a;

            if (numeric_slot(program, rs) &&
                numeric_slot(program, gs) &&
                numeric_slot(program, bs) &&
                numeric_slot(program, xs) &&
                numeric_slot(program, ys)) {
                first.code = OpCode::RGB_PSET_VV;
                first.a =
                    (rs & 0x3f) |
                    ((gs & 0x3f) << 6) |
                    ((bs & 0x3f) << 12) |
                    ((xs & 0x3f) << 18) |
                    ((ys & 0x3f) << 24);
                first.b = 0;
                first.s = 0;
                first.flags = 0;
                i += 6;
                continue;
            }
        }

        // LOAD lhs, LOAD rhs, ADD/SUB/MUL, STORE dst
        // -> one dispatch and no VM stack traffic.
        if (tail_is_straight(i, 4) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 3].code == OpCode::STORE_NUM) {
            const int lhs = first.a;
            const int rhs = program.code[i + 1].a;
            const int dst = program.code[i + 3].a;
            const OpCode arith = program.code[i + 2].code;

            if (numeric_slot(program, lhs) &&
                numeric_slot(program, rhs) &&
                numeric_slot(program, dst) &&
                (arith == OpCode::ADD_NUM ||
                 arith == OpCode::SUB_NUM ||
                 arith == OpCode::MUL_NUM)) {
                if (arith == OpCode::ADD_NUM) first.code = OpCode::ADD_VV_STORE;
                else if (arith == OpCode::SUB_NUM) first.code = OpCode::SUB_VV_STORE;
                else first.code = OpCode::MUL_VV_STORE;

                first.a = dst;
                first.b = (lhs & 0xff) | ((rhs & 0xff) << 8);
                first.s = 0;
                first.flags = 0;
                i += 3;
                continue;
            }
        }

        // PUSH_NUM c, LOAD v, MUL or LOAD v, PUSH_NUM c, MUL.
        // Multiplication is commutative, so both map to the same opcode.
        if (tail_is_straight(i, 3) &&
            program.code[i + 2].code == OpCode::MUL_NUM) {
            int constant = -1;
            int slot = -1;

            if (first.code == OpCode::PUSH_NUM &&
                program.code[i + 1].code == OpCode::LOAD_NUM) {
                constant = first.a;
                slot = program.code[i + 1].a;
            } else if (first.code == OpCode::LOAD_NUM &&
                       program.code[i + 1].code == OpCode::PUSH_NUM) {
                slot = first.a;
                constant = program.code[i + 1].a;
            }

            if (constant >= 0 &&
                static_cast<std::size_t>(constant) < program.number_count &&
                numeric_slot(program, slot)) {
                first.code = OpCode::MUL_CV_PUSH;
                first.a = constant;
                first.b = slot;
                first.s = 0;
                first.flags = 0;
                i += 2;
                continue;
            }
        }

        // LOAD lhs, LOAD rhs, ADD/SUB/MUL -> direct push of the result.
        if (tail_is_straight(i, 3) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM) {
            const int lhs = first.a;
            const int rhs = program.code[i + 1].a;
            const OpCode arith = program.code[i + 2].code;

            if (numeric_slot(program, lhs) &&
                numeric_slot(program, rhs) &&
                (arith == OpCode::ADD_NUM ||
                 arith == OpCode::SUB_NUM ||
                 arith == OpCode::MUL_NUM)) {
                if (arith == OpCode::ADD_NUM) first.code = OpCode::ADD_VV_PUSH;
                else if (arith == OpCode::SUB_NUM) first.code = OpCode::SUB_VV_PUSH;
                else first.code = OpCode::MUL_VV_PUSH;

                first.a = lhs;
                first.b = rhs;
                first.s = 0;
                first.flags = 0;
                i += 2;
                continue;
            }
        }

        // LOAD src, STORE dst -> direct numeric move.
        if (tail_is_straight(i, 2) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::STORE_NUM) {
            const int src = first.a;
            const int dst = program.code[i + 1].a;

            if (numeric_slot(program, src) && numeric_slot(program, dst)) {
                first.code = OpCode::MOV_NUM;
                first.a = dst;
                first.b = src;
                first.s = 0;
                first.flags = 0;
                ++i;
            }
        }
    }

    // Stage 4A cross-line fusion.
    //
    // Stage 3 intentionally stops at BASIC line boundaries so every line
    // remains a valid GOTO destination. Here we only replace the first line's
    // opcode and leave every following line-start opcode intact. Normal
    // fall-through executes the combined operation and skips the covered
    // slots; a GOTO into any covered BASIC line still executes its original
    // Stage-3 opcode, preserving classic BASIC control-flow semantics.
    //
    // Recognized core:
    //   T = X*X - Y*Y + CR
    //   Y = 2*X*Y + CI
    //   X = T
    //   [optional scalar move, e.g. I=J]
    //   IF X*X + Y*Y > C ...      or
    //   IF X*X > C OR Y*Y > C ...
    for (std::size_t i = 0; i < program.code_count; ++i) {
        if (i + 20 >= program.code_count) break;

        Op& first = program.code[i];
        if (first.code != OpCode::MULSUBADD_VVVVV_STORE) {
            continue;
        }

        const std::uint32_t p0 =
            static_cast<std::uint32_t>(first.a);
        const int temp = static_cast<int>(p0 & 0x3fu);
        const int x0 = static_cast<int>((p0 >> 6) & 0x3fu);
        const int x1 = static_cast<int>((p0 >> 12) & 0x3fu);
        const int y0 = static_cast<int>((p0 >> 18) & 0x3fu);
        const int y1 = static_cast<int>((p0 >> 24) & 0x3fu);
        const int real_add = first.b & 0x3f;

        // MULSUBADD covers 10 original IL slots.
        const std::size_t second_pc = i + 10;
        if (second_pc >= program.code_count ||
            program.code[second_pc].code !=
                OpCode::MUL_CVV_ADD_V_STORE) {
            continue;
        }

        const Op& second = program.code[second_pc];
        const std::uint32_t p1 =
            static_cast<std::uint32_t>(second.a);
        const int y_dst = static_cast<int>(p1 & 0x3fu);
        const int x2 = static_cast<int>((p1 >> 6) & 0x3fu);
        const int y2 = static_cast<int>((p1 >> 12) & 0x3fu);
        const int imag_add = static_cast<int>((p1 >> 18) & 0x3fu);

        if (second.b < 0 ||
            static_cast<std::size_t>(second.b) >= program.number_count ||
            program.number_pool[second.b] != 2.0) {
            continue;
        }

        // MUL_CVV_ADD covers 8 slots, then X=T must be the direct move.
        const std::size_t move_x_pc = i + 18;
        if (move_x_pc >= program.code_count ||
            program.code[move_x_pc].code != OpCode::MOV_NUM) {
            continue;
        }

        const Op& move_x = program.code[move_x_pc];

        if (x0 != x1 ||
            y0 != y1 ||
            y_dst != y0 ||
            x2 != x0 ||
            y2 != y0 ||
            move_x.a != x0 ||
            move_x.b != temp) {
            continue;
        }

        if (!numeric_slot(program, temp) ||
            !numeric_slot(program, x0) ||
            !numeric_slot(program, y0) ||
            !numeric_slot(program, real_add) ||
            !numeric_slot(program, imag_add)) {
            continue;
        }

        std::size_t condition_pc = i + 20;
        bool has_extra_move = false;
        int move_dst = 0;
        int move_src = 0;

        if (condition_pc < program.code_count &&
            program.code[condition_pc].code == OpCode::MOV_NUM) {
            const Op& move = program.code[condition_pc];
            if (!numeric_slot(program, move.a) ||
                !numeric_slot(program, move.b)) {
                continue;
            }
            has_extra_move = true;
            move_dst = move.a;
            move_src = move.b;
            condition_pc += 2; // MOV_NUM covers LOAD+STORE.
        }

        if (condition_pc >= program.code_count) continue;

        const Op& condition = program.code[condition_pc];
        bool or_escape = false;
        std::size_t condition_slots = 0;

        if (condition.code == OpCode::SQ2_GT_CONST_OR_JZ) {
            or_escape = true;
            condition_slots = 12;
        } else if (condition.code == OpCode::SUMSQ_GT_CONST_JZ) {
            condition_slots = 10;
        } else {
            continue;
        }

        const int cond_x = condition.s & 0x3f;
        const int cond_y = (condition.s >> 6) & 0x3f;
        if (cond_x != x0 || cond_y != y0) {
            continue;
        }

        if (condition.b < 0 ||
            static_cast<std::size_t>(condition.b) >= program.number_count ||
            condition.a < 0 ||
            static_cast<std::size_t>(condition.a) >= program.code_count) {
            continue;
        }

        // 12-bit packing is comfortably above current kMaxOps/number-pool
        // limits, but keep the format self-checking.
        if (condition.a > 0x0fff || condition.b > 0x0fff) {
            continue;
        }

        first.code = or_escape
            ? OpCode::COMPLEX_ITER_OR_JZ
            : OpCode::COMPLEX_ITER_SUMSQ_JZ;

        first.a =
            (x0 & 0x3f) |
            ((y0 & 0x3f) << 6) |
            ((real_add & 0x3f) << 12) |
            ((imag_add & 0x3f) << 18) |
            ((temp & 0x3f) << 24);

        first.b =
            (condition.a & 0x0fff) |
            ((condition.b & 0x0fff) << 12);

        first.s = static_cast<std::uint16_t>(
            (move_dst & 0x3f) |
            ((move_src & 0x3f) << 6)
        );

        const bool fuse_next =
            static_cast<std::size_t>(condition.a) < program.code_count &&
            program.code[condition.a].code == OpCode::FOR_INCR;

        first.flags =
            (has_extra_move ? 1u : 0u) |
            (fuse_next ? 2u : 0u);

        const std::size_t total_slots =
            20 + (has_extra_move ? 2 : 0) + condition_slots;

        // All covered opcodes remain intact for direct GOTO entry.
        i += total_slots - 1;
    }

    // Stage 4C cross-line graphics fusion. As with Stage 4A, covered BASIC
    // line starts stay intact so direct GOTO entry preserves normal semantics.
    for (std::size_t i = 0; i < program.code_count; ++i) {
        Op& first = program.code[i];

        // G=INT(A*B) / COLOR G,G,G / PSET X,Y
        // Raw slot layout remains 5 + 4 + 3 = 12 slots even when the first
        // multiplication has already become MUL_VV_PUSH.
        if (i + 11 < program.code_count) {
            int lhs = -1;
            int rhs = -1;

            if (first.code == OpCode::MUL_VV_PUSH) {
                lhs = first.a;
                rhs = first.b;
            } else if (first.code == OpCode::LOAD_NUM &&
                       program.code[i + 1].code == OpCode::LOAD_NUM &&
                       program.code[i + 2].code == OpCode::MUL_NUM) {
                lhs = first.a;
                rhs = program.code[i + 1].a;
            }

            if (lhs >= 0 &&
                program.code[i + 3].code == OpCode::CALLFN &&
                program.code[i + 3].a == FnId::INT &&
                program.code[i + 3].b == 1 &&
                program.code[i + 4].code == OpCode::STORE_NUM) {
                const int gray = program.code[i + 4].a;

                if (program.code[i + 5].code == OpCode::LOAD_NUM &&
                    program.code[i + 6].code == OpCode::LOAD_NUM &&
                    program.code[i + 7].code == OpCode::LOAD_NUM &&
                    program.code[i + 5].a == gray &&
                    program.code[i + 6].a == gray &&
                    program.code[i + 7].a == gray &&
                    program.code[i + 8].code == OpCode::CALLFN &&
                    program.code[i + 8].a == FnId::GCOLOR &&
                    program.code[i + 8].b == 3 &&
                    program.code[i + 9].code == OpCode::LOAD_NUM &&
                    program.code[i + 10].code == OpCode::LOAD_NUM &&
                    program.code[i + 11].code == OpCode::CALLFN &&
                    program.code[i + 11].a == FnId::GPSET &&
                    program.code[i + 11].b == 2) {
                    const int x = program.code[i + 9].a;
                    const int y = program.code[i + 10].a;

                    if (numeric_slot(program, lhs) &&
                        numeric_slot(program, rhs) &&
                        numeric_slot(program, gray) &&
                        numeric_slot(program, x) &&
                        numeric_slot(program, y)) {
                        first.code = OpCode::GRAY_PSET_MUL_INT;
                        first.a =
                            (gray & 0x3f) |
                            ((lhs & 0x3f) << 6) |
                            ((rhs & 0x3f) << 12) |
                            ((x & 0x3f) << 18) |
                            ((y & 0x3f) << 24);
                        first.b = 0;
                        first.s = 0;
                        first.flags = 0;
                        i += 11;
                        continue;
                    }
                }
            }
        }

        // R=MIN(CAP,I*KR), G=MIN(CAP,I*KG), B=MIN(CAP,I*KB),
        // COLOR R,G,B, PSET X,Y.
        // Each color assignment occupies six original IL slots.
        if (i + 24 < program.code_count) {
            int source_slot = -1;
            int dst[3] = {-1, -1, -1};
            int mul[3] = {0, 0, 0};
            int cap = -1;
            bool ok = true;

            for (int channel = 0; channel < 3 && ok; ++channel) {
                const std::size_t p = i + static_cast<std::size_t>(channel * 6);

                if (program.code[p].code != OpCode::PUSH_NUM ||
                    program.code[p].a < 0 ||
                    static_cast<std::size_t>(program.code[p].a) >=
                        program.number_count) {
                    ok = false;
                    break;
                }

                const BasicNumber cap_value =
                    program.number_pool[program.code[p].a];
                const int cap_int = static_cast<int>(cap_value);
                if (cap_value != static_cast<BasicNumber>(cap_int) ||
                    cap_int < 0 || cap_int > 255) {
                    ok = false;
                    break;
                }

                if (channel == 0) cap = cap_int;
                else if (cap != cap_int) {
                    ok = false;
                    break;
                }

                int src = -1;
                int mul_index = -1;

                if (program.code[p + 1].code == OpCode::MUL_CV_PUSH) {
                    mul_index = program.code[p + 1].a;
                    src = program.code[p + 1].b;
                } else if (program.code[p + 1].code == OpCode::LOAD_NUM &&
                           program.code[p + 2].code == OpCode::PUSH_NUM &&
                           program.code[p + 3].code == OpCode::MUL_NUM) {
                    src = program.code[p + 1].a;
                    mul_index = program.code[p + 2].a;
                } else {
                    ok = false;
                    break;
                }

                if (mul_index < 0 ||
                    static_cast<std::size_t>(mul_index) >=
                        program.number_count) {
                    ok = false;
                    break;
                }

                const BasicNumber mul_value = program.number_pool[mul_index];
                const int mul_int = static_cast<int>(mul_value);
                if (mul_value != static_cast<BasicNumber>(mul_int) ||
                    mul_int < 0 || mul_int > 255) {
                    ok = false;
                    break;
                }

                if (channel == 0) source_slot = src;
                else if (source_slot != src) {
                    ok = false;
                    break;
                }

                if (program.code[p + 4].code != OpCode::CALLFN ||
                    program.code[p + 4].a != FnId::MIN ||
                    program.code[p + 4].b != 2 ||
                    program.code[p + 5].code != OpCode::STORE_NUM) {
                    ok = false;
                    break;
                }

                dst[channel] = program.code[p + 5].a;
                mul[channel] = mul_int;
            }

            if (ok) {
                const std::size_t color_pc = i + 18;
                const std::size_t pset_pc = i + 22;

                ok =
                    program.code[color_pc].code == OpCode::LOAD_NUM &&
                    program.code[color_pc + 1].code == OpCode::LOAD_NUM &&
                    program.code[color_pc + 2].code == OpCode::LOAD_NUM &&
                    program.code[color_pc].a == dst[0] &&
                    program.code[color_pc + 1].a == dst[1] &&
                    program.code[color_pc + 2].a == dst[2] &&
                    program.code[color_pc + 3].code == OpCode::CALLFN &&
                    program.code[color_pc + 3].a == FnId::GCOLOR &&
                    program.code[color_pc + 3].b == 3 &&
                    program.code[pset_pc].code == OpCode::LOAD_NUM &&
                    program.code[pset_pc + 1].code == OpCode::LOAD_NUM &&
                    program.code[pset_pc + 2].code == OpCode::CALLFN &&
                    program.code[pset_pc + 2].a == FnId::GPSET &&
                    program.code[pset_pc + 2].b == 2;
            }

            if (ok) {
                const int x = program.code[i + 22].a;
                const int y = program.code[i + 23].a;

                if (numeric_slot(program, source_slot) &&
                    numeric_slot(program, dst[0]) &&
                    numeric_slot(program, dst[1]) &&
                    numeric_slot(program, dst[2]) &&
                    numeric_slot(program, x) &&
                    numeric_slot(program, y)) {
                    first.code = OpCode::RGB_PSET_MINMUL;
                    first.a =
                        (source_slot & 0x3f) |
                        ((dst[0] & 0x3f) << 6) |
                        ((dst[1] & 0x3f) << 12) |
                        ((dst[2] & 0x3f) << 18) |
                        ((x & 0x3f) << 24);
                    first.b =
                        (mul[0] & 0xff) |
                        ((mul[1] & 0xff) << 8) |
                        ((mul[2] & 0xff) << 16) |
                        ((cap & 0xff) << 24);
                    first.s = static_cast<std::uint16_t>(y & 0x3f);
                    first.flags = 0;
                    i += 24;
                    continue;
                }
            }
        }
    }

    // Stage 5: specialize remaining purely numeric function calls after the
    // cross-line pixel fusions have had a chance to consume INT/MIN patterns.
    for (std::size_t i = 0; i < program.code_count; ++i) {
        Op& op = program.code[i];
        if (op.code != OpCode::CALLFN) continue;

        switch (op.a) {
            case FnId::PI:
            case FnId::RND:
            case FnId::TIMER:
                if (op.b == 0) op.code = OpCode::FN0_NUM;
                break;

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
            case FnId::SGN:
            case FnId::RNDI:
                if (op.b == 1) op.code = OpCode::FN1_NUM;
                break;

            case FnId::MIN:
            case FnId::MAX:
                if (op.b == 2) op.code = OpCode::FN2_NUM;
                break;

            case FnId::CLAMP:
                if (op.b == 3) op.code = OpCode::FN3_NUM;
                break;

            default:
                break;
        }
    }

    // Stage 4C fallback after Stage 5.
    // If the initial cross-line pass missed the grayscale path, INT has now
    // become FN1_NUM. Re-match the same 12-slot pixel pipeline here so the
    // specialization still collapses to one dispatch.
    for (std::size_t i = 0; i < program.code_count; ++i) {
        if (i + 11 >= program.code_count) break;

        Op& first = program.code[i];

        int lhs = -1;
        int rhs = -1;

        if (first.code == OpCode::MUL_VV_PUSH) {
            lhs = first.a;
            rhs = first.b;
        } else if (first.code == OpCode::LOAD_NUM &&
                   program.code[i + 1].code == OpCode::LOAD_NUM &&
                   program.code[i + 2].code == OpCode::MUL_NUM) {
            lhs = first.a;
            rhs = program.code[i + 1].a;
        } else {
            continue;
        }

        const Op& int_op = program.code[i + 3];
        const bool is_int =
            (int_op.code == OpCode::CALLFN &&
             int_op.a == FnId::INT &&
             int_op.b == 1) ||
            (int_op.code == OpCode::FN1_NUM &&
             int_op.a == FnId::INT);

        if (!is_int ||
            program.code[i + 4].code != OpCode::STORE_NUM) {
            continue;
        }

        const int gray = program.code[i + 4].a;

        if (program.code[i + 5].code != OpCode::LOAD_NUM ||
            program.code[i + 6].code != OpCode::LOAD_NUM ||
            program.code[i + 7].code != OpCode::LOAD_NUM ||
            program.code[i + 5].a != gray ||
            program.code[i + 6].a != gray ||
            program.code[i + 7].a != gray ||
            program.code[i + 8].code != OpCode::CALLFN ||
            program.code[i + 8].a != FnId::GCOLOR ||
            program.code[i + 8].b != 3 ||
            program.code[i + 9].code != OpCode::LOAD_NUM ||
            program.code[i + 10].code != OpCode::LOAD_NUM ||
            program.code[i + 11].code != OpCode::CALLFN ||
            program.code[i + 11].a != FnId::GPSET ||
            program.code[i + 11].b != 2) {
            continue;
        }

        const int x = program.code[i + 9].a;
        const int y = program.code[i + 10].a;

        if (!numeric_slot(program, lhs) ||
            !numeric_slot(program, rhs) ||
            !numeric_slot(program, gray) ||
            !numeric_slot(program, x) ||
            !numeric_slot(program, y)) {
            continue;
        }

        first.code = OpCode::GRAY_PSET_MUL_INT;
        first.a =
            (gray & 0x3f) |
            ((lhs & 0x3f) << 6) |
            ((rhs & 0x3f) << 12) |
            ((x & 0x3f) << 18) |
            ((y & 0x3f) << 24);
        first.b = 0;
        first.s = 0;
        first.flags = 0;

        i += 11;
    }

    // Stage 4C robust fallback: once Stage 5 has specialized INT as
    // FN1_NUM, consume the already-computed numeric TOS and fuse the remainder
    // of G=INT(...), COLOR G,G,G, PSET X,Y. This is independent of how the
    // preceding multiplication was represented.
    for (std::size_t i = 0; i < program.code_count; ++i) {
        if (i + 8 >= program.code_count) break;

        Op& first = program.code[i];
        if (first.code != OpCode::FN1_NUM ||
            first.a != FnId::INT ||
            program.code[i + 1].code != OpCode::STORE_NUM) {
            continue;
        }

        const int gray = program.code[i + 1].a;

        if (program.code[i + 2].code != OpCode::LOAD_NUM ||
            program.code[i + 3].code != OpCode::LOAD_NUM ||
            program.code[i + 4].code != OpCode::LOAD_NUM ||
            program.code[i + 2].a != gray ||
            program.code[i + 3].a != gray ||
            program.code[i + 4].a != gray ||
            program.code[i + 5].code != OpCode::CALLFN ||
            program.code[i + 5].a != FnId::GCOLOR ||
            program.code[i + 5].b != 3 ||
            program.code[i + 6].code != OpCode::LOAD_NUM ||
            program.code[i + 7].code != OpCode::LOAD_NUM ||
            program.code[i + 8].code != OpCode::CALLFN ||
            program.code[i + 8].a != FnId::GPSET ||
            program.code[i + 8].b != 2) {
            continue;
        }

        const int x = program.code[i + 6].a;
        const int y = program.code[i + 7].a;

        if (!numeric_slot(program, gray) ||
            !numeric_slot(program, x) ||
            !numeric_slot(program, y)) {
            continue;
        }

        first.code = OpCode::GRAY_PSET_INT_STACK;
        first.a =
            (gray & 0x3f) |
            ((x & 0x3f) << 6) |
            ((y & 0x3f) << 12);
        first.b = 0;
        first.s = 0;
        first.flags = 0;

        i += 8;
    }

    // Stage 8: a late residual pass catches simple numeric patterns that were
    // skipped by an earlier fusion or exposed by the cross-line passes.
    for (std::size_t i = 0; i < program.code_count; ++i) {
        Op& first = program.code[i];

        // Retry A + B*C -> DST after the earlier passes.
        if (tail_is_straight(i, 6) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::LOAD_NUM &&
            program.code[i + 2].code == OpCode::LOAD_NUM &&
            program.code[i + 3].code == OpCode::MUL_NUM &&
            program.code[i + 4].code == OpCode::ADD_NUM &&
            program.code[i + 5].code == OpCode::STORE_NUM) {
            const int addend = first.a;
            const int lhs = program.code[i + 1].a;
            const int rhs = program.code[i + 2].a;
            const int dst = program.code[i + 5].a;

            if (numeric_slot(program, addend) &&
                numeric_slot(program, lhs) &&
                numeric_slot(program, rhs) &&
                numeric_slot(program, dst)) {
                first.code = OpCode::MULADD_VVV_STORE;
                first.a =
                    (dst & 0x3f) |
                    ((addend & 0x3f) << 6) |
                    ((lhs & 0x3f) << 12) |
                    ((rhs & 0x3f) << 18);
                first.b = 0;
                first.s = 0;
                first.flags = 0;
                i += 5;
                continue;
            }
        }

        // Constant assignment.
        if (tail_is_straight(i, 2) &&
            first.code == OpCode::PUSH_NUM &&
            program.code[i + 1].code == OpCode::STORE_NUM &&
            first.a >= 0 &&
            static_cast<std::size_t>(first.a) < program.number_count &&
            numeric_slot(program, program.code[i + 1].a)) {
            first.code = OpCode::STORE_CONST_NUM;
            first.b = first.a;
            first.a = program.code[i + 1].a;
            first.s = 0;
            first.flags = 0;
            ++i;
            continue;
        }

        // Variable op constant. The VM can also consume a following STORE_NUM.
        if (tail_is_straight(i, 3) &&
            first.code == OpCode::LOAD_NUM &&
            program.code[i + 1].code == OpCode::PUSH_NUM &&
            program.code[i + 1].a >= 0 &&
            static_cast<std::size_t>(program.code[i + 1].a) <
                program.number_count &&
            numeric_slot(program, first.a)) {
            OpCode fused = OpCode::HALT;
            switch (program.code[i + 2].code) {
                case OpCode::ADD_NUM:
                    fused = OpCode::ADD_VC_PUSH;
                    break;
                case OpCode::SUB_NUM:
                    fused = OpCode::SUB_VC_PUSH;
                    break;
                case OpCode::DIV_NUM:
                    fused = OpCode::DIV_VC_PUSH;
                    break;
                case OpCode::MUL_NUM:
                    fused = OpCode::MUL_CV_PUSH;
                    break;
                default:
                    break;
            }

            if (fused != OpCode::HALT) {
                const int slot = first.a;
                const int constant = program.code[i + 1].a;
                first.code = fused;
                if (fused == OpCode::MUL_CV_PUSH) {
                    first.a = constant;
                    first.b = slot;
                } else {
                    first.a = slot;
                    first.b = constant;
                }
                first.s = 0;
                first.flags = 0;
                i += 2;
                continue;
            }
        }
    }
}

struct Parser {
    const char* p = nullptr;
    CompiledProgram* out = nullptr;
    CompileControl* control = nullptr;
    std::int32_t line = 0;
    char error[96] = {};

    static char upper(char c) {
        return (c >= 'a' && c <= 'z')
            ? static_cast<char>(c - 'a' + 'A')
            : c;
    }

    void set_error(const char* message) {
        if (error[0] == '\0') {
            std::snprintf(error, sizeof(error), "%s", message);
        }
    }

    void skip_spaces() {
        while (*p == ' ' || *p == '\t') ++p;
    }

    bool match_char(char c) {
        skip_spaces();
        if (*p != c) return false;
        ++p;
        return true;
    }

    bool expect_char(char c) {
        if (match_char(c)) return true;
        set_error("SYNTAX ERROR");
        return false;
    }

    bool match_word(const char* word) {
        skip_spaces();

        const char* q = p;
        const char* w = word;

        while (*w && *q && upper(*q) == *w) {
            ++q;
            ++w;
        }

        if (*w != '\0') return false;

        if (std::isalnum(static_cast<unsigned char>(*q)) ||
            *q == '_' || *q == '$') {
            return false;
        }

        p = q;
        return true;
    }

    bool peek_word(const char* word) {
        const char* save = p;
        const bool ok = match_word(word);
        p = save;
        return ok;
    }

    bool parse_identifier(char* name, std::size_t capacity) {
        skip_spaces();

        if (!(std::isalpha(static_cast<unsigned char>(*p)) || *p == '_')) {
            return false;
        }

        std::size_t n = 0;
        while (std::isalnum(static_cast<unsigned char>(*p)) ||
               *p == '_' || *p == '$') {
            if (n + 1 < capacity) name[n++] = upper(*p);
            ++p;
        }

        name[n] = '\0';
        return n != 0;
    }

    bool emit(
        OpCode code,
        std::int32_t a = 0,
        std::int32_t b = 0,
        BasicNumber d = 0.0,
        std::uint16_t s = 0
    ) {
        Op op;
        op.code = code;
        op.a = a;
        op.b = b;
        op.s = s;

        if (code == OpCode::PUSH_NUM) {
            const std::int32_t index = out->intern_number(d);
            if (index < 0) {
                set_error("NUMBER POOL FULL");
                return false;
            }
            op.a = index;
        }

        if (!out->emit(op)) {
            set_error("PROGRAM TOO COMPLEX");
            return false;
        }
        return true;
    }

    bool emit_line_jump(OpCode code, std::int32_t target) {
        // b=1 identifies a BASIC line target that is resolved after compile.
        return emit(code, target, 1);
    }

    bool parse_line_target(std::int32_t& target) {
        skip_spaces();
        char* end = nullptr;
        const long value = std::strtol(p, &end, 10);

        if (end == p || value < 0 || value > INT32_MAX) return false;

        p = end;
        target = static_cast<std::int32_t>(value);
        return true;
    }

    ExprType parse_expression() {
        return parse_or();
    }

    ExprType parse_or() {
        ExprType type = parse_and();
        if (type == ExprType::Invalid) return type;

        while (match_word("OR")) {
            ExprType rhs = parse_and();
            if (type != ExprType::Number || rhs != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }
            if (!emit(OpCode::OR_NUM)) return ExprType::Invalid;
            type = ExprType::Number;
        }

        return type;
    }

    ExprType parse_and() {
        ExprType type = parse_relation();
        if (type == ExprType::Invalid) return type;

        while (match_word("AND")) {
            ExprType rhs = parse_relation();
            if (type != ExprType::Number || rhs != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }
            if (!emit(OpCode::AND_NUM)) return ExprType::Invalid;
            type = ExprType::Number;
        }

        return type;
    }

    ExprType parse_relation() {
        ExprType left = parse_add_sub();
        if (left == ExprType::Invalid) return left;

        skip_spaces();

        OpCode op = OpCode::HALT;
        bool found = true;

        if (p[0] == '<' && p[1] == '>') {
            op = OpCode::CNE; p += 2;
        } else if (p[0] == '<' && p[1] == '=') {
            op = OpCode::CLE; p += 2;
        } else if (p[0] == '>' && p[1] == '=') {
            op = OpCode::CGE; p += 2;
        } else if (p[0] == '=') {
            op = OpCode::CEQ; ++p;
        } else if (p[0] == '<') {
            op = OpCode::CLT; ++p;
        } else if (p[0] == '>') {
            op = OpCode::CGT; ++p;
        } else {
            found = false;
        }

        if (!found) return left;

        ExprType right = parse_add_sub();
        if (right == ExprType::Invalid) return right;

        if (left == ExprType::Number && right == ExprType::Number) {
            op = numeric_compare_opcode(op);
        }

        if (!emit(op)) return ExprType::Invalid;
        return ExprType::Number;
    }

    ExprType parse_add_sub() {
        ExprType type = parse_mul_div();
        if (type == ExprType::Invalid) return type;

        while (true) {
            skip_spaces();
            const char op = *p;
            if (op != '+' && op != '-') break;
            ++p;

            ExprType rhs = parse_mul_div();
            if (rhs == ExprType::Invalid) return rhs;

            if (op == '+') {
                if (type == ExprType::Number && rhs == ExprType::Number) {
                    if (!emit(OpCode::ADD_NUM)) return ExprType::Invalid;
                    type = ExprType::Number;
                } else {
                    if (!emit(OpCode::ADD)) return ExprType::Invalid;
                    type = ExprType::String;
                }
            } else {
                if (type != ExprType::Number || rhs != ExprType::Number) {
                    set_error("TYPE MISMATCH");
                    return ExprType::Invalid;
                }
                if (!emit(OpCode::SUB_NUM)) return ExprType::Invalid;
            }
        }

        return type;
    }

    ExprType parse_mul_div() {
        ExprType type = parse_pow();
        if (type == ExprType::Invalid) return type;

        while (true) {
            skip_spaces();

            OpCode op = OpCode::HALT;
            bool found = true;

            if (*p == '*') {
                op = OpCode::MUL; ++p;
            } else if (*p == '/') {
                op = OpCode::DIV; ++p;
            } else if (match_word("MOD")) {
                op = OpCode::MOD;
            } else {
                found = false;
            }

            if (!found) break;

            ExprType rhs = parse_pow();
            if (type != ExprType::Number || rhs != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }

            if (op == OpCode::MUL) op = OpCode::MUL_NUM;
            else if (op == OpCode::DIV) op = OpCode::DIV_NUM;
            else if (op == OpCode::MOD) op = OpCode::MOD_NUM;

            if (!emit(op)) return ExprType::Invalid;
        }

        return type;
    }

    ExprType parse_pow() {
        ExprType type = parse_unary();
        if (type == ExprType::Invalid) return type;

        while (true) {
            skip_spaces();
            if (*p != '^') break;
            ++p;

            ExprType rhs = parse_unary();
            if (type != ExprType::Number || rhs != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }

            if (!emit(OpCode::POW_NUM)) return ExprType::Invalid;
        }

        return type;
    }

    ExprType parse_unary() {
        skip_spaces();

        if (*p == '+') {
            ++p;
            return parse_unary();
        }

        if (*p == '-') {
            ++p;
            ExprType type = parse_unary();
            if (type != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }
            if (!emit(OpCode::NEG_NUM)) return ExprType::Invalid;
            return ExprType::Number;
        }

        if (match_word("NOT")) {
            ExprType type = parse_unary();
            if (type != ExprType::Number) {
                set_error("TYPE MISMATCH");
                return ExprType::Invalid;
            }
            if (!emit(OpCode::NOT_NUM)) return ExprType::Invalid;
            return ExprType::Number;
        }

        return parse_primary();
    }

    ExprType parse_function_call(int id, bool parenthesized) {
        int argc = 0;

        if (parenthesized) {
            if (!match_char(')')) {
                while (true) {
                    if (parse_expression() == ExprType::Invalid) {
                        return ExprType::Invalid;
                    }
                    ++argc;

                    if (match_char(',')) continue;
                    if (!expect_char(')')) return ExprType::Invalid;
                    break;
                }
            }
        } else if (!zero_arg_function(id)) {
            set_error("FUNCTION REQUIRES ()");
            return ExprType::Invalid;
        }

        if (!emit(OpCode::CALLFN, id, argc)) {
            return ExprType::Invalid;
        }

        return function_returns_string(id)
            ? ExprType::String
            : ExprType::Number;
    }

    ExprType parse_primary() {
        skip_spaces();

        if (*p == '(') {
            ++p;
            ExprType type = parse_expression();
            if (!expect_char(')')) return ExprType::Invalid;
            return type;
        }

        if (*p == '"') {
            ++p;

            char text[192] = {};
            std::size_t n = 0;

            while (*p && *p != '"') {
                if (n + 1 < sizeof(text)) text[n++] = *p;
                ++p;
            }

            if (*p != '"') {
                set_error("UNTERMINATED STRING");
                return ExprType::Invalid;
            }

            ++p;
            text[n] = '\0';

            const std::uint16_t offset = out->intern_string(text);
            if (offset == 0xffffu) {
                set_error("STRING POOL FULL");
                return ExprType::Invalid;
            }

            if (!emit(OpCode::PUSH_STR, 0, 0, 0.0, offset)) {
                return ExprType::Invalid;
            }

            return ExprType::String;
        }

        if (p[0] == '&' && (p[1] == 'H' || p[1] == 'h')) {
            char* end = nullptr;
            const long value = std::strtol(p + 2, &end, 16);
            if (end == p + 2 || value < 0 || value > 0x7fffffffL) {
                set_error("BAD NUMBER");
                return ExprType::Invalid;
            }
            p = end;
            if (!emit(OpCode::PUSH_NUM, 0, 0, static_cast<BasicNumber>(value))) {
                return ExprType::Invalid;
            }
            return ExprType::Number;
        }

        if (std::isdigit(static_cast<unsigned char>(*p)) ||
            (*p == '.' && std::isdigit(static_cast<unsigned char>(p[1])))) {
            char* end = nullptr;
            const BasicNumber value = std::strtof(p, &end);

            if (end == p) {
                set_error("BAD NUMBER");
                return ExprType::Invalid;
            }

            p = end;

            if (!emit(OpCode::PUSH_NUM, 0, 0, value)) {
                return ExprType::Invalid;
            }

            return ExprType::Number;
        }

        char name[kSymbolNameLength] = {};
        if (!parse_identifier(name, sizeof(name))) {
            set_error("EXPECTED EXPRESSION");
            return ExprType::Invalid;
        }

        const int fid = function_id(name);

        skip_spaces();
        if (*p == '(') {
            ++p;

            if (fid >= 0) {
                return parse_function_call(fid, true);
            }

            const int slot = out->find_or_add_symbol(name);
            if (slot < 0) {
                set_error("TOO MANY VARIABLES");
                return ExprType::Invalid;
            }
            int dims = 0;
            if (parse_expression() == ExprType::Invalid) return ExprType::Invalid;
            ++dims;

            if (match_char(',')) {
                if (parse_expression() == ExprType::Invalid) return ExprType::Invalid;
                ++dims;
            }

            if (!expect_char(')')) return ExprType::Invalid;

            if (!emit(OpCode::LOAD_ARR, slot, dims)) {
                return ExprType::Invalid;
            }
            return out->symbols[slot].is_string
                ? ExprType::String
                : ExprType::Number;
        }

        // Names such as MAX, MIN, ABS, etc. are valid BASIC
        // variable names when no argument list follows. Only true zero-arg
        // functions (RND, PI, TIMER) are recognized without parentheses.
        if (fid >= 0 && zero_arg_function(fid)) {
            return parse_function_call(fid, false);
        }

        const int slot = out->find_or_add_symbol(name);
        if (slot < 0) {
            set_error("TOO MANY VARIABLES");
            return ExprType::Invalid;
        }

        const bool is_string = out->symbols[slot].is_string;
        if (!emit(is_string ? OpCode::LOAD_STR : OpCode::LOAD_NUM, slot)) {
            return ExprType::Invalid;
        }

        return is_string ? ExprType::String : ExprType::Number;
    }

    bool compile_print() {
        skip_spaces();

        if (*p == '\0' || *p == ':') {
            return emit(OpCode::PRINT_NL);
        }

        bool trailing_semicolon = false;

        while (true) {
            ExprType type = parse_expression();
            if (type == ExprType::Invalid) return false;

            if (!emit(OpCode::PRINT)) return false;

            skip_spaces();

            if (*p == ';') {
                ++p;
                trailing_semicolon = true;
                skip_spaces();

                if (*p == '\0' || *p == ':' || peek_word("ELSE")) break;

                trailing_semicolon = false;
                continue;
            }

            if (*p == ',') {
                ++p;
                if (!emit(OpCode::PRINT_SPC)) return false;
                trailing_semicolon = false;
                continue;
            }

            break;
        }

        if (!trailing_semicolon) return emit(OpCode::PRINT_NL);
        return true;
    }

    bool compile_assignment() {
        char name[kSymbolNameLength] = {};
        if (!parse_identifier(name, sizeof(name))) {
            set_error("EXPECTED VARIABLE");
            return false;
        }

        const int slot = out->find_or_add_symbol(name);
        if (slot < 0) {
            set_error("TOO MANY VARIABLES");
            return false;
        }

        skip_spaces();

        int dims = 0;
        if (*p == '(') {
            ++p;

            if (parse_expression() == ExprType::Invalid) return false;
            ++dims;

            if (match_char(',')) {
                if (parse_expression() == ExprType::Invalid) return false;
                ++dims;
            }

            if (!expect_char(')')) return false;
        }

        if (!expect_char('=')) {
            set_error("EXPECTED =");
            return false;
        }

        const ExprType rhs = parse_expression();
        if (rhs == ExprType::Invalid) return false;

        if (dims != 0) {
            const ExprType wanted =
                out->symbols[slot].is_string
                    ? ExprType::String
                    : ExprType::Number;
            if (rhs != wanted) {
                set_error("TYPE MISMATCH");
                return false;
            }
            return emit(OpCode::STORE_ARR, slot, dims);
        }

        const ExprType wanted =
            out->symbols[slot].is_string ? ExprType::String : ExprType::Number;

        if (rhs != wanted) {
            set_error("TYPE MISMATCH");
            return false;
        }

        return emit(
            wanted == ExprType::String ? OpCode::STORE_STR : OpCode::STORE_NUM,
            slot
        );
    }

    bool compile_input() {
        skip_spaces();

        if (*p == '"') {
            if (parse_expression() != ExprType::String) return false;
            if (!emit(OpCode::PRINT)) return false;

            skip_spaces();
            if (*p == ';' || *p == ',') {
                ++p;
            } else {
                set_error("EXPECTED ; OR ,");
                return false;
            }
        } else {
            const std::uint16_t prompt = out->intern_string("? ");
            if (prompt == 0xffffu) {
                set_error("STRING POOL FULL");
                return false;
            }
            if (!emit(OpCode::PUSH_STR, 0, 0, 0.0, prompt) ||
                !emit(OpCode::PRINT)) {
                return false;
            }
        }

        char name[kSymbolNameLength] = {};
        if (!parse_identifier(name, sizeof(name))) {
            set_error("EXPECTED INPUT VARIABLE");
            return false;
        }

        const int slot = out->find_or_add_symbol(name);
        if (slot < 0) {
            set_error("TOO MANY VARIABLES");
            return false;
        }

        return emit(OpCode::CALLFN, FnId::INPUT, slot);
    }

    bool compile_dim() {
        while (true) {
            char name[kSymbolNameLength] = {};
            if (!parse_identifier(name, sizeof(name))) {
                set_error("EXPECTED ARRAY NAME");
                return false;
            }

            const int slot = out->find_or_add_symbol(name);
            if (slot < 0) {
                set_error("TOO MANY VARIABLES");
                return false;
            }
            if (!expect_char('(')) return false;

            int dims = 0;
            if (parse_expression() != ExprType::Number) {
                set_error("DIM SIZE MUST BE NUMERIC");
                return false;
            }
            ++dims;

            if (match_char(',')) {
                if (parse_expression() != ExprType::Number) {
                    set_error("DIM SIZE MUST BE NUMERIC");
                    return false;
                }
                ++dims;
            }

            if (!expect_char(')')) return false;

            if (!emit(OpCode::DIM_ARR, slot, dims)) return false;

            if (!match_char(',')) break;
        }

        return true;
    }

    bool compile_play() {
        if (match_word("STOP"))
            return emit(OpCode::CALLFN, FnId::PLAYSTOP, 0);
        if (match_word("PAUSE"))
            return emit(OpCode::CALLFN, FnId::PLAYPAUSE, 0);
        if (match_word("RESUME"))
            return emit(OpCode::CALLFN, FnId::PLAYRESUME, 0);
        if (match_word("WAIT"))
            return emit(OpCode::CALLFN, FnId::PLAYWAIT, 0);
        return compile_call_statement(FnId::PLAY);
    }

    bool compile_gdef() {
        if (match_word("CLEAR")) {
            return emit(OpCode::CALLFN, FnId::GDEF, 0);
        }

        // Parse the left side without consuming the GDEF assignment '=' as a
        // comparison operator. Expressions are still allowed on both sides.
        if (parse_add_sub() != ExprType::String) {
            set_error("GDEF CHARACTER MUST BE STRING");
            return false;
        }
        if (!expect_char('=')) return false;
        if (parse_expression() != ExprType::String) {
            set_error("GDEF DATA MUST BE STRING");
            return false;
        }
        return emit(OpCode::CALLFN, FnId::GDEF, 2);
    }

    bool compile_gpalette() {
        if (match_word("RESET")) {
            return emit(OpCode::CALLFN, FnId::GPALETTE, 0);
        }
        return compile_call_statement(FnId::GPALETTE);
    }

    bool compile_call_statement(int id) {
        int argc = 0;

        skip_spaces();

        if (*p == '(') {
            ++p;
            if (!match_char(')')) {
                while (true) {
                    if (parse_expression() == ExprType::Invalid) return false;
                    ++argc;
                    if (match_char(',')) continue;
                    if (!expect_char(')')) return false;
                    break;
                }
            }
        } else if (*p != '\0' && *p != ':') {
            while (true) {
                if (parse_expression() == ExprType::Invalid) return false;
                ++argc;
                if (!match_char(',')) break;
            }
        }

        return emit(OpCode::CALLFN, id, argc);
    }

    bool compile_line_graphics() {
        skip_spaces();

        bool shorthand = false;
        int argc = 0;

        if (*p == '-') {
            ++p;
            shorthand = true;

            if (!expect_char('(')) return false;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(',')) return false;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(')')) return false;
        } else if (*p == '(') {
            ++p;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(',')) return false;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(')')) return false;

            skip_spaces();
            if (*p != '-') {
                set_error("EXPECTED -");
                return false;
            }
            ++p;

            if (!expect_char('(')) return false;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(',')) return false;
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
            if (!expect_char(')')) return false;
        } else {
            for (int i = 0; i < 4; ++i) {
                if (parse_expression() == ExprType::Invalid) return false;
                ++argc;
                if (i != 3 && !expect_char(',')) return false;
            }
        }

        if (match_char(',')) {
            if (parse_expression() == ExprType::Invalid) return false;
            ++argc;
        }

        std::int32_t encoded = argc;
        if (shorthand) encoded |= (1 << 30);

        return emit(OpCode::CALLFN, FnId::GLINE, encoded);
    }

    bool compile_inline_branch() {
        skip_spaces();

        if (std::isdigit(static_cast<unsigned char>(*p))) {
            std::int32_t target = 0;
            if (!parse_line_target(target)) {
                set_error("BAD LINE TARGET");
                return false;
            }
            return emit_line_jump(OpCode::JMP, target);
        }

        return compile_statement();
    }

    bool compile_if() {
        if (parse_expression() != ExprType::Number) {
            set_error("IF CONDITION MUST BE NUMERIC");
            return false;
        }

        if (!match_word("THEN")) {
            set_error("EXPECTED THEN");
            return false;
        }

        const std::size_t jz_pos = out->code_count;
        if (!emit(OpCode::JZ, -1)) return false;

        if (!compile_inline_branch()) return false;

        skip_spaces();

        if (match_word("ELSE")) {
            const std::size_t end_jump = out->code_count;
            if (!emit(OpCode::JMP, -1, 0)) return false;

            out->code[jz_pos].a =
                static_cast<std::int32_t>(out->code_count);

            if (!compile_inline_branch()) return false;

            out->code[end_jump].a =
                static_cast<std::int32_t>(out->code_count);
        } else {
            out->code[jz_pos].a =
                static_cast<std::int32_t>(out->code_count);
        }

        return true;
    }

    bool compile_on_branch() {
        if (parse_expression() != ExprType::Number) {
            set_error("ON SELECTOR MUST BE NUMERIC");
            return false;
        }

        OpCode code = OpCode::HALT;
        if (match_word("GOTO")) {
            code = OpCode::ON_GOTO;
        } else if (match_word("GOSUB")) {
            code = OpCode::ON_GOSUB;
        } else {
            set_error("EXPECTED GOTO OR GOSUB");
            return false;
        }

        const std::size_t on_pc = out->code_count;
        if (!emit(code, 0)) return false;

        int count = 0;
        while (true) {
            std::int32_t target = 0;
            if (!parse_line_target(target)) {
                set_error("BAD ON TARGET");
                return false;
            }

            // Following JMP entries are a compact branch table. They are
            // resolved from BASIC line numbers after the complete program is
            // compiled, but are never dispatched directly on the normal path.
            if (!emit_line_jump(OpCode::JMP, target)) return false;
            ++count;

            if (!match_char(',')) break;
        }

        if (count <= 0) {
            set_error("ON REQUIRES TARGETS");
            return false;
        }

        out->code[on_pc].a = count;
        return true;
    }

    bool compile_for() {
        if (!control || control->for_depth >= 16) {
            set_error("FOR NESTING TOO DEEP");
            return false;
        }

        char name[kSymbolNameLength] = {};
        if (!parse_identifier(name, sizeof(name))) {
            set_error("EXPECTED FOR VARIABLE");
            return false;
        }

        const int slot = out->find_or_add_symbol(name);
        if (slot < 0 || out->symbols[slot].is_string) {
            set_error("BAD FOR VARIABLE");
            return false;
        }

        if (!expect_char('=')) return false;

        if (parse_expression() != ExprType::Number) return false;
        if (!emit(OpCode::STORE_NUM, slot)) return false;

        if (!match_word("TO")) {
            set_error("EXPECTED TO");
            return false;
        }

        if (parse_expression() != ExprType::Number) return false;

        if (match_word("STEP")) {
            if (parse_expression() != ExprType::Number) return false;
        } else {
            if (!emit(OpCode::PUSH_NUM, 0, 0, 1.0)) return false;
        }

        if (!emit(OpCode::FOR_INIT, slot)) return false;

        const std::size_t check_pc = out->code_count;
        if (!emit(OpCode::FOR_CHECK, slot, -1)) return false;

        const std::size_t skip_jump = out->code_count;
        if (!emit(OpCode::JMP, -1)) return false;

        out->code[check_pc].b =
            static_cast<std::int32_t>(out->code_count);

        control->for_stack[control->for_depth++] = {
            slot,
            skip_jump
        };

        return true;
    }

    bool compile_next() {
        if (!control || control->for_depth == 0) {
            set_error("NEXT WITHOUT FOR");
            return false;
        }

        int requested_slot = -1;
        const char* save = p;
        char name[kSymbolNameLength] = {};

        if (parse_identifier(name, sizeof(name))) {
            requested_slot = out->find_or_add_symbol(name);
        } else {
            p = save;
        }

        const auto entry =
            control->for_stack[control->for_depth - 1];

        if (requested_slot >= 0 && requested_slot != entry.slot) {
            set_error("NEXT VARIABLE MISMATCH");
            return false;
        }

        if (!emit(OpCode::FOR_INCR, requested_slot)) return false;

        out->code[entry.skip_jump].a =
            static_cast<std::int32_t>(out->code_count);

        --control->for_depth;
        return true;
    }

    bool compile_while() {
        if (!control || control->while_depth >= 16) {
            set_error("WHILE NESTING TOO DEEP");
            return false;
        }

        const std::size_t start_pc = out->code_count;

        if (parse_expression() != ExprType::Number) return false;

        const std::size_t jz_pc = out->code_count;
        if (!emit(OpCode::JZ, -1)) return false;

        control->while_stack[control->while_depth++] = {
            start_pc,
            jz_pc
        };

        return true;
    }

    bool compile_wend() {
        if (!control || control->while_depth == 0) {
            set_error("WEND WITHOUT WHILE");
            return false;
        }

        const auto entry =
            control->while_stack[--control->while_depth];

        if (!emit(
                OpCode::JMP,
                static_cast<std::int32_t>(entry.start_pc)
            )) {
            return false;
        }

        out->code[entry.jz_pc].a =
            static_cast<std::int32_t>(out->code_count);

        return true;
    }

    bool compile_do() {
        if (!control || control->do_depth >= 16) {
            set_error("DO NESTING TOO DEEP");
            return false;
        }

        control->do_stack[control->do_depth++] = out->code_count;
        return true;
    }

    bool compile_loop() {
        if (!control || control->do_depth == 0) {
            set_error("LOOP WITHOUT DO");
            return false;
        }

        const std::size_t start_pc =
            control->do_stack[--control->do_depth];

        if (match_word("UNTIL")) {
            if (parse_expression() != ExprType::Number) return false;
            return emit(
                OpCode::JZ,
                static_cast<std::int32_t>(start_pc)
            );
        }

        return emit(
            OpCode::JMP,
            static_cast<std::int32_t>(start_pc)
        );
    }

    bool compile_statement() {
        skip_spaces();

        if (*p == '\0') return true;

        if (*p == '\'') {
            p += std::strlen(p);
            return true;
        }

        const char* save = p;

        if (match_word("REM")) {
            p += std::strlen(p);
            return true;
        }

        if (match_word("PRINT")) return compile_print();
        if (match_word("INPUT")) return compile_input();
        if (match_word("LET")) return compile_assignment();
        if (match_word("IF")) return compile_if();
        if (match_word("FOR")) return compile_for();
        if (match_word("NEXT")) return compile_next();
        if (match_word("WHILE")) return compile_while();
        if (match_word("WEND")) return compile_wend();
        if (match_word("DO")) return compile_do();
        if (match_word("LOOP")) return compile_loop();
        if (match_word("DIM")) return compile_dim();
        if (match_word("ON")) return compile_on_branch();

        if (match_word("GOTO")) {
            std::int32_t target = 0;
            if (!parse_line_target(target)) {
                set_error("BAD GOTO TARGET");
                return false;
            }
            return emit_line_jump(OpCode::JMP, target);
        }

        if (match_word("GOSUB")) {
            std::int32_t target = 0;
            if (!parse_line_target(target)) {
                set_error("BAD GOSUB TARGET");
                return false;
            }
            return emit_line_jump(OpCode::GOSUB, target);
        }

        if (match_word("RETURN")) return emit(OpCode::RETSUB);

        if (match_word("SCREEN")) return compile_call_statement(FnId::SCREEN);
        if (match_word("CLS")) return compile_call_statement(FnId::GCLS);
        if (match_word("COLORHSV")) return compile_call_statement(FnId::GCOLORHSV);
        if (match_word("COLOR")) return compile_call_statement(FnId::GCOLOR);
        if (match_word("PSET")) return compile_call_statement(FnId::GPSET);
        if (match_word("LINE")) return compile_line_graphics();
        if (match_word("CIRCLE")) return compile_call_statement(FnId::GCIRCLE);
        if (match_word("BOX")) return compile_call_statement(FnId::GBOX);
        if (match_word("PAINT")) return compile_call_statement(FnId::GPAINT);
        if (match_word("FLUSH")) return compile_call_statement(FnId::GFLUSH);
        if (match_word("SLEEP")) return compile_call_statement(FnId::GSLEEP);
        if (match_word("PAUSE")) return compile_call_statement(FnId::PAUSE);
        if (match_word("BEEP")) return compile_call_statement(FnId::BEEP);
        if (match_word("PLAY")) return compile_play();
        if (match_word("WAVPLAY")) return compile_call_statement(FnId::WAVPLAY);
        if (match_word("WAVSTOP")) return emit(OpCode::CALLFN, FnId::WAVSTOP, 0);
        if (match_word("WAVPAUSE")) return emit(OpCode::CALLFN, FnId::WAVPAUSE, 0);
        if (match_word("WAVRESUME")) return emit(OpCode::CALLFN, FnId::WAVRESUME, 0);
        if (match_word("I2C")) {
            if (!match_word("SCAN")) {
                set_error("EXPECTED SCAN");
                return false;
            }
            return emit(OpCode::CALLFN, FnId::I2CSCAN, 0);
        }
        if (match_word("I2CWRITE")) return compile_call_statement(FnId::I2CWRITE);
        if (match_word("LOCATE")) return compile_call_statement(FnId::LOCATE);
        if (match_word("GLOCATE")) return compile_call_statement(FnId::GLOCATE);
        if (match_word("GPRINT")) return compile_call_statement(FnId::GPRINT);
        if (match_word("GDEF")) return compile_gdef();
        if (match_word("GPALETTE")) return compile_gpalette();
        if (match_word("RANDOMIZE")) return compile_call_statement(FnId::RANDOMIZE);
        if (match_word("SAVEIMAGE")) return compile_call_statement(FnId::GSAVE);

        {
            const char* save_pos = p;
            if (match_word("SAVE")) {
                if (match_word("IMAGE")) {
                    return compile_call_statement(FnId::GSAVE);
                }
                p = save_pos;
            }
        }

        if (match_word("END") || match_word("STOP")) {
            return emit(OpCode::HALT);
        }

        p = save;
        return compile_assignment();
    }

    bool compile_line() {
        while (true) {
            if (!compile_statement()) return false;

            skip_spaces();

            if (*p == '\0') return true;

            if (*p == ':') {
                ++p;
                continue;
            }

            set_error("UNEXPECTED TEXT");
            return false;
        }
    }
};

CompileResult fail_result(std::int32_t line, const char* message) {
    CompileResult result;
    result.ok = false;
    result.line = line;
    std::snprintf(result.message, sizeof(result.message), "%s", message);
    return result;
}

} // namespace

CompileResult BasicCompiler::compile(
    const ProgramStore& source,
    CompiledProgram& output
) {
    return compile_source(&source, nullptr, output);
}

CompileResult BasicCompiler::compile_direct(const char* line, CompiledProgram& output) {
    // Direct mode keeps the RAM-compatible 191-character boundary.
    char input[kMaxProgramLineLength] = {};
    if (!line || std::strlen(line) >= sizeof(input)) {
        output.reset();
        return fail_result(0, "DIRECT LINE TOO LONG");
    }
    std::strcpy(input, line);
    return compile_source(nullptr, input, output);
}

CompileResult BasicCompiler::compile_source(
    const ProgramStore* source, const char* direct_line, CompiledProgram& output
) {
    output.reset();

    if (source && !source->ready()) return fail_result(0, source->error());
    const std::size_t count = source ? source->size() : 1;
    if (count > kMaxSdProgramLines) {
        return fail_result(0, "TOO MANY PROGRAM LINES");
    }
    if (!output.reserve_lines(count)) return fail_result(0, "OUT OF MEMORY");

    CompileControl control;

    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t stored_number = 0;
        const char* stored_text = nullptr;
        std::size_t stored_length = 0;
        if (source && !source->read_line_text(
                i, stored_number, stored_text, stored_length))
            return fail_result(0, source->error());
        const auto number = source ? stored_number : 10;
        const char* text = source ? stored_text : direct_line;

        output.line_at(output.line_count).line = number;
        output.line_at(output.line_count).pc =
            static_cast<std::int32_t>(output.code_count);
        ++output.line_count;

        Parser parser;
        parser.p = text;
        parser.out = &output;
        parser.control = &control;
        parser.line = number;

        if (!parser.compile_line()) {
            return fail_result(
                number,
                parser.error[0] ? parser.error : "SYNTAX ERROR"
            );
        }
    }

    if (control.for_depth != 0) {
        return fail_result(0, "FOR WITHOUT NEXT");
    }
    if (control.while_depth != 0) {
        return fail_result(0, "WHILE WITHOUT WEND");
    }
    if (control.do_depth != 0) {
        return fail_result(0, "DO WITHOUT LOOP");
    }

    if (output.code_count == 0 ||
        output.code[output.code_count - 1].code != OpCode::HALT) {
        Op halt;
        halt.code = OpCode::HALT;
        if (!output.emit(halt)) {
            return fail_result(0, "PROGRAM TOO COMPLEX");
        }
    }

    for (std::size_t i = 0; i < output.code_count; ++i) {
        Op& op = output.code[i];

        if ((op.code != OpCode::JMP && op.code != OpCode::GOSUB) ||
            op.b != 1) {
            continue;
        }

        const int pc = output.find_pc_for_line(op.a);
        if (pc < 0) {
            return fail_result(op.a, "BAD JUMP TARGET");
        }

        op.a = pc;
        op.b = 0;
    }

    optimize_program(output);

    CompileResult result;
    result.ok = true;
    return result;
}

} // namespace rmb
