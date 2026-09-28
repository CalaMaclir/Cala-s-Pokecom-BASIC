#include "editor_model.hpp"
#include "editor_perf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rmb {
namespace {

bool position_before(
    EditorVisualPosition left,
    EditorVisualPosition right
) {
    return left.line < right.line ||
        (left.line == right.line && left.subrow < right.subrow);
}

} // namespace

EditorModel::EditorModel(
    EditorDocument& document,
    EditorHistory* history
) : document_(document), history_(history) {}

bool EditorModel::fail(const char* message) {
    error_ = message ? message : "EDITOR ERROR";
    return false;
}

void EditorModel::configure_viewport(
    std::size_t visible_lines,
    std::size_t visible_body_columns
) {
    visible_lines_ = visible_lines ? visible_lines : 1;
    visible_body_columns_ = visible_body_columns ? visible_body_columns : 1;
    preferred_visual_column_ = std::min(
        preferred_visual_column_, visible_body_columns_ - 1);
    ensure_visual_cursor_visible();
}

bool EditorModel::begin() {
    index_ = 0;
    number_ = 0;
    length_ = 0;
    cursor_ = 0;
    preferred_visual_column_ = 0;
    visual_top_ = {};
    cursor_screen_row_ = 0;
    cursor_screen_row_valid_ = false;
    has_line_ = false;
    line_dirty_ = false;
    state_id_ = 0;
    saved_state_id_ = 0;
    next_state_id_ = 1;
    edit_group_ = EditGroup::None;
    history_fault_ = false;
    if (history_ready()) history_->clear();
    find_term_[0] = '\0';
    body_[0] = '\0';
    error_ = "OK";
    if (document_.line_count() == 0) return true;
    return load_index(0, false);
}

bool EditorModel::read_visible_line(
    std::size_t index,
    std::int32_t& number,
    char* body,
    std::size_t capacity,
    std::size_t& length
) const {
    const char* source = nullptr;
    if (!line_view(index, number, source, length)) return false;
    if (!body || !source || capacity <= length) return false;
    std::memcpy(body, source, length + 1);
    return true;
}

bool EditorModel::line_view(
    std::size_t index,
    std::int32_t& number,
    const char*& body,
    std::size_t& length
) const {
    if (index == index_ && has_line_) {
        number = number_;
        body = body_;
        length = length_;
        return true;
    }
    editor_perf::body_read();
    return document_.read_line(index, number, body, length);
}

bool EditorModel::load_index(
    std::size_t index,
    bool preserve_visual_column,
    bool synchronize_viewport
) {
    if (index >= document_.line_count()) return fail("BAD LINE INDEX");
    std::int32_t number = 0;
    std::size_t length = 0;
    const char* source = nullptr;
    editor_perf::body_read();
    if (!document_.read_line(index, number, source, length)) {
        return fail(document_.error());
    }
    if (length >= sizeof(body_)) return fail("SD LINE TOO LONG");
    std::memcpy(body_, source, length + 1);
    index_ = index;
    number_ = number;
    length_ = length;
    has_line_ = true;
    line_dirty_ = false;
    if (preserve_visual_column) {
        cursor_ = cursor_for_visual_row(0, preferred_visual_column_);
    } else {
        cursor_ = 0;
        preferred_visual_column_ = 0;
    }
    if (synchronize_viewport) ensure_visual_cursor_visible();
    error_ = "OK";
    return true;
}

std::size_t EditorModel::visual_row_count(std::size_t index) const {
    std::int32_t number = 0;
    std::size_t length = 0;
    if (index == index_ && has_line_) {
        number = number_;
        length = length_;
    } else {
        editor_perf::metadata_read();
        if (!document_.line_metadata(index, number, length)) return 1;
    }
    return std::max<std::size_t>(
        1, (length + visible_body_columns_ - 1) / visible_body_columns_);
}

EditorVisualPosition EditorModel::current_visual_position() const {
    if (!has_line_) return {};
    if (cursor_ == length_ && length_ != 0 &&
        length_ % visible_body_columns_ == 0) {
        return {index_, length_ / visible_body_columns_ - 1};
    }
    return {index_, cursor_ / visible_body_columns_};
}

std::size_t EditorModel::cursor_visual_column() const {
    if (!has_line_) return 0;
    if (cursor_ == length_ && length_ != 0 &&
        length_ % visible_body_columns_ == 0) {
        return visible_body_columns_ - 1;
    }
    return cursor_ % visible_body_columns_;
}

