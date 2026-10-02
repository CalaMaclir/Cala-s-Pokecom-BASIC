#include "path_compaction.hpp"
#include "full_screen_editor.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor_perf.hpp"
#include "line_editor.hpp"
#include "menu_scroll.hpp"
#include "platform.hpp"
#include "storage.hpp"

namespace rmb {
namespace {

constexpr int kEnter = 0x0a;
constexpr int kCarriageReturn = 0x0d;
constexpr int kBackspace = 0x08;
constexpr int kTab = 0x09;
constexpr int kEscape = 0xb1;
constexpr int kLeft = 0xb4;
constexpr int kUp = 0xb5;
constexpr int kDown = 0xb6;
constexpr int kRight = 0xb7;
constexpr int kHome = 0xd2;
constexpr int kDelete = 0xd4;
constexpr int kEnd = 0xd5;
constexpr int kF1 = 0x81;
constexpr int kF2 = 0x82;
constexpr int kF3 = 0x83;
constexpr int kF4 = 0x84;
constexpr int kF5 = 0x85;
constexpr int kF6 = 0x86;
constexpr int kF7 = 0x87;
constexpr int kF8 = 0x88;
constexpr int kF9 = 0x89;
constexpr int kF10 = 0x90;

bool enter_key(int key) {
    return key == kEnter || key == kCarriageReturn;
}

bool same_text(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

// PicoCalc reports Shift+F1..F5 as F6..F10. External keyboards can
// instead report F1..F5 with the Shift modifier still held, so normalize
// that representation to the same editor command layer.
int editor_command_key(int key, bool shift) {
    if (!shift) return key;
    switch (key) {
    case kF1: return kF6;
    case kF2: return kF7;
    case kF3: return kF8;
    case kF4: return kF9;
    case kF5: return kF10;
    default: return key;
    }
}

} // namespace

FullScreenEditor::FullScreenEditor(
    ProgramStore& program,
    char* filename,
    std::size_t filename_capacity,
    bool& program_dirty
) :
    program_(program),
    filename_(filename),
    filename_capacity_(filename_capacity),
    program_dirty_(program_dirty),
    entry_dirty_(program_dirty),
    document_(program),
    model_(document_, &history_) {
    model_.configure_viewport(kVisibleLines, body_columns());
}

void FullScreenEditor::set_message(const char* text) {
    std::snprintf(message_, sizeof(message_), "%s", text ? text : "");
    platform::draw_text_row(37, message_, 0xffff80, 0x000000);
}

void FullScreenEditor::set_model_error() {
    set_message(model_.error());
}

bool FullScreenEditor::effective_dirty() const {
    return entry_dirty_ || model_.changed() || model_.line_dirty();
}

bool FullScreenEditor::sync_committed_dirty() {
    const bool dirty = entry_dirty_ || model_.changed();
    if (!program_.set_dirty(dirty)) {
        program_dirty_ = true;
        set_message(program_.error());
        return false;
    }
    program_dirty_ = dirty;
    return true;
}

void FullScreenEditor::render_header() {
    char row[80] = {};
    const bool dirty = effective_dirty() || program_.is_dirty();
    char label[80]={};
    const bool named=filename_&&*filename_&&!file_paths::same(filename_,"UNTITLED");
    if(named)file_paths::compact_root_path(filename_,dirty?27:29,
        file_paths::CompactPathPolicy::ProgramName,label,sizeof(label));
    else std::snprintf(label,sizeof(label),"UNTITLED");
    std::snprintf(
        row, sizeof(row), "CPB v0.92 [%s] %s%s",
        model_.structured() ? "STRUCTURED" : "CLASSIC",
        label,
        dirty ? " *" : "");
    platform::draw_text_row(0, row, 0xffffff, 0x203060);

    render_location();
    platform::draw_text_row(
        2,
        "ARROWS  HOME/END  TAB INDENT  SHIFT+UP/DN PAGE  ESC",
        0xa0a0a0,
        0x000000);
}

void FullScreenEditor::render_location(bool lower_row) {
    char row[80] = {};

    if (model_.has_line()) {
        std::snprintf(
            row, sizeof(row), model_.structured() ? "ROW:%ld Col:%lu INS %s U:%lu R:%lu" : "Ln:%ld Col:%lu INS %s U:%lu R:%lu",
            static_cast<long>(model_.structured()?model_.current_index()+1:model_.line_number()),
            static_cast<unsigned long>(model_.cursor() + 1),
            program_.backend_type() == ProgramBackend::Sd ? "SD" : "RAM",
            static_cast<unsigned long>(model_.undo_count()),
            static_cast<unsigned long>(model_.redo_count()));
    } else {
        std::snprintf(
            row, sizeof(row), "(EMPTY PROGRAM)  INS  %s",
            program_.backend_type() == ProgramBackend::Sd ? "SD" : "RAM");
    }
    platform::draw_text_row(1, row, 0x00ff80, 0x000000);

    if (!lower_row) return;
    std::snprintf(
        row, sizeof(row), "Line %lu/%lu  Wrap row %lu",
        static_cast<unsigned long>(
            model_.has_line() ? model_.current_index() + 1 : 0),
        static_cast<unsigned long>(model_.line_count()),
        static_cast<unsigned long>(
            model_.has_line()
                ? model_.current_visual_position().subrow + 1 : 0));
    platform::draw_text_row(36, row, 0xa0a0a0, 0x000000);
}

void FullScreenEditor::render_visual_row(
    EditorVisualPosition position,
    int row_number
) {
    if (position.line >= model_.line_count()) {
        platform::draw_text_row(row_number, "", 0x00ff80, 0x000000);
        return;
    }

    std::int32_t number = 0;
    const char* body = nullptr;
    std::size_t length = 0;
    if (!model_.line_view(position.line, number, body, length)) {
        platform::draw_text_row(row_number, "?READ ERROR", 0xff8080, 0x000000);
        return;
    }

    render_visual_row_text(
        position, row_number, number, body, length);
}

void FullScreenEditor::render_visual_row_text(
    EditorVisualPosition position,
    int row_number,
    std::int32_t number,
    const char* body,
    std::size_t length
) {
    editor_perf::text_row_render();
    char row[80] = {};
    const std::size_t width = static_cast<std::size_t>(body_columns());
    const std::size_t offset = position.subrow * width;
    const std::size_t available = offset < length ? length - offset : 0;
    const std::size_t shown = available < width ? available : width;
    int prefix = 0;
    if (model_.structured()) {
        // Display source ordinals only on the first visual row of each line.
        prefix = position.subrow == 0
            ? std::snprintf(row, sizeof(row), "%05lu  ",
                static_cast<unsigned long>(position.line + 1))
            : std::snprintf(row, sizeof(row), "%7s", "");
    } else {
        prefix = position.subrow == 0
            ? std::snprintf(row, sizeof(row), "%11ld ", static_cast<long>(number))
            : std::snprintf(row, sizeof(row), "%12s", "");
    }
    if (prefix > 0 && shown != 0) {
        std::memcpy(row + prefix, body + offset, shown);
        row[prefix + shown] = '\0';
    }
    platform::draw_text_row(
        row_number,
        row,
        0x00ff80,
        0x000000);
}

bool FullScreenEditor::render_viewport_line(
    std::size_t index,
    std::int32_t number,
    const char* body,
    std::size_t length,
    void* context
) {
    auto* state = static_cast<ViewportRenderContext*>(context);
    if (!state || !state->editor || !state->valid) return true;
    FullScreenEditor& editor = *state->editor;

    if (index == editor.model_.current_index() &&
        editor.model_.has_line()) {
        number = editor.model_.line_number();
        body = editor.model_.body();
        length = editor.model_.length();
    }

    while (state->valid &&
           state->row <= kBodyLastRow &&
           state->position.line == index) {
        editor.render_visual_row_text(
            state->position, state->row, number, body, length);
        ++state->row;
        if (!editor.model_.next_visual_position(state->position)) {
            state->valid = false;
        }
    }
    return true;
}

void FullScreenEditor::position_cursor() {
    if (!model_.has_line()) {
        platform::set_cursor_position(0, kBodyFirstRow);
        return;
    }
    if (!model_.cursor_screen_row_valid()) {
        platform::set_cursor_position(0, kBodyFirstRow);
        return;
    }
    const std::size_t column = model_.cursor_visual_column();
    platform::set_cursor_position(
        prefix_columns() + static_cast<int>(column),
        kBodyFirstRow + static_cast<int>(model_.cursor_screen_row()));
}

void FullScreenEditor::render_full() {
    editor_perf::full_render();
    render_header();

    ViewportRenderContext context;
    context.editor = this;
    context.position = model_.visual_top();
    context.row = kBodyFirstRow;
    context.valid = model_.line_count() != 0;

    if (context.valid) {
        EditorVisualPosition last = context.position;
        for (int row = 1; row < kVisibleLines; ++row) {
            if (!model_.next_visual_position(last)) break;
        }
        const std::size_t line_count =
            last.line - context.position.line + 1;
        if (!program_.visit_line_range(
                context.position.line,
                line_count,
                render_viewport_line,
                &context)) {
            platform::draw_text_row(
                context.row++, "?READ ERROR", 0xff8080, 0x000000);
            context.valid = false;
        }
    }
    while (context.row <= kBodyLastRow) {
        platform::draw_text_row(
            context.row++, "", 0x00ff80, 0x000000);
    }

    platform::draw_text_row(37, message_, 0xffff80, 0x000000);
    platform::draw_text_row(
        38, "F6 SAVE AS  F7 GOTO  F8 PREV  F9 REDO  F10 DELETE",
        0xffffff, 0x203060);
    platform::draw_text_row(
        39, "F1 SAVE  F2 FIND  F3 NEXT  F4 UNDO  F5 INSERT LINE",
        0xffffff, 0x203060);
    position_cursor();
}

void FullScreenEditor::render_edge_line_number(bool downward) {
    EditorVisualPosition position = model_.visual_top();
    const int row_number = downward ? kBodyLastRow : kBodyFirstRow;
    if (downward) {
        for (int i = 1; i < kVisibleLines; ++i) {
            if (!model_.next_visual_position(position)) break;
        }
    }

    char prefix[kPrefixColumns + 1] = {};
    const int columns = prefix_columns();
    std::memset(prefix, ' ', columns);
    prefix[columns] = '\0';
    if (model_.structured() && position.line < model_.line_count()) {
        if (position.subrow == 0) {
            std::snprintf(prefix, static_cast<std::size_t>(columns + 1), "%05lu  ",
                static_cast<unsigned long>(position.line + 1));
        }
    } else if (!model_.structured() && position.line < model_.line_count() && position.subrow == 0) {
        std::int32_t number = 0;
        std::size_t length = 0;
        if (document_.line_metadata(position.line, number, length)) {
            std::snprintf(
                prefix, sizeof(prefix), "%11ld ",
                static_cast<long>(number));
        }
    }
    platform::draw_text_span(
        row_number, 0, prefix, columns, 0x00ff80, 0x000000);
}

void FullScreenEditor::navigation_burst_step(int key) {
    const EditorVisualPosition previous_top = model_.visual_top();
    const std::uint64_t model_started = platform::monotonic_micros();

    bool moved = false;
    switch (key) {
    case kLeft: moved = model_.move_left(); break;
    case kRight: moved = model_.move_right(); break;
    case kUp: moved = model_.move_up(); break;
    case kDown: moved = model_.move_down(); break;
    default: return;
    }
    if (!moved) return;
    editor_perf::navigation_step();
    editor_perf::model_time(
        platform::monotonic_micros() - model_started);
    if (navigation_burst_active_) editor_perf::repeat_event();

    const EditorVisualPosition current_top = model_.visual_top();
    const bool viewport_changed =
        current_top.line != previous_top.line ||
        current_top.subrow != previous_top.subrow;
    if (viewport_changed) {
        navigation_viewport_changed_ = true;
        if (key == kUp || key == kDown) {
            render_edge_line_number(key == kDown);
        }
    }

    const std::uint32_t now = platform::monotonic_millis();
    if (!navigation_burst_active_ ||
        static_cast<std::uint32_t>(
            now - navigation_last_paint_ms_) >= 25u) {
        const std::uint64_t cursor_started = platform::monotonic_micros();
        platform::set_cursor_visible(false);
        position_cursor();
        platform::set_cursor_visible(true);
        editor_perf::cursor_time(
            platform::monotonic_micros() - cursor_started);
        navigation_last_paint_ms_ = now;
    }
}

void FullScreenEditor::navigation_burst_step_thunk(
    int key,
    void* context
) {
    if (!context) return;
    static_cast<FullScreenEditor*>(context)->navigation_burst_step(key);
}

void FullScreenEditor::render_navigation(
    EditorVisualPosition previous
) {
    const EditorVisualPosition current = model_.current_visual_position();
    // Cursor-only movement must stay ahead of the keyboard MCU repeat FIFO.
    // Left/right and movement between wrapped rows perform no text-row writes.
    // When the logical source line changes, refresh only the primary location
    // row; the lower Line/Wrap row is refreshed by the next full redraw.
    if (current.line != previous.line) render_location(false);
    position_cursor();
}

bool FullScreenEditor::prompt(
    const char* title,
    const char* label,
    char* output,
    std::size_t capacity,
    const char* initial
) {
    platform::clear_lcd_color(0x000000);
    platform::set_status_area_enabled(false);
    platform::set_function_key_bar_enabled(false);
    platform::draw_text_row(0, title ? title : "EDITOR", 0xffffff, 0x203060);
    platform::set_cursor_position(0, 3);
    platform::put_string(label ? label : "");
    const std::size_t length = LineEditor::read(
        output, capacity, nullptr, initial);
    platform::set_function_key_bar_enabled(false);
    platform::clear_lcd_color(0x000000);
    render_full();
    return length != 0;
}

bool FullScreenEditor::confirm_overwrite(const char* name) {
    int selected = 0;
    while (true) {
        platform::clear_lcd_color(0x000000);
        platform::draw_text_row(0, "PROGRAM FILE EXISTS", 0xffffff, 0x203060);
        char path[file_paths::display_capacity]={},row[54]={};
        file_paths::format_root_path(name,path,sizeof(path));
        std::snprintf(row,sizeof(row),"%.53s",path);
        platform::draw_text_row(3,row,0xffff80,0x000000);
        platform::draw_text_row(4,std::strlen(path)>53?path+53:"",0xffff80,0x000000);
        platform::draw_text_row(
            6, selected == 0 ? "> Cancel" : "  Cancel",
            0xffffff, selected == 0 ? 0x204080 : 0x000000);
        platform::draw_text_row(
            7, selected == 1 ? "> Overwrite" : "  Overwrite",
            0xffffff, selected == 1 ? 0x204080 : 0x000000);
        const int key = platform::get_char();
        if (key == kUp || key == kDown) selected = 1 - selected;
        else if (enter_key(key)) return selected == 1;
        else if (key == kEscape || key == 0x1b) return false;
    }
}

bool FullScreenEditor::save(bool save_as) {
    if (!model_.commit()) {
        set_model_error();
        return false;
    }
    program_dirty_ = entry_dirty_ || model_.changed();

    char name[80] = {};
    const bool has_name = filename_ && *filename_ &&
        std::strcmp(filename_, "UNTITLED") != 0;
    if (!save_as && has_name) {
        std::snprintf(name, sizeof(name), "%s", filename_);
    } else {
        if (!prompt(
                "SAVE PROGRAM AS",
                "Filename: ",
                name,
                sizeof(name),
                has_name ? filename_ : nullptr)) {
            return false;
        }
        if (storage::program_exists(name) && !confirm_overwrite(name)) {
            platform::clear_lcd_color(0x000000);
            render_full();
            return false;
        }
    }

    if (!storage::save_program(name, program_)) {
        set_message(storage::last_error());
        return false;
    }
    std::snprintf(
        filename_, filename_capacity_, "%s", program_.filename());
    program_dirty_ = false;
    entry_dirty_ = false;
    model_.mark_saved();
    char saved[80] = {};
    char label[48]={};file_paths::compact_root_path(filename_,47,
        file_paths::CompactPathPolicy::FullName,label,sizeof(label));
    std::snprintf(saved,sizeof(saved),"SAVED %s",label);
    set_message(saved);
    return true;
}

bool FullScreenEditor::insert_line() {
    if(model_.structured()) { if(!model_.insert_line(0)){set_model_error();return false;}return true; }
    char text[16] = {};
    if (!prompt("INSERT LINE", "Line number: ", text, sizeof(text))) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 0 || value > INT32_MAX) {
        set_message("BAD LINE NUMBER");
        return false;
    }
    if (!model_.insert_line(static_cast<std::int32_t>(value))) {
        set_model_error();
        return false;
    }
    set_message("LINE INSERTED - TYPE SOURCE TEXT");
    return true;
}


bool FullScreenEditor::split_line() {
    if(model_.structured()) { if(!model_.split_line(0)){set_model_error();return false;}return true; }
    if (!model_.has_line()) return insert_line();

    // Enter at logical column zero inserts a blank numbered line before the
    // current line. This is the only editor gesture needed to create a new
    // first line (for example line 5 before an existing line 10). At every
    // other column Enter retains the normal split-after-current semantics.
    const bool insert_before = model_.cursor() == 0;
    std::int32_t boundary = 0;
    std::size_t ignored = 0;
    bool has_boundary = false;

    if (insert_before) {
        has_boundary = model_.current_index() != 0;
        if (has_boundary && !program_.read_line_metadata(
                model_.current_index() - 1u, boundary, ignored)) {
            set_message(program_.error());
            return false;
        }
        if (model_.line_number() == 0 ||
            (has_boundary && static_cast<std::int64_t>(model_.line_number()) -
                                 boundary <= 1)) {
            set_message("NO LINE NUMBER SPACE BEFORE - SOURCE UNCHANGED");
            return false;
        }
    } else {
        has_boundary = model_.current_index() + 1u < program_.size();
        if (has_boundary && !program_.read_line_metadata(
                model_.current_index() + 1u, boundary, ignored)) {
            set_message(program_.error());
            return false;
        }
        if (model_.line_number() == INT32_MAX ||
            (has_boundary && static_cast<std::int64_t>(boundary) -
                                 model_.line_number() <= 1)) {
            set_message("NO LINE NUMBER SPACE - SOURCE UNCHANGED");
            return false;
        }
    }

    if (program_.size() >= program_.line_capacity()) {
        set_message("PROGRAM FULL - SOURCE UNCHANGED");
        return false;
    }

    char label[80] = {};
    if (insert_before) {
        if (has_boundary) {
            std::snprintf(
                label, sizeof(label), "New number (> %ld, < %ld): ",
                static_cast<long>(boundary),
                static_cast<long>(model_.line_number()));
        } else {
            std::snprintf(
                label, sizeof(label), "New number (< %ld): ",
                static_cast<long>(model_.line_number()));
        }
    } else if (has_boundary) {
        std::snprintf(
            label, sizeof(label), "New number (> %ld, < %ld): ",
            static_cast<long>(model_.line_number()),
            static_cast<long>(boundary));
    } else {
        std::snprintf(
            label, sizeof(label), "New number (> %ld): ",
            static_cast<long>(model_.line_number()));
    }

    char text[16] = {};
    if (!prompt(
            insert_before ? "INSERT LINE BEFORE - MANUAL NUMBER"
                          : "SPLIT LINE - MANUAL NUMBER",
            label, text, sizeof(text))) {
        return false;
    }

    std::uint64_t number = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' ||
            (number = number * 10u + (*p - '0')) > INT32_MAX) {
            set_message("BAD LINE NUMBER");
            return false;
        }
    }
    if (!model_.split_line(static_cast<std::int32_t>(number))) {
        set_model_error();
        return false;
    }
    set_message(insert_before
        ? "LINE INSERTED BEFORE CURRENT LINE"
        : "LINE SPLIT - SOURCE TEXT UNCHANGED");
    return true;
}

