// Compile only, with the firmware ARM ABI. nm symbol sizes encode sizeof.
#include <cstddef>
#include <cstdint>
#define private public
#include "repl.hpp"
#include "full_screen_editor.hpp"
#include "function_runtime.hpp"
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
SIZE(editor_heap, rmb::FullScreenEditor)
SIZE(editor_model, rmb::EditorModel)
SIZE(compiled, rmb::CompiledProgram)
SIZE(program_storage, rmb::Repl::program_)
SIZE(numeric_arrays, rmb::BasicNumber[rmb::VM::kArrayCells])
SIZE(string_arrays, char[rmb::VM::kStringArrayCells][rmb::VM::kRuntimeStringLength])
SIZE(direct_scalars, rmb::VM::DirectScalar[rmb::kMaxSymbols])
SIZE(runtime_strings, rmb::VM::strings_)
SIZE(string_pool, rmb::CompiledProgram::string_pool)
SIZE(line_map, rmb::CompiledProgram::lines)
SIZE(symbols, rmb::CompiledProgram::symbols)
SIZE(ops, rmb::CompiledProgram::code)
SIZE(number_pool, rmb::CompiledProgram::number_pool)

SIZE(compiler, rmb::BasicCompiler)
SIZE(function_info, rmb::FunctionInfo)
SIZE(local_symbol, rmb::LocalSymbol)
SIZE(source_row, rmb::SourceRowPc)
SIZE(call_frame_header, rmb::UserCallFrame)
extern "C" {
char memory_size_call_frame_fact[sizeof(rmb::UserCallFrame)+sizeof(rmb::BasicNumber)];
char memory_size_call_frame_fact_depth16[16*(sizeof(rmb::UserCallFrame)+sizeof(rmb::BasicNumber))];
char memory_size_call_frame_wrap[sizeof(rmb::UserCallFrame)+128];
char memory_size_call_frame_max_strings[sizeof(rmb::UserCallFrame)+128*128];
char memory_size_call_frame_max_strings_depth16[16*(sizeof(rmb::UserCallFrame)+128*128)];
}