bool EditorModel::next_visual_position(
    EditorVisualPosition& position
) const {
    const std::size_t count = document_.line_count();
    if (position.line >= count) return false;
    const std::size_t rows = visual_row_count(position.line);
    if (position.subrow + 1 < rows) {
        ++position.subrow;
        return true;
    }
    if (position.line + 1 >= count) return false;
    ++position.line;
    position.subrow = 0;
    return true;
}

bool EditorModel::previous_visual_position(
    EditorVisualPosition& position
) const {
    if (position.line >= document_.line_count()) return false;
    if (position.subrow != 0) {
        --position.subrow;
        return true;
    }
    if (position.line == 0) return false;
    --position.line;
    position.subrow = visual_row_count(position.line) - 1;
    return true;
}

bool EditorModel::visual_screen_row(
    EditorVisualPosition position,
    std::size_t& screen_row
) const {
    editor_perf::viewport_scan();
    if (position_before(position, visual_top_)) return false;
    EditorVisualPosition scan = visual_top_;
    for (std::size_t row = 0; row < visible_lines_; ++row) {
        if (scan.line == position.line && scan.subrow == position.subrow) {
            screen_row = row;
            return true;
        }
        if (!next_visual_position(scan)) break;
    }
    return false;
}

std::size_t EditorModel::cursor_for_visual_row(
    std::size_t subrow,
    std::size_t visual_column
) const {
    const std::size_t start = subrow * visible_body_columns_;
    if (start >= length_) return length_;
    const std::size_t end = std::min(
        length_, start + visible_body_columns_);
    std::size_t last = end;
    if (end < length_) --last;
    return std::min(start + visual_column, last);
}

void EditorModel::normalize_visual_top() {
    const std::size_t count = document_.line_count();
    if (!has_line_ || count == 0) {
        visual_top_ = {};
        cursor_screen_row_ = 0;
        cursor_screen_row_valid_ = false;
        return;
    }
    if (visual_top_.line >= count) visual_top_.line = count - 1;
    const std::size_t rows = visual_row_count(visual_top_.line);
    if (visual_top_.subrow >= rows) visual_top_.subrow = rows - 1;
}

void EditorModel::ensure_visual_cursor_visible() {
    normalize_visual_top();
    if (!has_line_) return;
    const EditorVisualPosition current = current_visual_position();
    if (position_before(current, visual_top_)) {
        visual_top_ = current;
        cursor_screen_row_ = 0;
        cursor_screen_row_valid_ = true;
        return;
    }
    std::size_t row = 0;
    if (visual_screen_row(current, row)) {
        cursor_screen_row_ = row;
        cursor_screen_row_valid_ = true;
        return;
    }
    visual_top_ = current;
    cursor_screen_row_ = 0;
    for (std::size_t i = 1; i < visible_lines_; ++i) {
        if (!previous_visual_position(visual_top_)) break;
        ++cursor_screen_row_;
    }
    cursor_screen_row_valid_ = true;
}

void EditorModel::update_navigation_view(
    EditorVisualPosition previous,
    EditorVisualPosition current
) {
    if (!cursor_screen_row_valid_) {
        ensure_visual_cursor_visible();
        return;
    }
    if (previous.line == current.line &&
        previous.subrow == current.subrow) return;

    if (position_before(current, previous)) {
        if (cursor_screen_row_ != 0) {
            --cursor_screen_row_;
        } else {
            visual_top_ = current;
        }
        return;
    }

    if (cursor_screen_row_ + 1 < visible_lines_) {
        ++cursor_screen_row_;
        return;
    }
    EditorVisualPosition next_top = visual_top_;
    if (next_visual_position(next_top)) visual_top_ = next_top;
    cursor_screen_row_ = visible_lines_ - 1;
}

bool EditorModel::move_left() {
    history_boundary();
    if (!has_line_ || cursor_ == 0) return false;
    const EditorVisualPosition previous = current_visual_position();
    --cursor_;
    preferred_visual_column_ = cursor_visual_column();
    update_navigation_view(previous, current_visual_position());
    return true;
}

bool EditorModel::move_right() {
    history_boundary();
    if (!has_line_ || cursor_ >= length_) return false;
    const EditorVisualPosition previous = current_visual_position();
    ++cursor_;
    preferred_visual_column_ = cursor_visual_column();
    update_navigation_view(previous, current_visual_position());
    return true;
}

