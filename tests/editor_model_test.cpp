#include "editor_model.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Line { std::int32_t number; std::string body; };

class FakeDocument final : public rmb::EditorDocument {
public:
    explicit FakeDocument(std::size_t maximum = 2047) : maximum_(maximum) {}
    std::size_t line_count() const override { return lines.size(); }
    std::size_t max_body_length() const override { return maximum_; }
    bool line_metadata(std::size_t index, std::int32_t& number,
                       std::size_t& length) const override {
        ++metadata_reads;
        if (index >= lines.size()) { error_ = "BAD LINE INDEX"; return false; }
        number = lines[index].number;
        length = lines[index].body.size();
        return true;
    }
    bool read_line(std::size_t index, std::int32_t& number,
                   const char*& body, std::size_t& length) const override {
        ++body_reads;
        if (index >= lines.size()) { error_ = "BAD LINE INDEX"; return false; }
        number = lines[index].number; body = lines[index].body.c_str();
        length = lines[index].body.size(); return true;
    }
    bool set_line(std::int32_t number, const char* body) override {
        if (fail_commit) { error_ = "INJECTED COMMIT FAILURE"; return false; }
        const std::string value = body ? body : "";
        if (value.size() > maximum_) {
            error_ = maximum_ < 2047
                ? "LINE TOO LONG FOR RAM PROGRAM STORAGE" : "SD LINE TOO LONG";
            return false;
        }
        auto it = std::lower_bound(lines.begin(), lines.end(), number,
            [](const Line& line, std::int32_t value) {
                return line.number < value;
            });
        if (it != lines.end() && it->number == number) it->body = value;
        else lines.insert(it, Line{number, value});
        error_ = "OK"; return true;
    }
    bool erase_line(std::int32_t number) override {
        auto it = std::find_if(lines.begin(), lines.end(),
            [number](const Line& line) { return line.number == number; });
        if (it != lines.end()) lines.erase(it);
        error_ = "OK"; return true;
    }
    const char* error() const override { return error_; }
    std::vector<Line> lines;
    bool fail_commit = false;
    mutable std::size_t metadata_reads = 0;
    mutable std::size_t body_reads = 0;
private:
    std::size_t maximum_;
    mutable const char* error_ = "OK";
};

class MemoryHistory final : public rmb::EditorHistory {
public:
    bool ready() const override { return available; }
    void clear() override { undo.clear(); redo.clear(); }
    void clear_redo() override { redo.clear(); }
    bool push_undo(const rmb::EditorHistorySnapshot& value) override {
        if (!available) return false;
        undo.push_back(value); return true;
    }
    bool pop_undo(rmb::EditorHistorySnapshot& value) override {
        if (!available || undo.empty()) return false;
        value = undo.back(); undo.pop_back(); return true;
    }
    bool push_redo(const rmb::EditorHistorySnapshot& value) override {
        if (!available) return false;
        redo.push_back(value); return true;
    }
    bool pop_redo(rmb::EditorHistorySnapshot& value) override {
        if (!available || redo.empty()) return false;
        value = redo.back(); redo.pop_back(); return true;
    }
    std::size_t undo_count() const override { return undo.size(); }
    std::size_t redo_count() const override { return redo.size(); }
    bool available = true;
    std::vector<rmb::EditorHistorySnapshot> undo;
    std::vector<rmb::EditorHistorySnapshot> redo;
};

