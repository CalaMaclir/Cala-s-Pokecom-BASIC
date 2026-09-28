#pragma once

#include <cstddef>
#include <cstdint>

#include "editor_model.hpp"
#include "psram_editor_history.hpp"
#include "program_store.hpp"

namespace rmb {

class ProgramStoreEditorDocument final : public EditorDocument {
public:
    explicit ProgramStoreEditorDocument(ProgramStore& program)
        : program_(program) {}

    std::size_t line_count() const override { return program_.size(); }
    std::size_t max_body_length() const override {
        return program_.backend_type() == ProgramBackend::Sd
            ? kMaxSdProgramLineLength - 1
            : kMaxProgramLineLength - 1;
    }
    bool line_metadata(
        std::size_t index,
        std::int32_t& number,
        std::size_t& length
    ) const override {
        return program_.read_line_metadata(index, number, length);
    }
    bool read_line(
        std::size_t index,
        std::int32_t& number,
        const char*& body,
        std::size_t& length
    ) const override {
        return program_.read_line_text(index, number, body, length);
    }
    bool set_line(std::int32_t number, const char* body) override {
        return program_.set_line(number, body);
    }
    bool erase_line(std::int32_t number) override {
        return program_.erase_line(number);
    }
    const char* error() const override { return program_.error(); }

private:
    ProgramStore& program_;
};

class FullScreenEditor {
public:
    FullScreenEditor(
        ProgramStore& program,
        char* filename,
        std::size_t filename_capacity,
        bool& program_dirty
    );

    // Runs synchronously and returns when the user exits the editor.
    bool run();

private:
    static constexpr int kColumns = 53;
    static constexpr int kBodyFirstRow = 3;
    static constexpr int kBodyLastRow = 35;
    static constexpr int kVisibleLines =
        kBodyLastRow - kBodyFirstRow + 1;
    // Signed 32-bit BASIC line numbers need 11 characters plus a separator.
    static constexpr int kPrefixColumns = 12;
    static constexpr int kBodyColumns = kColumns - kPrefixColumns;

    ProgramStore& program_;
    char* filename_;
    std::size_t filename_capacity_;
    bool& program_dirty_;
    bool entry_dirty_ = false;
    ProgramStoreEditorDocument document_;
    PsramEditorHistory history_;
    EditorModel model_;
    char message_[80] = {};
    bool navigation_burst_active_ = false;
    bool navigation_viewport_changed_ = false;
    std::uint32_t navigation_last_paint_ms_ = 0;

    void render_full();
    void render_header();
    void render_location(bool lower_row = true);
    struct ViewportRenderContext {
        FullScreenEditor* editor = nullptr;
        EditorVisualPosition position{};
        int row = kBodyFirstRow;
        bool valid = false;
    };

    void render_navigation(EditorVisualPosition previous);
    void render_edge_line_number(bool downward);
    void navigation_burst_step(int key);
    static void navigation_burst_step_thunk(int key, void* context);
    void render_visual_row(EditorVisualPosition position, int row);
    void render_visual_row_text(
        EditorVisualPosition position,
        int row,
        std::int32_t number,
        const char* body,
        std::size_t length
    );
    static bool render_viewport_line(
        std::size_t index,
        std::int32_t number,
        const char* body,
        std::size_t length,
        void* context
    );
    void position_cursor();
    void set_message(const char* text);
    void set_model_error();
    bool effective_dirty() const;
    bool sync_committed_dirty();
    bool prompt(
        const char* title,
        const char* label,
        char* output,
        std::size_t capacity,
        const char* initial = nullptr
    );
    bool confirm_delete_line();
    int confirm_exit();
    bool confirm_overwrite(const char* filename);
    bool save(bool save_as);
    bool insert_line();
    bool find(bool next, bool previous = false);
    bool goto_line();
    bool exit_requested();
};

} // namespace rmb