bool EditorModel::move_home() {
    history_boundary();
    if (!has_line_) return false;
    cursor_ = preferred_visual_column_ = 0;
    ensure_visual_cursor_visible();
    return true;
}

bool EditorModel::move_end() {
    history_boundary();
    if (!has_line_) return false;
    cursor_ = length_;
    preferred_visual_column_ = cursor_visual_column();
    ensure_visual_cursor_visible();
    return true;
}

bool EditorModel::move_to_visual(
    EditorVisualPosition position,
    bool incremental_navigation
) {
    if (!has_line_) return false;
    const EditorVisualPosition previous = current_visual_position();
    const std::size_t desired_column = preferred_visual_column_;
    if (position.line != index_) {
        if (!commit()) return false;
        if (!load_index(position.line, false, false)) return false;
    }
    preferred_visual_column_ = desired_column;
    cursor_ = cursor_for_visual_row(
        position.subrow, preferred_visual_column_);
    if (incremental_navigation) {
        update_navigation_view(previous, current_visual_position());
    } else {
        ensure_visual_cursor_visible();
    }
    error_ = "OK";
    return true;
}

bool EditorModel::move_vertical(int direction, std::size_t rows) {
    history_boundary();
    if (!has_line_ || rows == 0) return false;
    const bool incremental_navigation = rows == 1;
    EditorVisualPosition target = current_visual_position();
    bool moved = false;
    while (rows-- != 0) {
        const bool step = direction < 0
            ? previous_visual_position(target)
            : next_visual_position(target);
        if (!step) break;
        moved = true;
    }
    return moved && move_to_visual(target, incremental_navigation);
}

bool EditorModel::move_up() {
    return move_vertical(-1, 1);
}

bool EditorModel::move_down() {
    return move_vertical(1, 1);
}

bool EditorModel::page_up() {
    return move_page(-1);
}

bool EditorModel::page_down() {
    return move_page(1);
}

bool EditorModel::move_page(int direction) {
    history_boundary();
    if (!has_line_ || direction == 0) return false;

    const EditorVisualPosition current = current_visual_position();
    std::size_t screen_row = 0;
    if (!visual_screen_row(current, screen_row)) screen_row = 0;

    EditorVisualPosition new_top = visual_top_;
    bool shifted = false;
    for (std::size_t i = 0; i < visible_lines_; ++i) {
        const bool step = direction < 0
            ? previous_visual_position(new_top)
            : next_visual_position(new_top);
        if (!step) break;
        shifted = true;
    }
    if (!shifted) return false;

    EditorVisualPosition target = new_top;
    for (std::size_t i = 0; i < screen_row; ++i) {
        if (!next_visual_position(target)) break;
    }

    if (!move_to_visual(target, false)) return false;
    // Page movement scrolls the document while keeping the cursor on the same
    // screen row whenever enough document rows remain.
    visual_top_ = new_top;
    ensure_visual_cursor_visible();
    return true;
}

bool EditorModel::insert_char(char value) {
    if (!has_line_) return fail("PROGRAM EMPTY - INSERT LINE FIRST");
    if (value < 0x20 || value > 0x7e) return false;
    if (length_ >= document_.max_body_length()) {
        return fail(document_.max_body_length() < 2047
            ? "LINE TOO LONG FOR RAM PROGRAM STORAGE"
            : "SD LINE TOO LONG");
    }
    const bool new_group = !history_group_matches(EditGroup::Insert);
    if (new_group) capture_current(history_snapshot_);
    std::memmove(body_ + cursor_ + 1, body_ + cursor_, length_ - cursor_ + 1);
    body_[cursor_++] = value;
    ++length_;
    preferred_visual_column_ = cursor_visual_column();
    line_dirty_ = true;
    if (new_group) record_edit(history_snapshot_, EditGroup::Insert);
    else edit_group_cursor_ = cursor_;
    ensure_visual_cursor_visible();
    error_ = "OK";
    return true;
}

bool EditorModel::backspace() {
    if (!has_line_ || cursor_ == 0) return false;
    const bool new_group = !history_group_matches(EditGroup::Backspace);
    if (new_group) capture_current(history_snapshot_);
    std::memmove(
        body_ + cursor_ - 1,
        body_ + cursor_,
        length_ - cursor_ + 1);
    --cursor_;
    --length_;
    preferred_visual_column_ = cursor_visual_column();
    line_dirty_ = true;
    if (new_group) record_edit(history_snapshot_, EditGroup::Backspace);
    else edit_group_cursor_ = cursor_;
    ensure_visual_cursor_visible();
    error_ = "OK";
    return true;
}