void tab_indentation() {
    for (std::size_t column = 0; column < 8; ++column) {
        FakeDocument document;
        const std::string original = "ABCDEFGH";
        document.lines = {{10, original}};
        MemoryHistory history;
        rmb::EditorModel model(document, &history);
        model.configure_viewport(3, 5);
        assert(model.begin());
        for (std::size_t i = 0; i < column; ++i) assert(model.move_right());
        const std::size_t spaces = 4 - column % 4;
        assert(model.insert_tab());
        assert(std::string(model.body()) == original.substr(0, column) +
            std::string(spaces, ' ') + original.substr(column));
        assert(model.cursor() == column + spaces && model.undo_count() == 1);
        assert(model.undo() && std::string(model.body()) == original);
        assert(model.cursor() == column);
        assert(model.redo());
        assert(model.insert_char('X'));
        assert(model.undo()); // Adjacent typing does not join the Tab group.
        assert(model.cursor() == column + spaces);
        assert(model.undo() && std::string(model.body()) == original);
    }
    for (std::size_t limit : {191u, 2047u}) {
        for (bool fits : {false, true}) {
            FakeDocument document(limit);
            const std::string original(limit - (fits ? 4 : 2), 'A');
            document.lines = {{10, original}};
            MemoryHistory history;
            rmb::EditorModel model(document, &history);
            assert(model.begin());
            assert(model.insert_tab() == fits);
            if (fits) {
                assert(model.length() == limit && model.cursor() == 4);
                assert(model.undo() && std::string(model.body()) == original);
            } else {
                assert(std::string(model.body()) == original && model.cursor() == 0);
                assert(!model.changed() && !model.line_dirty() && model.undo_count() == 0);
            }
        }
    }
}

void cursor_and_editing() {
    FakeDocument document; document.lines = {{100, "ABCDE"}};
    rmb::EditorModel model(document); model.configure_viewport(4, 3);
    assert(model.begin()); assert(model.move_end() && model.cursor() == 5);
    assert(model.current_visual_position().subrow == 1);
    assert(model.cursor_visual_column() == 2);
    assert(model.move_left() && model.cursor() == 4);
    assert(model.move_home() && model.cursor() == 0); assert(!model.move_left());
    assert(model.insert_char('X')); assert(std::string(model.body()) == "XABCDE");
    assert(model.move_right()); assert(model.insert_char('Y'));
    assert(std::string(model.body()) == "XAYBCDE");
    assert(model.backspace()); assert(std::string(model.body()) == "XABCDE");
    assert(model.delete_char()); assert(std::string(model.body()) == "XACDE");
    assert(model.move_end()); assert(model.insert_char('Z'));
    assert(std::string(model.body()) == "XACDEZ"); assert(!model.delete_char());
    assert(model.commit()); assert(document.lines[0].body == "XACDEZ");
}

void navigation_and_viewport() {
    FakeDocument document;
    document.lines = {{10,"0123456789"},{20,"12"},{30,"0123456789"},
                      {40,"four"},{50,"five"},{60,"six"}};
    rmb::EditorModel model(document); model.configure_viewport(3, 5);
    assert(model.begin()); assert(model.move_end() && model.cursor() == 10);
    assert(model.move_down() && model.cursor() == 2);
    assert(model.move_down() && model.cursor() == 4);
    assert(model.page_down() && model.line_number() == 50);
    assert(model.page_down() && model.line_number() == 60);
    assert(!model.page_down());
    assert(model.page_up() && model.line_number() == 30);
    assert(model.page_up() && model.line_number() == 10);
    assert(model.page_up() && model.line_number() == 10);
    assert(!model.page_up());
}