bool FullScreenEditor::confirm_delete_line() {
    if (!model_.has_line()) return false;
    int selected = 0;
    while (true) {
        platform::clear_lcd_color(0x000000);
        char title[80] = {};
        std::snprintf(
            title, sizeof(title), model_.structured() ? "DELETE ROW %ld?" : "DELETE LINE %ld?",
            static_cast<long>(model_.structured()?model_.current_index()+1:model_.line_number()));
        platform::draw_text_row(0, title, 0xffffff, 0x203060);
        platform::draw_text_row(
            5, selected == 0 ? "> Cancel" : "  Cancel",
            0xffffff, selected == 0 ? 0x204080 : 0x000000);
        platform::draw_text_row(
            6, selected == 1 ? "> Delete" : "  Delete",
            0xffffff, selected == 1 ? 0x204080 : 0x000000);
        const int key = platform::get_char();
        if (key == kUp || key == kDown) selected = 1 - selected;
        else if (enter_key(key)) {
            platform::clear_lcd_color(0x000000);
            return selected == 1;
        } else if (key == kEscape || key == 0x1b) {
            platform::clear_lcd_color(0x000000);
            return false;
        }
    }
}

bool FullScreenEditor::find(bool next, bool previous) {
    char term[EditorModel::kFindCapacity] = {};
    if (!next && !previous) {
        if (!prompt(
                "FIND", "Find: ", term, sizeof(term),
                *model_.find_term() ? model_.find_term() : nullptr)) {
            return false;
        }
    }
    const bool found = previous
        ? model_.find_previous()
        : model_.find(next ? nullptr : term, next);
    if (!found) {
        set_model_error();
        return false;
    }
    set_message(previous ? "PREVIOUS MATCH" :
        next ? "NEXT MATCH" : "MATCH FOUND");
    return true;
}

