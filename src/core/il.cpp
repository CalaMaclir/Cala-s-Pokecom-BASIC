#include "il.hpp"

#include <cstring>
#include <cstdlib>

namespace rmb {
CompiledProgram::~CompiledProgram() { std::free(extra_lines); }
bool CompiledProgram::reserve_lines(std::size_t count) {
    if(count<=kMaxLineMap) return true;
    extra_lines=static_cast<LinePc*>(std::calloc(count-kMaxLineMap,sizeof(LinePc)));
    return extra_lines!=nullptr;
}

void CompiledProgram::reset() {
    std::free(extra_lines); extra_lines=nullptr;
    code_count = 0;
    number_count = 0;
    string_used = 1;
    string_pool[0] = '\0';
    symbol_count = 0;
    line_count = 0;
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
    if (!text) return 0;

    const std::size_t len = std::strlen(text) + 1;
    if (string_used + len > kStringPoolSize || string_used > 0xffffu) {
        return 0xffffu;
    }

    const auto offset = static_cast<std::uint16_t>(string_used);
    std::memcpy(string_pool + string_used, text, len);
    string_used += len;
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
    for (std::size_t i = 0; i < line_count; ++i) {
        if (line_at(i).line == line) return line_at(i).pc;
    }
    return -1;
}

} // namespace rmb