bool EditorModel::delete_char() {
    if (!has_line_ || cursor_ >= length_) return false;
    const bool new_group = !history_group_matches(EditGroup::Delete);
    if (new_group) capture_current(history_snapshot_);
    std::memmove(
        body_ + cursor_,
        body_ + cursor_ + 1,
        length_ - cursor_);
    --length_;
    preferred_visual_column_ = cursor_visual_column();
    line_dirty_ = true;
    if (new_group) record_edit(history_snapshot_, EditGroup::Delete);
    else edit_group_cursor_ = cursor_;
    ensure_visual_cursor_visible();
    error_ = "OK";
    return true;
}

bool EditorModel::commit() {
    if (!line_dirty_) return true;
    if (!document_.set_line(number_, body_)) return fail(document_.error());
    line_dirty_ = false;
    error_ = "OK";
    return true;
}

void EditorModel::history_boundary() {
    edit_group_ = EditGroup::None;
}

bool EditorModel::history_group_matches(EditGroup group) const {
    return edit_group_ == group && has_line_ &&
        edit_group_line_ == number_ && edit_group_cursor_ == cursor_;
}

void EditorModel::capture_current(EditorHistorySnapshot& snapshot) const {
    snapshot = {};
    snapshot.state_id = state_id_;
    snapshot.line_number = number_;
    snapshot.line_index = static_cast<std::uint32_t>(index_);
    snapshot.length = static_cast<std::uint16_t>(length_);
    snapshot.cursor = static_cast<std::uint16_t>(
        std::min(cursor_, length_));
    snapshot.existed = has_line_;
    if (has_line_) std::memcpy(snapshot.body, body_, length_ + 1);
}

bool EditorModel::capture_line(
    std::int32_t line_number,
    EditorHistorySnapshot& snapshot
) {
    snapshot = {};
    snapshot.state_id = state_id_;
    snapshot.line_number = line_number;
    const std::size_t count = document_.line_count();
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t number = 0;
        std::size_t length = 0;
        const char* source = nullptr;
        editor_perf::body_read();
        if (!document_.read_line(i, number, source, length)) {
            return fail(document_.error());
        }
        if (number < line_number) continue;
        snapshot.line_index = static_cast<std::uint32_t>(i);
        if (number != line_number) return true;
        if (length >= sizeof(snapshot.body)) return fail("SD LINE TOO LONG");
        snapshot.existed = true;
        snapshot.length = static_cast<std::uint16_t>(length);
        snapshot.cursor = static_cast<std::uint16_t>(
            has_line_ && number_ == line_number
                ? std::min(cursor_, length) : 0);
        std::memcpy(snapshot.body, source, length + 1);
        return true;
    }
    snapshot.line_index = static_cast<std::uint32_t>(count);
    return true;
}

void EditorModel::record_edit(
    const EditorHistorySnapshot& before,
    EditGroup group
) {
    // Allocate a new identity even after Undo. This prevents a divergent
    // edit from colliding with an older saved state at the same history depth.
    state_id_ = next_state_id_++;
    if (history_ready()) {
        history_->clear_redo();
        if (!history_->push_undo(before)) {
            history_fault_ = true;
            history_ = nullptr;
        }
    }
    edit_group_ = group;
    edit_group_line_ = number_;
    edit_group_cursor_ = cursor_;
}

bool EditorModel::restore_snapshot(
    const EditorHistorySnapshot& snapshot
) {
    std::size_t existing_index = 0;
    bool exact = false;
    if (!locate_line(snapshot.line_number, existing_index, exact)) return false;
    if (snapshot.existed) {
        if (!document_.set_line(snapshot.line_number, snapshot.body)) {
            return fail(document_.error());
        }
    } else if (exact) {
        if (!document_.erase_line(snapshot.line_number)) {
            return fail(document_.error());
        }
    }

    const std::size_t count = document_.line_count();
    if (count == 0) {
        has_line_ = false;
        line_dirty_ = false;
        body_[0] = '\0';
        length_ = cursor_ = preferred_visual_column_ = 0;
        index_ = 0;
        visual_top_ = {};
        cursor_screen_row_ = 0;
        cursor_screen_row_valid_ = false;
        error_ = "OK";
        return true;
    }

    std::size_t target = std::min<std::size_t>(snapshot.line_index, count - 1);
    if (snapshot.existed) {
        if (!locate_line(snapshot.line_number, target, exact) || !exact) {
            return fail("UNDO LINE NOT FOUND");
        }
    }
    if (!load_index(target, false)) return false;
    if (snapshot.existed && number_ == snapshot.line_number) {
        cursor_ = std::min<std::size_t>(snapshot.cursor, length_);
        preferred_visual_column_ = cursor_visual_column();
        ensure_visual_cursor_visible();
    }
    error_ = "OK";
    return true;
}

