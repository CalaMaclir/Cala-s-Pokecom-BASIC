#include "il.hpp"

#include <cstring>
#include <cstdlib>

namespace rmb {
CompiledProgram::~CompiledProgram() { reset(); }
bool CompiledProgram::reserve_lines(std::size_t count) {
    if(count<=kMaxLineMap) return true;
    extra_lines=static_cast<LinePc*>(std::calloc(count-kMaxLineMap,sizeof(LinePc)));
    return extra_lines!=nullptr;
}

void CompiledProgram::reset() {
    for(std::size_t i=0;i<function_count;++i)std::free(functions[i].locals);
    std::free(functions);functions=nullptr;function_count=0;
    std::free(source_rows);source_rows=nullptr;source_row_count=0;
    source_mode=ProgramSourceMode::ClassicNumbered;
    std::free(extra_lines); extra_lines=nullptr;
    code_count = 0;
    number_count = 0;
    string_used = 1;
    string_pool[0] = '\0';
    symbol_count = 0;
    line_count = 0;
}

bool CompiledProgram::reserve_source_rows(std::size_t count) {
    if(count==0)return true;
    source_rows=static_cast<SourceRowPc*>(std::calloc(count,sizeof(SourceRowPc)));
    return source_rows!=nullptr;
}
int CompiledProgram::find_function(const char* name,bool base_name) const {
    const auto stem=[](const char* s) {auto n=std::strlen(s);return n&&s[n-1]=='$'?n-1:n;};
    for(std::size_t i=0;i<function_count;++i)
        if(base_name ? (stem(name)==stem(functions[i].name)&&!std::strncmp(name,functions[i].name,stem(name)))
                     : !std::strcmp(name,functions[i].name))return static_cast<int>(i);
    return -1;
}
int CompiledProgram::add_function(const char* name) {
    if(function_count>=kMaxUserFunctions)return -1;
    void* memory=std::realloc(functions,(function_count+1)*sizeof(FunctionInfo));
    if(!memory)return -1;
    functions=static_cast<FunctionInfo*>(memory);
    auto& fn=functions[function_count];fn=FunctionInfo{};
    std::strcpy(fn.name,name);const auto n=std::strlen(name);
    fn.returns_string=n&&name[n-1]=='$';
    return static_cast<int>(function_count++);
}
int CompiledProgram::add_local(std::size_t function,const char* name) {
    auto& fn=functions[function];
    for(std::size_t i=0;i<fn.local_count;++i)
        if(!std::strcmp(fn.locals[i].symbol.name,name))return static_cast<int>(i);
    if(fn.local_count>=kMaxFunctionLocals)return -1;
    void* memory=std::realloc(fn.locals,(fn.local_count+1)*sizeof(LocalSymbol));
    if(!memory)return -1;
    fn.locals=static_cast<LocalSymbol*>(memory);
    auto& local=fn.locals[fn.local_count];local=LocalSymbol{};
    std::strcpy(local.symbol.name,name);const auto n=std::strlen(name);
    local.symbol.is_string=n&&name[n-1]=='$';
    local.slot=local.symbol.is_string?fn.string_local_count++:fn.numeric_local_count++;
    return static_cast<int>(fn.local_count++);
}
bool CompiledProgram::emit(const Op& op) {
    if (code_count >= kMaxOps) return false;
    code[code_count++] = op;
    return true;
}

std::int32_t CompiledProgram::intern_number(BasicNumber value) {
    // Deduplicate by representation, not by floating-point comparison. This
    // preserves signed zero and avoids surprising NaN behaviour if such a
    // value is introduced by a future parser extension.
    for (std::size_t i = 0; i < number_count; ++i) {
        if (std::memcmp(&number_pool[i], &value, sizeof(value)) == 0) {
            return static_cast<std::int32_t>(i);
        }
    }

    if (number_count >= kNumberPoolSize) return -1;
    number_pool[number_count] = value;
    return static_cast<std::int32_t>(number_count++);
}

std::uint16_t CompiledProgram::intern_string(const char* text) {
    return text ? intern_string(text, std::strlen(text)) : 0;
}

std::uint16_t CompiledProgram::intern_string(
    const char* text,
    std::size_t length
) {
    if (!text) return 0;

    const std::size_t bytes = length + 1;
    if (string_used + bytes > kStringPoolSize || string_used > 0xffffu) {
        return 0xffffu;
    }

    const auto offset = static_cast<std::uint16_t>(string_used);
    std::memcpy(string_pool + string_used, text, length);
    string_pool[string_used + length] = '\0';
    string_used += bytes;
    return offset;
}

int CompiledProgram::find_or_add_symbol(const char* name) {
    if (!name || !*name) return -1;

    char normalized[kSymbolNameLength] = {};
    std::size_t n = 0;

    while (name[n] && n + 1 < sizeof(normalized)) {
        char c = name[n];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        normalized[n] = c;
        ++n;
    }

    for (std::size_t i = 0; i < symbol_count; ++i) {
        if (std::strcmp(symbols[i].name, normalized) == 0) {
            return static_cast<int>(i);
        }
    }

    if (symbol_count >= kMaxSymbols) return -1;

    Symbol& sym = symbols[symbol_count];
    std::strncpy(sym.name, normalized, kSymbolNameLength - 1);
    const std::size_t len = std::strlen(sym.name);
    sym.is_string = len > 0 && sym.name[len - 1] == '$';

    return static_cast<int>(symbol_count++);
}

int CompiledProgram::find_pc_for_line(std::int32_t line) const {
    if(source_mode==ProgramSourceMode::Structured)return -1;
    for (std::size_t i = 0; i < line_count; ++i) {
        if (line_at(i).line == line) return line_at(i).pc;
    }
    return -1;
}

} // namespace rmb
