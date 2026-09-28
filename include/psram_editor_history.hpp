#pragma once

#include <cstddef>
#include <cstdint>

#include "editor_history.hpp"

namespace rmb {

class PsramEditorHistory final : public EditorHistory {
public:
    PsramEditorHistory() = default;
    ~PsramEditorHistory() override;

    bool begin();
    void end();
    bool ready() const override { return ready_; }
    void clear() override;
    void clear_redo() override;
    bool push_undo(const EditorHistorySnapshot& snapshot) override;
    bool pop_undo(EditorHistorySnapshot& snapshot) override;
    bool push_redo(const EditorHistorySnapshot& snapshot) override;
    bool pop_redo(EditorHistorySnapshot& snapshot) override;
    std::size_t undo_count() const override { return undo_.count; }
    std::size_t redo_count() const override { return redo_.count; }
    std::uint32_t allocated_bytes() const { return allocated_bytes_; }

private:
    struct Stack {
        std::uint32_t base = 0;
        std::uint32_t capacity = 0;
        std::uint32_t start = 0;
        std::uint32_t count = 0;
    };

    bool ready_ = false;
    std::uint32_t allocated_bytes_ = 0;
    Stack undo_{};
    Stack redo_{};

    bool push(Stack& stack, const EditorHistorySnapshot& snapshot);
    bool pop(Stack& stack, EditorHistorySnapshot& snapshot);
};

} // namespace rmb
