#pragma once
#include <cctype>

namespace rmb::basic_lexical {
inline void skip_spaces(const char*& p) {
    while (*p == ' ' || *p == '\t') ++p;
}
inline bool identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
// Same whitespace, case and identifier-boundary rules as Parser::match_word.
inline bool match_word(const char*& p, const char* word) {
    skip_spaces(p);
    const char* q = p;
    const char* w = word;
    while (*w && *q && (*q >= 'a' && *q <= 'z' ? *q - 'a' + 'A' : *q) == *w) {
        ++q;
        ++w;
    }
    if (*w || identifier_char(*q)) return false;
    p = q;
    return true;
}
} // namespace rmb::basic_lexical