void visual_wrap_boundaries_and_navigation() {
    FakeDocument document;
    document.lines = {{10,""},{20,"A"},{30,"ABC"},{40,"ABCD"},
                      {50,"ABCDE"},{60,"ABCDEFG"},{70,"ABCDEFGH"},
                      {80,"ABCDEFGHIJ"},{90,"XY"},
                      {100,std::string(2047,'L')}};
    rmb::EditorModel model(document); model.configure_viewport(3,4);
    assert(model.begin());
    assert(model.visual_row_count(0)==1);
    assert(model.visual_row_count(1)==1);
    assert(model.visual_row_count(2)==1);
    assert(model.visual_row_count(3)==1);
    assert(model.visual_row_count(4)==2);
    assert(model.visual_row_count(5)==2);
    assert(model.visual_row_count(6)==2);
    assert(model.visual_row_count(9)==512);
    assert(model.goto_line(80));
    for(int i=0;i<3;++i) assert(model.move_right());
    assert(model.cursor()==3 && model.current_visual_position().subrow==0);
    assert(model.move_right());
    assert(model.cursor()==4);
    assert(model.current_visual_position().subrow==1);
    assert(model.cursor_visual_column()==0);
    assert(model.move_right() && model.cursor()==5);
    assert(model.cursor_visual_column()==1);
    assert(model.move_left() && model.cursor()==4);
    assert(model.move_left() && model.cursor()==3);
    assert(model.move_right() && model.cursor()==4);
    assert(model.move_up() && model.cursor()==0);
    assert(model.move_down() && model.cursor()==4);
    assert(model.move_end() && model.cursor()==10);
    assert(model.current_visual_position().subrow==2);
    assert(model.cursor_visual_column()==2);
    assert(model.move_down() && model.line_number()==90 && model.cursor()==2);
    assert(model.move_up() && model.line_number()==80 && model.cursor()==10);
}

void wrapped_viewport_mapping() {
    FakeDocument document;
    document.lines = {{10,std::string(20,'A')},{20,"B"},{30,"C"}};
    rmb::EditorModel model(document); model.configure_viewport(3,4);
    assert(model.begin()); assert(model.move_end());
    const auto top=model.visual_top();
    assert(top.line==0 && top.subrow==2);
    std::size_t row=99;
    assert(model.visual_screen_row(model.current_visual_position(),row));
    assert(row==2);
    auto position=top;
    assert(model.next_visual_position(position));
    assert(position.line==0 && position.subrow==3);
    assert(model.next_visual_position(position));
    assert(position.line==0 && position.subrow==4);
}

void preferred_column_and_page_clamp() {
    FakeDocument document;
    document.lines = {{10,"ABCDEFGHIJ"},{20,"XY"},{30,"ABCDEFGHIJ"}};
    rmb::EditorModel model(document); model.configure_viewport(3,4);
    assert(model.begin());
    for(int i=0;i<3;++i) assert(model.move_right());
    assert(model.move_down() && model.cursor()==7);
    assert(model.move_down() && model.cursor()==10);
    assert(model.move_down() && model.line_number()==20 && model.cursor()==2);
    assert(model.move_down() && model.line_number()==30 && model.cursor()==3);

    FakeDocument one;
    one.lines={{10,std::string(20,'P')}};
    rmb::EditorModel page(one);page.configure_viewport(3,4);
    assert(page.begin());
    assert(page.move_down());
    std::size_t screen_row=99;
    assert(page.visual_screen_row(page.current_visual_position(),screen_row));
    assert(screen_row==1);
    assert(page.page_down() && page.current_visual_position().subrow==4);
    assert(page.visual_screen_row(page.current_visual_position(),screen_row));
    assert(screen_row==1);
    assert(page.page_down() && page.current_visual_position().subrow==4);
    assert(page.visual_screen_row(page.current_visual_position(),screen_row));
    assert(screen_row==0);
    assert(!page.page_down());
    assert(page.page_up() && page.current_visual_position().subrow==1);
    assert(page.page_up() && page.current_visual_position().subrow==0);
    assert(!page.page_up());
}

void edit_reflows_wrap_rows() {
    FakeDocument document; document.lines={{10,"ABCD"}};
    rmb::EditorModel model(document);model.configure_viewport(3,4);
    assert(model.begin() && model.visual_row_count(0)==1);
    assert(model.move_end() && model.insert_char('E'));
    assert(model.visual_row_count(0)==2);
    assert(model.backspace() && model.visual_row_count(0)==1);
    model.move_home();
    for(int i=0;i<3;++i) assert(model.move_right());
    assert(model.delete_char());
    assert(model.visual_row_count(0)==1);
    assert(model.insert_char('D'));
    assert(model.visual_row_count(0)==1);
}

