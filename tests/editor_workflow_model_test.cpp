#include "editor_model.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
using namespace rmb;

struct Document final : EditorDocument {
    std::vector<std::string> rows;
    std::size_t limit = 2047;
    ProgramSourceMode mode = ProgramSourceMode::Structured;
    unsigned writes = 0;
    ProgramSourceMode source_mode() const override { return mode; }
    std::size_t line_count() const override { return rows.size(); }
    std::size_t max_body_length() const override { return limit; }
    bool line_metadata(std::size_t i, std::int32_t& n, std::size_t& l) const override {
        if (i >= rows.size()) return false;
        n = static_cast<std::int32_t>(i + 1); l = rows[i].size(); return true;
    }
    bool read_line(std::size_t i, std::int32_t& n, const char*& p, std::size_t& l) const override {
        if (!line_metadata(i,n,l)) return false;
        p = rows[i].c_str(); return true;
    }
    bool set_line(std::int32_t n, const char* p) override {
        assert(n > 0 && static_cast<std::size_t>(n) <= rows.size());
        assert(std::strlen(p) <= limit); rows[n-1] = p; ++writes; return true;
    }
    bool erase_line(std::int32_t) override { return false; }
    const char* error() const override { return "READ ERROR"; }
    bool replace_source_rows(std::size_t first, std::size_t remove,
                             const char* const* source, std::size_t insert) override {
        rows.erase(rows.begin()+first, rows.begin()+first+remove);
        for (std::size_t i=0; i<insert; ++i) rows.insert(rows.begin()+first+i,source[i]);
        ++writes; return true;
    }
};
struct History final : EditorHistory {
    std::vector<EditorHistorySnapshot> u,r;
    bool ready() const override {return true;}
    void clear() override {u.clear();r.clear();}
    void clear_redo() override {r.clear();}
    bool push_undo(const EditorHistorySnapshot& s) override {u.push_back(s);return true;}
    bool pop_undo(EditorHistorySnapshot& s) override {if(u.empty())return false;s=u.back();u.pop_back();return true;}
    bool push_redo(const EditorHistorySnapshot& s) override {r.push_back(s);return true;}
    bool pop_redo(EditorHistorySnapshot& s) override {if(r.empty())return false;s=r.back();r.pop_back();return true;}
    std::size_t undo_count() const override {return u.size();}
    std::size_t redo_count() const override {return r.size();}
};

void outdent_tests() {
    for (std::size_t limit : {191u,2047u}) {
        for (std::size_t spaces : {0u,1u,2u,3u,4u,8u}) {
            const std::string original = std::string(spaces,' ') + "PRINT A + B";
            for (std::size_t cursor : {0u,2u,static_cast<unsigned>(original.size())}) {
                Document d; d.limit=limit;d.rows={original};History h;EditorModel m(d,&h);
                m.configure_viewport(33,46);assert(m.begin());
                for (std::size_t i=0;i<cursor;++i) assert(m.move_right());
                assert(m.outdent());const auto removed=std::min<std::size_t>(4,spaces);
                assert(std::string(m.body())==original.substr(removed));
                assert(m.cursor()==(cursor>removed?cursor-removed:0));
                assert(m.changed()==(removed!=0));assert(m.undo_count()==(removed?1:0));
                if (removed) {
                    assert(m.undo());assert(std::string(m.body())==original);
                    assert(m.cursor()==cursor&&!m.changed());
                    assert(m.redo());assert(std::string(m.body())==original.substr(removed));
                    assert(m.changed());
                }
            }
        }
        Document d;d.limit=limit;d.rows={"    "+std::string(limit-4,'X')};History h;EditorModel m(d,&h);
        m.configure_viewport(33,46);assert(m.begin());assert(m.move_end());
        assert(m.outdent()&&m.length()==limit-4);assert(m.undo()&&m.length()==limit);
        assert(m.redo()&&m.length()==limit-4);
    }
    Document d;d.mode=ProgramSourceMode::ClassicNumbered;d.rows={"    PRINT 1"};
    EditorModel m(d);assert(m.begin());assert(!m.outdent());assert(!m.matching_block());
    assert(d.writes==0&&std::string(m.body())==d.rows[0]);
}