bool EditorModel::apply_history(bool redo_action) {
    history_boundary();
    if (!history_ready()) {
        return fail(history_fault_
            ? "UNDO DISABLED: PSRAM ERROR"
            : "UNDO REQUIRES PICOCALC PSRAM");
    }
    const std::size_t available = redo_action
        ? history_->redo_count() : history_->undo_count();
    if (available == 0) {
        return fail(redo_action ? "NOTHING TO REDO" : "NOTHING TO UNDO");
    }
    if (!commit()) return false;
    const bool popped = redo_action
        ? history_->pop_redo(history_snapshot_)
        : history_->pop_undo(history_snapshot_);
    if (!popped) {
        history_fault_ = true;
        history_ = nullptr;
        return fail("UNDO DISABLED: PSRAM ERROR");
    }
    if (!capture_line(history_snapshot_.line_number, history_swap_)) return false;
    const bool saved = redo_action
        ? history_->push_undo(history_swap_)
        : history_->push_redo(history_swap_);
    if (!saved) {
        history_fault_ = true;
        history_ = nullptr;
        return fail("UNDO DISABLED: PSRAM ERROR");
    }
    if (!restore_snapshot(history_snapshot_)) return false;
    state_id_ = history_snapshot_.state_id;
    error_ = "OK";
    return true;
}

bool EditorModel::undo() {
    return apply_history(false);
}

bool EditorModel::redo() {
    return apply_history(true);
}

void EditorModel::mark_saved() {
    history_boundary();
    saved_state_id_ = state_id_;
}

bool EditorModel::locate_line(
    std::int32_t requested,
    std::size_t& index,
    bool& exact
) {
    const std::size_t count = document_.line_count();
    if (count == 0) {
        index = 0;
        exact = false;
        return true;
    }
    std::size_t length = 0;
    std::int32_t number = 0;
    for (std::size_t i = 0; i < count; ++i) {
        editor_perf::metadata_read();
        if (!document_.line_metadata(i, number, length)) {
            return fail(document_.error());
        }
        if (number >= requested) {
            index = i;
            exact = number == requested;
            return true;
        }
    }
    index = count - 1;
    exact = false;
    return true;
}

bool EditorModel::insert_line(std::int32_t number) {
    history_boundary();
    if (number < 0) return fail("BAD LINE NUMBER");
    if (!commit()) return false;
    std::size_t target = 0;
    bool exact = false;
    if (!locate_line(number, target, exact)) return false;
    if (exact) return fail("LINE ALREADY EXISTS");
    history_snapshot_ = {};
    history_snapshot_.state_id = state_id_;
    history_snapshot_.line_number = number;
    history_snapshot_.line_index = static_cast<std::uint32_t>(target);
    history_snapshot_.existed = false;
    if (!document_.set_line(number, "")) return fail(document_.error());
    record_edit(history_snapshot_, EditGroup::None);
    if (!locate_line(number, target, exact) || !exact) {
        return fail("INSERTED LINE NOT FOUND");
    }
    preferred_visual_column_ = 0;
    return load_index(target, true);
}

bool EditorModel::delete_line() {
    history_boundary();
    if (!has_line_) return false;
    if (!commit()) return false;
    capture_current(history_snapshot_);
    const std::int32_t removed = number_;
    const std::size_t old_index = index_;
    if (!document_.erase_line(removed)) return fail(document_.error());
    record_edit(history_snapshot_, EditGroup::None);
    if (document_.line_count() == 0) {
        has_line_ = false;
        line_dirty_ = false;
        body_[0] = '\0';
        length_ = cursor_ = preferred_visual_column_ = 0;
        index_ = 0;
        visual_top_ = {};
        cursor_screen_row_ = 0;
        cursor_screen_row_valid_ = false;
        error_ = "OK";
        return true;
    }
    const std::size_t next = std::min(old_index, document_.line_count() - 1);
    preferred_visual_column_ = 0;
    return load_index(next, true);
}

