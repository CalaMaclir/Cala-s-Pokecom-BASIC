// Compile only, with the firmware ARM ABI. nm symbol sizes encode sizeof.
#include <cstddef>
#include <cstdint>
#define private public
#include "repl.hpp"
#undef private
static_assert(rmb::kMaxProgramLineLength==192);
static_assert(sizeof(rmb::ProgramLine)==sizeof(std::int32_t)+192);
static_assert(sizeof(rmb::RamProgramStore)==
              rmb::kMaxRamProgramLines*sizeof(rmb::ProgramLine));
#define SIZE(name, expression) extern "C" { char memory_size_##name[sizeof(expression)]; }
SIZE(repl, rmb::Repl)
SIZE(vm, rmb::VM)
SIZE(program, rmb::ProgramStore)
SIZE(ram_backend_heap, rmb::RamProgramStore)
SIZE(sd_backend_heap, rmb::SdProgramStore)
SIZE(compiled, rmb::CompiledProgram)
SIZE(program_storage, rmb::Repl::program_)
SIZE(numeric_arrays, rmb::VM::array_pool_)
SIZE(string_arrays, rmb::VM::string_array_pool_)
SIZE(direct_scalars, rmb::VM::direct_state_)
SIZE(runtime_strings, rmb::VM::strings_)
SIZE(string_pool, rmb::CompiledProgram::string_pool)
SIZE(line_map, rmb::CompiledProgram::lines)
SIZE(symbols, rmb::CompiledProgram::symbols)
SIZE(ops, rmb::CompiledProgram::code)
SIZE(number_pool, rmb::CompiledProgram::number_pool)