void match(const std::vector<std::string>& rows,
           const std::vector<std::pair<std::size_t,std::size_t>>& pairs) {
    Document d;d.rows=rows;History h;EditorModel m(d,&h);m.configure_viewport(33,46);
    assert(m.begin());
    for (const auto& pair : pairs) {
        assert(m.goto_line(static_cast<std::int32_t>(pair.first+1)));
        assert(m.matching_block());assert(m.current_index()==pair.second);
        assert(m.cursor()==0&&!m.changed()&&h.undo_count()==0&&d.writes==0);
    }
    assert(d.rows==rows);
}
void matching_tests() {
    match({"SELECT CASE A","CASE 1","PRINT 1","CASE ELSE","PRINT 2","END SELECT"},
          {{0,5},{5,0},{1,0},{3,0}});
    match({"select case A","case 1","SELECT CASE B","CASE 2","CASE ELSE","END SELECT",
           "CASE ELSE","END SELECT"},{{0,7},{7,0},{1,0},{2,5},{5,2},{3,2},{4,2},{6,0}});
    match({"FUNCTION F()","FOR I=1 TO 2","SELECT CASE I","CASE 1","IF 1 THEN","END IF",
           "CASE ELSE","DO","EXIT DO","LOOP","END SELECT","NEXT I","RETURN 0","END FUNCTION"},
          {{0,13},{13,0},{1,11},{11,1},{2,10},{10,2},{3,2},{6,2},{4,5},{5,4},{7,9},{9,7}});
    match({"SELECT CASE A","CASE 1","PRINT \"SELECT CASE:CASE ELSE:END SELECT\"",
           "REM SELECT CASE X:END SELECT","' CASE ELSE", "SELECTOR=1:CASEVALUE=2:ENDSELECT=3",
           "CASE ELSE","END SELECT"},{{0,7},{7,0},{1,0},{6,0}});
    {
        std::vector<std::string> rows;
        for(int i=0;i<16;++i){rows.push_back("SELECT CASE 1");rows.push_back("CASE 1");}
        for(int i=0;i<16;++i)rows.push_back("END SELECT");
        match(rows,{{0,47},{47,0},{30,32},{32,30},{31,30}});
    }
    match({"IF 1 THEN","PRINT 1","END IF"},{{0,2},{2,0}});
    match({"IF 1 THEN","IF 2 THEN","ELSEIF 3 THEN","ELSE","END IF","ELSE","END IF"},
          {{0,6},{6,0},{1,4},{4,1},{2,1},{3,1},{5,0}});
    match({"FOR I=1 TO 2","FOR J=1 TO 2","NEXT J","NEXT I"},{{0,3},{3,0},{1,2},{2,1}});
    match({"WHILE 1","WHILE 2","WEND","WEND"},{{0,3},{3,0},{1,2},{2,1}});
    match({"DO WHILE 1","DO","LOOP UNTIL 1","LOOP"},{{0,3},{3,0},{1,2},{2,1}});
    match({"function F(A)","if A then","FOR I=1 TO 2","NEXT I","END IF","RETURN A","end function"},
          {{0,6},{6,0},{1,4},{4,1},{2,3},{3,2}});
    match({"IF 1 THEN","PRINT \"END IF:FOR\"","REM END IF: NEXT", "' DO LOOP",
           "DIFF=1:BEFORE=2:FUNCTIONAL=3:ENDIF=4:FOR_=5:FUNCTION$=6",
           "IF 0 THEN PRINT \"THEN\"", "IF 1 THEN REM END IF", "IF 1 THEN ' END IF", "END IF"},{{0,8},{8,0}});
    // Colons inside strings and comments never begin a boundary. Standalone
    // colon statements may contain loops; an ambiguous boundary row fails.
    match({"PRINT \"FOR:NEXT\":FOR I=1 TO 2", "NEXT I:PRINT 1"},{{0,1},{1,0}});
    const std::vector<std::vector<std::string>> broken={
        {"CASE 1"},{"END SELECT"},{"SELECT CASE A","CASE ELSE","CASE 1","END SELECT"},
        {"SELECT CASE A","CASE ELSE","CASE ELSE","END SELECT"},
        {"SELECT CASE A","CASE 1","IF 1 THEN","CASE 2","END IF","END SELECT"},
        {"SELECTOR=1"},{"CASEVALUE=1"},{"REM SELECT CASE 1"},{"PRINT \"END SELECT\""},
        {"IF 1 THEN","NEXT"},{"FOR I=1 TO 2"},{"END IF"},
        {"IF 1 THEN","ELSE","ELSEIF 1 THEN","END IF"},
        {"IF 1 THEN","ELSE","ELSE","END IF"},
        {"FUNCTION F()","FUNCTION G()","END FUNCTION","END FUNCTION"},
        {"PRINT 1"},{"IF 1 THEN PRINT 1"},{"FOR I=1 TO 2:NEXT I"}
    };
    for (const auto& rows : broken) {
        Document d;d.rows=rows;History h;EditorModel m(d,&h);assert(m.begin());
        assert(m.insert_char(' '));const auto text=std::string(m.body());
        const auto cursor=m.cursor(), undo=m.undo_count();
        assert(!m.matching_block());assert(d.rows==rows&&d.writes==0);
        assert(m.current_index()==0&&m.cursor()==cursor&&std::string(m.body())==text);
        assert(m.undo_count()==undo&&m.line_dirty());
    }
    // Scan the working buffer, not a stale committed row.
    Document d;d.rows={"IF 1 THE","END IF"};EditorModel m(d);assert(m.begin());
    assert(m.move_end()&&m.insert_char('N'));assert(m.matching_block());
    assert(m.current_index()==1&&d.rows[0]=="IF 1 THEN");
    // 1024 maximum-length rows without allocating a source-sized temporary.
    d.rows.assign(1024,"REM "+std::string(2043,'X'));
    d.rows.front()="IF 1 THEN";d.rows.back()="END IF";
    assert(m.begin());assert(m.matching_block()&&m.current_index()==1023);
    assert(m.matching_block()&&m.current_index()==0);
    d.limit=191;d.rows.assign(256,"REM "+std::string(187,'X'));
    d.rows.front()="DO";d.rows.back()="LOOP";assert(m.begin());
    assert(m.matching_block()&&m.current_index()==255);
}
int main(){outdent_tests();matching_tests();std::puts("Editor Outdent/history/block navigation: PASS");}
