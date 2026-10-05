#pragma once

#include <cstddef>
#include <cstdint>

#include "editor_history.hpp"
#include "program_store.hpp"

namespace rmb {

// Hardware-independent source interface. The firmware adapter delegates to
// ProgramStore; host tests use a deterministic in-memory implementation.
class EditorDocument {
public:
    virtual ~EditorDocument() = default;
    virtual ProgramSourceMode source_mode() const { return ProgramSourceMode::ClassicNumbered; }
    virtual bool replace_source_rows(std::size_t,std::size_t,const char* const*,std::size_t) { return false; }
    virtual std::size_t line_count() const = 0;
    virtual std::size_t max_body_length() const = 0;
    virtual bool line_metadata(
        std::size_t index,
        std::int32_t& number,
        std::size_t& length
    ) const = 0;
    virtual bool read_line(
        std::size_t index,
        std::int32_t& number,
        const char*& body,
        std::size_t& length
    ) const = 0;
    virtual bool set_line(std::int32_t number, const char* body) = 0;
    virtual bool erase_line(std::int32_t number) = 0;
    virtual const char* error() const = 0;
    virtual bool replace_line_pair(std::int32_t, const char*, std::int32_t, const char*) {
        return false;
    }
};

struct EditorVisualPosition {
    std::size_t line = 0;
    std::size_t subrow = 0;
};

class EditorModel {
public:
    static constexpr std::size_t kWorkingCapacity = kMaxSdProgramLineLength;
    static constexpr std::size_t kFindCapacity = 96;

    explicit EditorModel(
        EditorDocument& document,
        EditorHistory* history = nullptr
    );

    void set_history(EditorHistory* history) { history_ = history; }

    bool begin();
    void configure_viewport(
        std::size_t visible_lines,
        std::size_t visible_body_columns
    );

    bool structured() const { return document_.source_mode()==ProgramSourceMode::Structured; }
    bool has_line() const { return has_line_; }
    std::size_t line_count() const { return document_.line_count(); }
    std::size_t current_index() const { return index_; }
    std::int32_t line_number() const { return number_; }
    const char* body() const { return body_; }
    std::size_t length() const { return length_; }
    std::size_t cursor() const { return cursor_; }
    EditorVisualPosition visual_top() const { return visual_top_; }
    std::size_t cursor_screen_row() const { return cursor_screen_row_; }
    bool cursor_screen_row_valid() const { return cursor_screen_row_valid_; }
    EditorVisualPosition current_visual_position() const;
    std::size_t cursor_visual_column() const;
    std::size_t visual_row_count(std::size_t index) const;
    bool next_visual_position(EditorVisualPosition& position) const;
    bool previous_visual_position(EditorVisualPosition& position) const;
    bool visual_screen_row(
        EditorVisualPosition position,
        std::size_t& screen_row
    ) const;
    bool line_dirty() const { return line_dirty_; }
    bool changed() const { return state_id_ != saved_state_id_; }
    bool history_ready() const { return history_ && history_->ready(); }
    std::size_t undo_count() const {
        return history_ready() ? history_->undo_count() : 0;
    }
    std::size_t redo_count() const {
        return history_ready() ? history_->redo_count() : 0;
    }
    const char* find_term() const { return find_term_; }
    const char* error() const { return error_; }

    bool read_visible_line(
        std::size_t index,
        std::int32_t& number,
        char* body,
        std::size_t capacity,
        std::size_t& length
    ) const;
    bool line_view(
        std::size_t index,
        std::int32_t& number,
        const char*& body,
        std::size_t& length
    ) const;

    bool move_left();
    bool move_right();
    bool move_home();
    bool move_end();
    bool move_up();
    bool move_down();
    bool page_up();
    bool page_down();

    bool insert_char(char value);
    // Insert spaces to the next logical four-column tab stop, atomically.
    bool insert_tab();
    bool outdent();
    bool matching_block();
    bool backspace();
    bool delete_char();

    bool commit();
    bool insert_line(std::int32_t number);
    bool split_line(std::int32_t new_number);
    bool join_line(bool previous);
    bool delete_line();
    bool undo();
    bool redo();
    bool find(const char* term, bool next);
    bool find_previous();
    bool goto_line(std::int32_t requested);
    void mark_saved();

private:
    EditorDocument& document_;
    EditorHistory* history_ = nullptr;
    char body_[kWorkingCapacity] = {};
    char find_term_[kFindCapacity] = {};
    std::size_t index_ = 0;
    std::int32_t number_ = 0;
    std::size_t length_ = 0;
    std::size_t cursor_ = 0;
    std::size_t preferred_visual_column_ = 0;
    EditorVisualPosition visual_top_{};
    std::size_t cursor_screen_row_ = 0;
    std::size_t visible_lines_ = 1;
    std::size_t visible_body_columns_ = 1;
    bool has_line_ = false;
    bool cursor_screen_row_valid_ = false;
    bool line_dirty_ = false;
    // State IDs identify document contents, not edit depth. next_state_id_
    // never moves backwards during Undo, so a divergent branch cannot reuse
    // the saved state's identity.
    std::uint64_t state_id_ = 0;
    std::uint64_t saved_state_id_ = 0;
    std::uint64_t next_state_id_ = 1;
    EditorHistorySnapshot history_snapshot_{};
    EditorHistorySnapshot history_swap_{};
    enum class EditGroup : std::uint8_t {
        None,
        Insert,
        Backspace,
        Delete
    };
    EditGroup edit_group_ = EditGroup::None;
    std::int32_t edit_group_line_ = 0;
    std::size_t edit_group_cursor_ = 0;
    bool history_fault_ = false;
    const char* error_ = "OK";

    bool capture_rows(std::size_t first,std::size_t count,EditorHistorySnapshot& out);
    bool matching_indent(const char* current, std::size_t& indent) const;
    bool structured_replace(std::size_t first,std::size_t remove,const char* const* rows,
                            std::size_t insert,std::size_t focus,std::size_t cursor);
    bool fail(const char* message);
    void history_boundary();
    bool history_group_matches(EditGroup group) const;
    void capture_current(EditorHistorySnapshot& snapshot) const;
    bool capture_line(
        std::int32_t line_number,
        EditorHistorySnapshot& snapshot
    );
    void record_edit(
        const EditorHistorySnapshot& before,
        EditGroup group
    );
    bool capture_other(std::int32_t number, EditorHistorySnapshot& snapshot);
    bool restore_snapshot(const EditorHistorySnapshot& snapshot);
    bool apply_history(bool redo);
    bool load_index(
        std::size_t index,
        bool preserve_visual_column,
        bool synchronize_viewport = true
    );
    bool move_vertical(int direction, std::size_t rows);
    bool move_page(int direction);
    bool move_to_visual(
        EditorVisualPosition position,
        bool incremental_navigation
    );
    std::size_t cursor_for_visual_row(
        std::size_t subrow,
        std::size_t visual_column
    ) const;
    void ensure_visual_cursor_visible();
    void update_navigation_view(
        EditorVisualPosition previous,
        EditorVisualPosition current
    );
    void normalize_visual_top();
    bool locate_line(
        std::int32_t requested,
        std::size_t& index,
        bool& exact
    );
    static const char* find_case_insensitive(
        const char* text,
        std::size_t length,
        const char* term,
        std::size_t start
    );
    static const char* rfind_case_insensitive(
        const char* text,
        std::size_t length,
        const char* term,
        std::size_t end
    );
};

} // namespace rmb
