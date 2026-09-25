#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb {

// RetroMiniBASIC targets pocket-computer workloads. Single precision is the
// native numeric format for the fast VM and materially reduces RP2350 math,
// stack and array costs.
using BasicNumber = float;

enum class OpCode : std::uint8_t {
    PUSH_NUM, PUSH_STR, LOAD, STORE,
    LOAD_NUM, LOAD_STR, STORE_NUM, STORE_STR,
    LOAD_ARR, STORE_ARR, DIM_ARR,
    LOAD_IND, STORE_IND, LOAD_ARR_IND, STORE_ARR_IND,
    ADD, SUB, MUL, DIV, POW, NEG, MOD,
    ADD_NUM, SUB_NUM, MUL_NUM, DIV_NUM, POW_NUM, NEG_NUM, MOD_NUM,
    CEQ, CNE, CLT, CLE, CGT, CGE,
    CEQ_NUM, CNE_NUM, CLT_NUM, CLE_NUM, CGT_NUM, CGE_NUM,
    CEQ_NUM_JZ, CNE_NUM_JZ, CLT_NUM_JZ, CLE_NUM_JZ, CGT_NUM_JZ, CGE_NUM_JZ,
    NOT, AND, OR,
    NOT_NUM, AND_NUM, OR_NUM,
    NOT_NUM_JZ, AND_NUM_JZ, OR_NUM_JZ,
    CALLFN,
    PRINT, PRINT_SPC, PRINT_SUPPRESS_NL, PRINT_NL,
    JMP, JZ,
    FOR_INIT, FOR_CHECK, FOR_INCR,
    GOSUB, RETSUB,
    ON_GOTO, ON_GOSUB,
    DUP, DROP, SWAP, OVER, ROT,
    PRINT_STACK, PRINT_CR, EMIT_CHAR,
    BAND, BOR, BXOR,

    // VM 0.6 compile-time superinstructions. These preserve the original
    // instruction slots; the VM skips the covered tail operations.
    MOV_NUM,
    ADD_VV_PUSH,
    SUB_VV_PUSH,
    MUL_VV_PUSH,
    MUL_CV_PUSH,
    ADD_VV_STORE,
    SUB_VV_STORE,
    MUL_VV_STORE,
    RGB_PSET_VV,

    // VM 0.6 stage 3 expression superinstructions.
    MULADD_VVV_STORE,          // dst = addend + lhs * rhs
    MULSUBADD_VVVVV_STORE,     // dst = a*b - c*d + e
    MUL_CVV_ADD_V_STORE,       // dst = (constant*a)*b + c
    SQ2_GT_CONST_OR_JZ,        // if !(x*x>c || y*y>c) jump
    SUMSQ_GT_CONST_JZ,         // if !(x*x + y*y > c) jump

    // VM 0.6 stage 4A: one complex iteration + escape test.
    COMPLEX_ITER_OR_JZ,         // z=z*z+c; optional move; |Re|/|Im| escape
    COMPLEX_ITER_SUMSQ_JZ,      // z=z*z+c; optional move; |z|^2 escape

    // VM 0.6 stage 4C: complete pixel color pipeline.
    GRAY_PSET_MUL_INT,           // G=INT(A*B); COLOR G,G,G; PSET X,Y
    GRAY_PSET_INT_STACK,         // G=INT(TOS); COLOR G,G,G; PSET X,Y
    RGB_PSET_MINMUL,             // RGB=MIN(cap,I*k); COLOR; PSET

    // VM 0.6 stage 5: numeric-function fast paths.
    FN0_NUM,                     // zero-argument numeric function
    FN1_NUM,                     // one-argument numeric function
    FN2_NUM,                     // two-argument numeric function
    FN3_NUM,                     // three-argument numeric function

    // VM 0.6 stage 8: residual dispatch reduction.
    STORE_CONST_NUM,             // dst = constant
    ADD_VC_PUSH,                 // push(var + constant)
    SUB_VC_PUSH,                 // push(var - constant)
    DIV_VC_PUSH,                 // push(var / constant)

    HALT
};