const char* EditorModel::find_case_insensitive(
    const char* text,
    std::size_t length,
    const char* term,
    std::size_t start
) {
    if (!text || !term || !*term || start > length) return nullptr;
    const std::size_t needle = std::strlen(term);
    if (needle > length) return nullptr;
    for (std::size_t i = start; i + needle <= length; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle; ++j) {
            const auto a = static_cast<unsigned char>(text[i + j]);
            const auto b = static_cast<unsigned char>(term[j]);
            if (std::toupper(a) != std::toupper(b)) {
                match = false;
                break;
            }
        }
        if (match) return text + i;
    }
    return nullptr;
}

const char* EditorModel::rfind_case_insensitive(
    const char* text,
    std::size_t length,
    const char* term,
    std::size_t end
) {
    if (!text || !term || !*term) return nullptr;
    const std::size_t needle = std::strlen(term);
    if (needle > length) return nullptr;
    const std::size_t limit = std::min(end, length);
    if (needle > limit) return nullptr;
    for (std::size_t i = limit - needle + 1; i-- != 0;) {
        bool match = true;
        for (std::size_t j = 0; j < needle; ++j) {
            const auto a = static_cast<unsigned char>(text[i + j]);
            const auto b = static_cast<unsigned char>(term[j]);
            if (std::toupper(a) != std::toupper(b)) {
                match = false;
                break;
            }
        }
        if (match) return text + i;
    }
    return nullptr;
}

bool EditorModel::find(const char* term, bool next) {
    history_boundary();
    if (!has_line_) return fail("PROGRAM EMPTY");
    if (!commit()) return false;
    if (!next) {
        if (!term || !*term) return fail("EMPTY SEARCH");
        std::snprintf(find_term_, sizeof(find_term_), "%s", term);
    } else if (!*find_term_) {
        return fail("NO PREVIOUS SEARCH");
    }

    const std::size_t count = document_.line_count();
    for (std::size_t i = index_; i < count; ++i) {
        std::int32_t number = 0;
        std::size_t length = 0;
        const char* source = nullptr;
        editor_perf::body_read();
        if (!document_.read_line(i, number, source, length)) {
            return fail(document_.error());
        }
        const std::size_t start = i == index_
            ? std::min(cursor_ + (next ? 1u : 0u), length)
            : 0;
        const char* match = find_case_insensitive(
            source, length, find_term_, start);
        if (!match) continue;
        const std::size_t found = static_cast<std::size_t>(match - source);
        if (!load_index(i, false)) return false;
        cursor_ = found;
        preferred_visual_column_ = cursor_visual_column();
        ensure_visual_cursor_visible();
        error_ = "OK";
        return true;
    }
    return fail("NOT FOUND");
}

bool EditorModel::find_previous() {
    history_boundary();
    if (!has_line_) return fail("PROGRAM EMPTY");
    if (!commit()) return false;
    if (!*find_term_) return fail("NO PREVIOUS SEARCH");

    for (std::size_t reverse = index_ + 1; reverse-- != 0;) {
        std::int32_t number = 0;
        std::size_t length = 0;
        const char* source = nullptr;
        editor_perf::body_read();
        if (!document_.read_line(reverse, number, source, length)) {
            return fail(document_.error());
        }
        const std::size_t end = reverse == index_
            ? std::min(cursor_, length)
            : length;
        const char* match = rfind_case_insensitive(
            source, length, find_term_, end);
        if (!match) continue;
        const std::size_t found = static_cast<std::size_t>(match - source);
        if (!load_index(reverse, false)) return false;
        cursor_ = found;
        preferred_visual_column_ = cursor_visual_column();
        ensure_visual_cursor_visible();
        error_ = "OK";
        return true;
    }
    return fail("NOT FOUND");
}

bool EditorModel::goto_line(std::int32_t requested) {
    history_boundary();
    if (!commit()) return false;
    if (document_.line_count() == 0) return fail("PROGRAM EMPTY");
    std::size_t target = 0;
    bool exact = false;
    if (!locate_line(requested, target, exact)) return false;
    preferred_visual_column_ = 0;
    return load_index(target, true);
}

} // namespace rmb