void limits_and_failure() {
    FakeDocument ram(191); ram.lines = {{10, std::string(191, 'R')}};
    rmb::EditorModel ram_model(ram); assert(ram_model.begin());
    assert(ram_model.move_end()); assert(!ram_model.insert_char('X'));
    assert(std::strstr(ram_model.error(), "RAM PROGRAM STORAGE"));
    assert(ram.lines[0].body.size() == 191);

    FakeDocument sd(2047); sd.lines = {{10, std::string(2047, 'S')}};
    rmb::EditorModel sd_model(sd); sd_model.configure_viewport(10, 45);
    assert(sd_model.begin()); assert(sd_model.move_end());
    assert(sd_model.cursor() == 2047);
    assert(sd_model.current_visual_position().subrow == 45);
    assert(!sd_model.insert_char('X'));
    assert(std::strstr(sd_model.error(), "SD LINE TOO LONG"));
    assert(sd_model.move_left()); assert(sd_model.backspace());
    assert(sd_model.insert_char('Z')); assert(sd_model.commit());
    assert(sd.lines[0].body.size() == 2047 && sd.lines[0].body[2045] == 'Z');
    sd_model.move_home(); assert(sd_model.delete_char());
    assert(sd_model.insert_char('Q')); sd.fail_commit = true;
    assert(!sd_model.commit()); assert(sd_model.line_dirty());
    assert(sd_model.cursor() == 1 && std::strstr(sd_model.error(), "INJECTED"));
}

void find_and_goto() {
    FakeDocument document;
    document.lines = {{100,"PRINT alpha"},{200,"REM middle ALPHA and alpha"},
                      {300,std::string(2000,'X')+"Needle"}};
    rmb::EditorModel model(document); model.configure_viewport(10,45);
    assert(model.begin()); assert(model.find("ALPHA",false));
    assert(model.line_number()==100 && model.cursor()==6);
    assert(model.find(nullptr,true));
    assert(model.line_number()==200 && model.cursor()==11);
    assert(model.find(nullptr,true));
    assert(model.line_number()==200 && model.cursor()==21);
    assert(model.find_previous());
    assert(model.line_number()==200 && model.cursor()==11);
    assert(model.find_previous());
    assert(model.line_number()==100 && model.cursor()==6);
    assert(!model.find_previous());
    assert(!std::strcmp(model.error(),"NOT FOUND"));
    assert(model.find("needle",false));
    assert(model.line_number()==300 && model.cursor()==2000);
    assert(model.current_visual_position().subrow==44);
    assert(!model.find(nullptr,true)); assert(!std::strcmp(model.error(),"NOT FOUND"));
    assert(model.goto_line(200) && model.line_number()==200);
    assert(model.current_visual_position().subrow==0);
    assert(model.goto_line(250) && model.line_number()==300);
    assert(model.goto_line(1) && model.line_number()==100);
    assert(model.goto_line(999) && model.line_number()==300);
}

void insertion_and_deletion() {
    FakeDocument document; rmb::EditorModel model(document);
    assert(model.begin() && !model.has_line()); assert(!model.insert_char('A'));
    assert(model.insert_line(20)); assert(model.has_line() && model.line_number()==20);
    assert(model.insert_char('A') && model.commit()); assert(model.insert_line(10));
    assert(model.line_number()==10); assert(!model.insert_line(20));
    assert(!std::strcmp(model.error(),"LINE ALREADY EXISTS"));
    assert(model.goto_line(20)); assert(model.delete_line());
    assert(model.line_count()==1 && model.line_number()==10);
    assert(model.delete_line()); assert(model.line_count()==0 && !model.has_line());
}