namespace FnId {
constexpr int ABS = 1;
constexpr int INT = 2;
constexpr int VAL = 3;
constexpr int STRS = 4;
constexpr int LEN = 5;
constexpr int CHRS = 6;
constexpr int ASC = 7;
constexpr int LEFTS = 8;
constexpr int RIGHTS = 9;
constexpr int MIDS = 10;
constexpr int RND = 11;
constexpr int SPC = 12;
constexpr int TAB = 13;
constexpr int INSTR = 14;
constexpr int STRINGS = 15;

constexpr int SIN = 20;
constexpr int COS = 21;
constexpr int TAN = 22;
constexpr int SQR = 23;
constexpr int ATN = 24;
constexpr int LOG = 25;
constexpr int EXP = 26;
constexpr int PI = 27;
constexpr int RAD = 28;
constexpr int DEG = 29;
constexpr int SGN = 30;
constexpr int MIN = 31;
constexpr int MAX = 32;
constexpr int CLAMP = 33;
constexpr int RNDI = 34;
constexpr int TIMER = 40;
constexpr int RANDOMIZE = 41;
constexpr int INKEY = 42;
constexpr int I2CREAD = 43;
constexpr int I2CWRITE = 44;
constexpr int I2CSCAN = 45;
constexpr int BEEP = 46;
constexpr int PLAY = 47;
constexpr int PLAYSTOP = 48;
constexpr int PLAYPAUSE = 49;
constexpr int PLAYRESUME = 50;
constexpr int PLAYWAIT = 51;
constexpr int PLAYING = 52;
constexpr int WAVPLAY = 53;
constexpr int WAVSTOP = 54;
constexpr int WAVPAUSE = 55;
constexpr int WAVRESUME = 56;

constexpr int INPUT = 100;
constexpr int LOCATE = 120;

constexpr int SCREEN = 200;
constexpr int GCLS = 201;
constexpr int GCOLOR = 202;
constexpr int GPSET = 203;
constexpr int GLINE = 204;
constexpr int GCIRCLE = 205;
constexpr int GBOX = 206;
constexpr int GFLUSH = 207;
constexpr int GCOLORHSV = 208;
constexpr int GSAVE = 209;
constexpr int GSLEEP = 210;
constexpr int GPOINT = 211;
constexpr int GLOCATE = 212;
constexpr int GPRINT = 213;
constexpr int GPAINT = 214;
constexpr int PAUSE = 215;
constexpr int GDEF = 216;
constexpr int GPALETTE = 217;
} // namespace FnId

// Compact fixed-width VM instruction.
//
// Numeric literals are stored in CompiledProgram::number_pool, so the large
// double field no longer has to be carried by every instruction.  The layout
// remains intentionally simple because a and b are also used while the
// compiler patches jumps/FOR control flow.
//
// RP2350/ARM ABI: 12 bytes instead of the previous 32 bytes.
struct Op {
    OpCode code = OpCode::HALT;
    std::uint8_t flags = 0;
    std::uint16_t s = 0;
    std::int32_t a = 0;
    std::int32_t b = 0;
};

static_assert(sizeof(Op) == 12, "VM Op must stay compact");

constexpr std::size_t kMaxOps = 1536;
constexpr std::size_t kNumberPoolSize = kMaxOps;
constexpr std::size_t kStringPoolSize = 6144;
constexpr std::size_t kMaxSymbols = 64;
static_assert(kMaxSymbols <= 64, "Stage-3 packed opcodes require 6-bit symbol slots");
constexpr std::size_t kSymbolNameLength = 17;
constexpr std::size_t kMaxLineMap = 256;

struct Symbol {
    char name[kSymbolNameLength] = {};
    bool is_string = false;
};

struct LinePc {
    std::int32_t line = 0;
    std::int32_t pc = 0;
};

struct CompiledProgram {
    Op code[kMaxOps] = {};
    std::size_t code_count = 0;

    BasicNumber number_pool[kNumberPoolSize] = {};
    std::size_t number_count = 0;

    char string_pool[kStringPoolSize] = {};
    std::size_t string_used = 1;

    Symbol symbols[kMaxSymbols] = {};
    std::size_t symbol_count = 0;

    LinePc lines[kMaxLineMap] = {};
    std::size_t line_count = 0;
    // Only SD programs above the RAM source limit need an extended line map.
    LinePc* extra_lines = nullptr;
    CompiledProgram() = default;
    ~CompiledProgram();
    CompiledProgram(const CompiledProgram&) = delete;
    CompiledProgram& operator=(const CompiledProgram&) = delete;
    bool reserve_lines(std::size_t count);
    LinePc& line_at(std::size_t i) { return i<kMaxLineMap ? lines[i] : extra_lines[i-kMaxLineMap]; }
    const LinePc& line_at(std::size_t i) const { return i<kMaxLineMap ? lines[i] : extra_lines[i-kMaxLineMap]; }

    void reset();
    bool emit(const Op& op);
    std::int32_t intern_number(BasicNumber value);
    std::uint16_t intern_string(const char* text);
    int find_or_add_symbol(const char* name);
    int find_pc_for_line(std::int32_t line) const;
};

} // namespace rmb