bool FullScreenEditor::goto_line() {
    char text[16] = {};
    if (!prompt(model_.structured()?"GO TO ROW":"GOTO LINE", model_.structured()?"Row: ":"Line: ", text, sizeof(text))) return false;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 0 || value > INT32_MAX) {
        set_message("BAD LINE NUMBER");
        return false;
    }
    if (!model_.goto_line(static_cast<std::int32_t>(value))) {
        set_model_error();
        return false;
    }
    set_message("GOTO COMPLETE");
    return true;
}

int FullScreenEditor::confirm_exit() {
    int selected = 2; // Cancel is the safe default.
    while (true) {
        platform::clear_lcd_color(0x000000);
        platform::draw_text_row(0, "UNSAVED CHANGES", 0xffffff, 0x203060);
        static const char* options[] = {
            "Save", "Keep Unsaved", "Cancel"
        };
        for (int i = 0; i < 3; ++i) {
            char row[40] = {};
            std::snprintf(row, sizeof(row), "%c %s", selected == i ? '>' : ' ', options[i]);
            platform::draw_text_row(
                5 + i, row, 0xffffff,
                selected == i ? 0x204080 : 0x000000);
        }
        const int key = platform::get_char();
        if (key == kUp && selected > 0) --selected;
        else if (key == kDown && selected < 2) ++selected;
        else if (enter_key(key)) return selected;
        else if (key == kEscape || key == 0x1b) return 2;
    }
}

