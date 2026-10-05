#include "editor_model.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace rmb;
struct Document final : EditorDocument {
    std::vector<std::string> rows;
    std::size_t limit=2047;
    bool reject=false;
    ProgramSourceMode mode=ProgramSourceMode::Structured;
    ProgramSourceMode source_mode() const override { return mode; }
    std::size_t line_count() const override { return rows.size(); }
    std::size_t max_body_length() const override { return limit; }
    bool line_metadata(std::size_t i,std::int32_t& n,std::size_t& l) const override {
        if(i>=rows.size()) return false;
        n=static_cast<std::int32_t>(i+1);l=rows[i].size();return true;
    }
    bool read_line(std::size_t i,std::int32_t& n,const char*& b,std::size_t& l) const override {
        if(!line_metadata(i,n,l)) return false;
        b=rows[i].c_str();return true;
    }
    bool set_line(std::int32_t n,const char* b) override { rows.at(n-1)=b;return true; }
    bool erase_line(std::int32_t n) override { rows.erase(rows.begin()+n-1);return true; }
    const char* error() const override { return "INJECTED WRITE ERROR"; }
    bool replace_source_rows(std::size_t first,std::size_t remove,const char* const* text,std::size_t insert) override {
        if(reject)return false;
        auto next=rows;next.erase(next.begin()+first,next.begin()+first+remove);
        for(std::size_t i=0;i<insert;++i) {
            if(std::strlen(text[i])>limit) return false;
            next.insert(next.begin()+first+i,text[i]);
        }
        rows.swap(next);return true;
    }
};
struct History final : EditorHistory {
    std::vector<EditorHistorySnapshot> u,r;
    bool ready() const override{return true;}
    void clear() override{u.clear();r.clear();}
    void clear_redo() override{r.clear();}
    bool push_undo(const EditorHistorySnapshot& s) override{u.push_back(s);return true;}
    bool push_redo(const EditorHistorySnapshot& s) override{r.push_back(s);return true;}
    bool pop_undo(EditorHistorySnapshot& s) override{if(u.empty())return false;s=u.back();u.pop_back();return true;}
    bool pop_redo(EditorHistorySnapshot& s) override{if(r.empty())return false;s=r.back();r.pop_back();return true;}
    std::size_t undo_count() const override{return u.size();}
    std::size_t redo_count() const override{return r.size();}
};
int main() {
    for(const char* opening:{"IF X THEN","FOR I=1 TO 3","WHILE X","DO","SELECT CASE X","FUNCTION F()","SUB S()"}) {
        Document d;History h;d.rows={opening};const auto original=d.rows;
        EditorModel m(d,&h);assert(m.begin()&&m.move_end());
        assert(d.rows==original&&!m.changed());
        assert(m.split_line(0)&&m.cursor()==4&&d.rows.back()=="    "&&m.undo_count()==1);
        assert(m.undo()&&d.rows==original&&m.cursor()==std::strlen(opening));
        assert(m.redo()&&d.rows.back()=="    "&&m.cursor()==4);
    }
    for(const char* inert:{"PRINT \"END IF\"","' END IF","REM IF X THEN","PRINT \"THEN\"","IF X THEN PRINT X","IF X THEN ' comment","FORWARD=1","PRINT \"A:FOR B\""}) {
        Document d;d.rows={std::string("  ")+inert};EditorModel m(d);
        assert(m.begin()&&m.move_end()&&m.split_line(0));assert(d.rows.back()=="  "&&m.cursor()==2);
    }
    const std::vector<std::pair<std::string,std::string>> pairs={
        {"IF X THEN","END IF"},{"FOR I=1 TO 3","NEXT I"},{"WHILE X","WEND"},
        {"DO","LOOP"},{"SELECT CASE X","END SELECT"},{"FUNCTION F()","END FUNCTION"},{"SUB S()","END SUB"},
        {"IF X THEN","ELSE"},{"IF X THEN","ELSEIF Y THEN"},{"SELECT CASE X","CASE 1"},{"SELECT CASE X","CASE ELSE"}};
    for(const auto& pair:pairs) {
        Document d;History h;d.rows={"  "+pair.first,"      "};
        EditorModel m(d,&h);assert(m.begin()&&m.goto_line(2)&&m.move_end());
        for(char c:pair.second)assert(m.insert_char(c));
        const auto typed=std::string(m.body());const auto cursor=m.cursor();
        const auto count=m.undo_count();assert(m.split_line(0)&&m.undo_count()==count+1);
        const bool branch=pair.second.rfind("CASE",0)==0;
        const bool close=pair.second.rfind("END",0)==0||pair.second.rfind("NEXT",0)==0||pair.second=="WEND"||pair.second=="LOOP";
        const auto aligned=branch?6u:2u;
        assert(d.rows[1]==std::string(aligned,' ')+pair.second);
        assert(d.rows[2]==std::string(aligned+(close?0:4),' '));
        assert(m.undo()&&d.rows[1]==typed&&m.cursor()==cursor);
        assert(m.redo()&&d.rows[1]==std::string(aligned,' ')+pair.second);
    }
    // Existing indentation is not changed by opening or Enter on untouched code.
    {Document d;d.rows={"IF X THEN","       END IF"};EditorModel m(d);
     assert(m.begin()&&m.goto_line(2)&&m.move_end()&&m.split_line(0));assert(d.rows[1]=="       END IF");}
    // Mid-line splits preserve exact spaces; incomplete/malformed blocks are conservative.
    {Document d;d.rows={"  PRINT 1"};EditorModel m(d);assert(m.begin()&&m.move_right()&&m.move_right());
     assert(m.split_line(0)&&d.rows[0]=="  "&&d.rows[1]=="PRINT 1"&&m.cursor()==0);}
    {Document d;d.rows={"PRINT 1","    "};EditorModel m(d);assert(m.begin()&&m.goto_line(2)&&m.move_end());
     for(char c:std::string("END IF")) { assert(m.insert_char(c)); }
     assert(m.split_line(0));assert(d.rows[1]=="    END IF");}
    {Document d;History h;d.rows={"IF X THEN"};d.reject=true;EditorModel m(d,&h);assert(m.begin()&&m.move_end());
     assert(!m.split_line(0)&&d.rows.size()==1&&m.undo_count()==0&&std::string(m.body())=="IF X THEN");}
    {Document d;d.limit=12;d.rows={"         DO"};EditorModel m(d);assert(m.begin()&&m.move_end());
     assert(!m.split_line(0)&&d.rows.size()==1&&!m.changed());}
    std::puts("Structured auto-indent / dedent / comments / atomic Undo / bounds: PASS");
}
