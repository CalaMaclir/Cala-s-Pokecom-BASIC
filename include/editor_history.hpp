#pragma once

#include <cstddef>
#include <cstdint>

#include "program_store.hpp"

namespace rmb {

struct EditorHistorySnapshot {
    std::uint64_t state_id = 0;
    std::int32_t line_number = 0;
    std::uint32_t line_index = 0;
    std::uint16_t length = 0;
    std::uint16_t cursor = 0;
    bool existed = false;
    char body[kMaxSdProgramLineLength] = {};
};

class EditorHistory {
public:
    virtual ~EditorHistory() = default;
    virtual bool ready() const = 0;
    virtual void clear() = 0;
    virtual void clear_redo() = 0;
    virtual bool push_undo(const EditorHistorySnapshot& snapshot) = 0;
    virtual bool pop_undo(EditorHistorySnapshot& snapshot) = 0;
    virtual bool push_redo(const EditorHistorySnapshot& snapshot) = 0;
    virtual bool pop_redo(EditorHistorySnapshot& snapshot) = 0;
    virtual std::size_t undo_count() const = 0;
    virtual std::size_t redo_count() const = 0;
};

} // namespace rmb