bool FullScreenEditor::exit_requested() {
    if (!model_.commit()) {
        set_model_error();
        return false;
    }
    if (!sync_committed_dirty()) return false;
    if (!program_dirty_) return true;
    const int action = confirm_exit();
    if (action == 0) return save(false);
    if (action == 1) return true;
    platform::clear_lcd_color(0x000000);
    render_full();
    return false;
}

bool FullScreenEditor::run(std::int32_t initial_line) {
    if (!program_.ready()) return false;
    editor_perf::reset();
    (void)history_.begin();
    model_.set_history(history_.ready() ? &history_ : nullptr);
    model_.configure_viewport(kVisibleLines, body_columns());
    if (!model_.begin()) {
        history_.end();
        return false;
    }

    if (initial_line > 0 && model_.goto_line(initial_line)) {
        std::snprintf(message_, sizeof(message_), "COMPILE ERROR AT %s %ld",
            model_.structured() ? "ROW" : "LINE", static_cast<long>(initial_line));
    }

    const platform::ConsoleMode previous_mode = platform::get_console_mode();
    platform::set_console_mode(platform::ConsoleMode::Both);
    platform::set_status_area_enabled(false);
    // The editor owns rows 38 and 39 as a two-line command bar. Disable the
    // normal one-line Quick Load overlay until the editor returns to REPL.
    platform::set_function_key_bar_enabled(false);
    platform::clear_lcd_color(0x000000);
    render_full();

    bool finished = false;
    while (!finished) {
        const int key = platform::get_char();
        editor_perf::key_event();
        const bool shift = platform::shift_held();
        const int command_key = editor_command_key(key, shift);
        const EditorVisualPosition previous_position =
            model_.current_visual_position();
        const EditorVisualPosition previous_top = model_.visual_top();
        bool redraw_all = false;
        bool redraw_navigation = false;

        if (!shift &&
            (key == kLeft || key == kRight ||
             key == kUp || key == kDown)) {
            navigation_viewport_changed_ = false;
            navigation_burst_active_ = false;
            navigation_burst_step(key);
            navigation_burst_active_ = true;
            const std::uint64_t burst_started = platform::monotonic_micros();
            platform::collect_navigation_burst(
                key, navigation_burst_step_thunk, this);
            editor_perf::burst_time(
                platform::monotonic_micros() - burst_started);
            navigation_burst_active_ = false;

            platform::set_cursor_visible(false);
            if (navigation_viewport_changed_) {
                render_full();
            } else {
                render_location();
                position_cursor();
            }
            platform::set_cursor_visible(true);
            continue;
        }

        menu_scroll::Move movement = menu_scroll::Move::Up;
        if (menu_scroll::decode_move_key(
                key, shift, movement)) {
            bool moved = false;
            switch (movement) {
            case menu_scroll::Move::Up: moved = model_.move_up(); break;
            case menu_scroll::Move::Down: moved = model_.move_down(); break;
            case menu_scroll::Move::PageUp:
                moved = model_.page_up();
                redraw_all = moved;
                break;
            case menu_scroll::Move::PageDown:
                moved = model_.page_down();
                redraw_all = moved;
                break;
            }
            if (movement == menu_scroll::Move::Up ||
                movement == menu_scroll::Move::Down) {
                redraw_navigation = moved;
            }
            if (!moved && !same_text(model_.error(), "OK")) set_model_error();
        } else if (key == kLeft) {
            redraw_navigation = model_.move_left();
        } else if (key == kRight) {
            redraw_navigation = model_.move_right();
        } else if (key == kHome) {
            redraw_navigation = model_.move_home();
        } else if (key == kEnd) {
            redraw_navigation = model_.move_end();
        } else if (key == kTab) {
            redraw_all = model_.insert_tab();
            if (!redraw_all) set_model_error();
        } else if (key == kBackspace) {
            if (!model_.backspace() && !same_text(model_.error(), "OK")) set_model_error();
            redraw_all = true;
        } else if (key == kDelete) {
            if (!model_.delete_char() && !same_text(model_.error(), "OK")) set_model_error();
            redraw_all = true;
        } else if (key >= 0x20 && key <= 0x7e) {
            redraw_all = model_.insert_char(static_cast<char>(key));
            if (!redraw_all) set_model_error();
        } else if (command_key == kF1) {
            // SAVE automatically opens SAVE AS for an untitled program.
            (void)save(false);
            redraw_all = true;
        } else if (command_key == kF2) {
            (void)find(false);
            redraw_all = true;
        } else if (command_key == kF3) {
            (void)find(true);
            redraw_all = true;
        } else if (command_key == kF4) {
            if (!model_.undo()) {
                set_model_error();
            } else if (sync_committed_dirty()) {
                set_message("UNDO");
            }
            redraw_all = true;
        } else if (command_key == kF5) {
            (void)insert_line();
            redraw_all = true;
        } else if (command_key == kF6) {
            (void)save(true);
            redraw_all = true;
        } else if (command_key == kF7) {
            (void)goto_line();
            redraw_all = true;
        } else if (command_key == kF8) {
            (void)find(false, true);
            redraw_all = true;
        } else if (command_key == kF9) {
            if (!model_.redo()) {
                set_model_error();
            } else if (sync_committed_dirty()) {
                set_message("REDO");
            }
            redraw_all = true;
        } else if (command_key == kF10) {
            if (confirm_delete_line()) {
                if (!model_.delete_line()) set_model_error();
                else set_message("LINE DELETED");
            }
            redraw_all = true;
        } else if (key == kEscape || key == 0x1b) {
            finished = exit_requested();
            redraw_all = !finished;
        } else if (enter_key(key)) {
            (void)split_line();
            redraw_all = true;
        }

        if (redraw_all) {
            render_full();
        } else if (redraw_navigation) {
            const EditorVisualPosition current_top = model_.visual_top();
            if (current_top.line != previous_top.line ||
                current_top.subrow != previous_top.subrow) {
                render_full();
            } else {
                render_navigation(previous_position);
            }
        }
    }

#ifdef RMB_EDITOR_PERF
    const editor_perf::Counters& perf = editor_perf::snapshot();
    char perf_row[192] = {};
    std::snprintf(
        perf_row, sizeof(perf_row),
        "[EDITOR PERF] KEYS=%llu STEPS=%llu MODEL_US=%llu CURSOR_US=%llu "
        "SCANS=%llu META_READS=%llu BODY_READS=%llu SD_OPENS=%llu\r\n",
        static_cast<unsigned long long>(perf.key_events),
        static_cast<unsigned long long>(perf.navigation_steps),
        static_cast<unsigned long long>(perf.model_us),
        static_cast<unsigned long long>(perf.cursor_us),
        static_cast<unsigned long long>(perf.viewport_scans),
        static_cast<unsigned long long>(perf.metadata_reads),
        static_cast<unsigned long long>(perf.body_reads),
        static_cast<unsigned long long>(perf.sd_opens));
    platform::serial_put_string_raw(perf_row);
    std::snprintf(
        perf_row, sizeof(perf_row),
        "[EDITOR PERF] FULL_RENDERS=%llu TEXT_ROWS=%llu BURST_US=%llu "
        "REPEATS=%llu MAX_STEP_US=%llu\r\n",
        static_cast<unsigned long long>(perf.full_renders),
        static_cast<unsigned long long>(perf.text_row_renders),
        static_cast<unsigned long long>(perf.burst_us),
        static_cast<unsigned long long>(perf.repeat_events),
        static_cast<unsigned long long>(perf.max_step_us));
    platform::serial_put_string_raw(perf_row);
#endif

    platform::set_console_mode(previous_mode);
    platform::set_status_area_enabled(true);
    platform::set_function_key_bar_enabled(true);
    platform::clear_lcd_color(0x000000);
    history_.end();
    return true;
}

} // namespace rmb