void psram_history_undo_redo() {
    FakeDocument document; document.lines = {{10, "ABC"}};
    MemoryHistory history;
    rmb::EditorModel model(document, &history);
    model.configure_viewport(4, 4);
    assert(model.begin() && model.move_end());
    assert(model.insert_char('X') && model.insert_char('Y'));
    assert(model.undo_count() == 1 && model.redo_count() == 0);
    assert(model.commit() && document.lines[0].body == "ABCXY");
    assert(model.undo());
    assert(std::string(model.body()) == "ABC");
    assert(!model.changed() && model.undo_count() == 0 && model.redo_count() == 1);
    assert(model.redo());
    assert(std::string(model.body()) == "ABCXY" && model.changed());
    model.mark_saved(); assert(!model.changed());

    assert(model.backspace() && model.backspace());
    assert(std::string(model.body()) == "ABC");
    assert(model.undo());
    assert(std::string(model.body()) == "ABCXY" && !model.changed());
    assert(model.undo());
    assert(std::string(model.body()) == "ABC" && model.changed());
    assert(model.insert_char('Z'));
    // This is a divergent branch from before the save point. Its state must
    // remain dirty even though its history depth matches the saved branch.
    assert(model.changed());
    assert(model.redo_count() == 0);
    assert(!model.redo() && !std::strcmp(model.error(), "NOTHING TO REDO"));
}

void psram_history_lines_and_fallback() {
    FakeDocument document; document.lines = {{10, "TEN"}};
    MemoryHistory history;
    rmb::EditorModel model(document, &history);
    assert(model.begin());
    assert(model.insert_line(20));
    assert(model.line_count() == 2 && model.line_number() == 20);
    assert(model.undo());
    assert(model.line_count() == 1 && model.line_number() == 10);
    assert(model.redo());
    assert(model.line_count() == 2 && model.line_number() == 20);
    assert(model.insert_char('A') && model.insert_char('B') && model.commit());
    assert(model.delete_line());
    assert(model.line_count() == 1);
    assert(model.undo());
    assert(model.line_count() == 2 && model.line_number() == 20);
    assert(std::string(model.body()) == "AB");

    FakeDocument no_psram; no_psram.lines = {{1, "X"}};
    rmb::EditorModel fallback(no_psram);
    assert(fallback.begin() && fallback.insert_char('Y'));
    assert(!fallback.undo());
    assert(!std::strcmp(fallback.error(), "UNDO REQUIRES PICOCALC PSRAM"));
}

void cached_cursor_row_and_navigation_reads() {
    const std::size_t lengths[] = {0,1,41,42,82,100,500,1000,2047};
    for (const std::size_t length : lengths) {
        FakeDocument document;
        document.lines = {
            {10, std::string(length, 'A')},
            {20, "SHORT"},
            {30, std::string(100, 'B')}
        };
        rmb::EditorModel model(document);
        model.configure_viewport(5, 41);
        assert(model.begin());
        auto check = [&]() {
            assert(model.cursor_screen_row_valid());
            std::size_t reference = 999;
            assert(model.visual_screen_row(
                model.current_visual_position(), reference));
            assert(model.cursor_screen_row() == reference);
            assert(reference < 5);
        };
        check();

        document.body_reads = 0;
        document.metadata_reads = 0;
        while (model.move_right()) check();
        while (model.move_left()) check();
        assert(document.body_reads == 0);

        document.body_reads = 0;
        while (model.current_visual_position().subrow + 1 <
               model.visual_row_count(model.current_index())) {
            assert(model.move_down());
            check();
        }
        assert(document.body_reads == 0);

        document.body_reads = 0;
        assert(model.move_down());
        check();
        assert(model.line_number() == 20);
        assert(document.body_reads == 1);
        document.body_reads = 0;
        assert(model.move_up());
        check();
        assert(model.line_number() == 10);
        assert(document.body_reads == 1);
    }
}

} // namespace

int main() {
    tab_indentation();
    cursor_and_editing(); navigation_and_viewport();
    visual_wrap_boundaries_and_navigation(); wrapped_viewport_mapping();
    preferred_column_and_page_clamp(); edit_reflows_wrap_rows();
    limits_and_failure();
    find_and_goto(); insertion_and_deletion();
    psram_history_undo_redo(); psram_history_lines_and_fallback();
    cached_cursor_row_and_navigation_reads();
    std::puts("Editor model: PASS");
}
