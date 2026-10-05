#pragma once
#define RMB_LINE_EDITOR_WORKFLOW_API 1

#include <cstddef>

namespace rmb {

class CommandHistory;

class LineEditor {
public:
    // Reads one editable console line and always NUL-terminates the buffer.
    // Returns the number of characters stored, excluding the terminator.
    static std::size_t read(
        char* buffer,
        std::size_t capacity,
        CommandHistory* history = nullptr,
        const char* initial = nullptr
    );
    // Only the BASIC prompt opts in; dialogs keep the existing four-arg API.
    static std::size_t read(
        char* buffer,
        std::size_t capacity,
        CommandHistory* history,
        const char* initial,
        bool workflow_shortcuts
    );
    static int last_special_key();
};

} // namespace rmb
