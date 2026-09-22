#pragma once

#include <cstddef>

namespace rmb {

class LineEditor {
public:
    // Reads one editable console line and always NUL-terminates the buffer.
    // Returns the number of characters stored, excluding the terminator.
    static std::size_t read(char* buffer, std::size_t capacity);
    static int last_special_key();
};

} // namespace rmb
