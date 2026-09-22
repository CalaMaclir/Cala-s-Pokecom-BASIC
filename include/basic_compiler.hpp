#pragma once

#include <cstdint>

#include "il.hpp"
#include "program_store.hpp"

namespace rmb {

struct CompileResult {
    bool ok = false;
    std::int32_t line = 0;
    char message[96] = {};
};

class BasicCompiler {
public:
    CompileResult compile(
        const ProgramStore& source,
        CompiledProgram& output
    );
    // Borrow one input line during compilation, never allocate ProgramStore.
    CompileResult compile_direct(const char* line, CompiledProgram& output);

private:
    CompileResult compile_source(const ProgramStore* source,
                                 const char* direct_line,
                                 CompiledProgram& output);
};

} // namespace rmb
