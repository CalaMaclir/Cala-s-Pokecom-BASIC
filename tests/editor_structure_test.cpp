#include "editor_model.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
using namespace rmb;
struct Document final : EditorDocument {
    std::map<std::int32_t,std::string> lines;
    std::size_t limit = 2047, capacity = 1024;
    bool reject_pair = false;
    const char* last = "OK";
    std::size_t line_count() const override { return lines.size(); }
    std::size_t max_body_length() const override { return limit; }
    bool line_metadata(std::size_t i, std::int32_t& n, std::size_t& l) const override {
        if (i >= lines.size()) return false;
        auto it=lines.begin(); std::advance(it,i); n=it->first; l=it->second.size(); return true;
    }
    bool read_line(std::size_t i, std::int32_t& n, const char*& p, std::size_t& l) const override {
        if (!line_metadata(i,n,l)) return false;
        p=lines.at(n).c_str(); return true;
    }
    bool set_line(std::int32_t n,const char* p) override {
        if (std::strlen(p)>limit) return false;
        lines[n]=p; return true;
    }
    bool erase_line(std::int32_t n) override { lines.erase(n); return true; }
    const char* error() const override { return last; }
    bool replace_line_pair(std::int32_t a,const char* ap,std::int32_t b,const char* bp) override {
        if (reject_pair) {last="INJECTED WRITE FAILURE"; return false;}
        auto next=lines;
        for (const auto& pair : {std::make_pair(a,ap),std::make_pair(b,bp)}) {
            if (pair.second) {
                if (std::strlen(pair.second)>limit) {last="TOO LONG"; return false;}
                next[pair.first]=pair.second;
            } else next.erase(pair.first);
        }
        if (next.size()>capacity) {last="PROGRAM FULL";return false;}
        lines.swap(next);last="OK";return true;
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
void split_tests() {
    // A non-zero cursor retains normal split-after-current behavior.
    for (std::size_t position : {1u,7u,15u}) {
        Document d; History h;
        d.lines={{100,"PRINT 1:PRINT 2 "},{200,"END"}};
        const auto original=d.lines;
        EditorModel m(d,&h);m.configure_viewport(33,41);assert(m.begin());
        for(std::size_t i=0;i<position;++i)assert(m.move_right());
        assert(m.split_line(150));
        assert(d.lines.at(100)==original.at(100).substr(0,position));
        assert(d.lines.at(150)==original.at(100).substr(position));
        assert(m.line_number()==150&&m.cursor()==0&&h.undo_count()==1);
        assert(m.undo()&&d.lines==original&&m.line_number()==100&&m.cursor()==position);
        assert(m.redo()&&d.lines.size()==3&&m.line_number()==150&&m.cursor()==0);
        // Backspace at the beginning exactly reverses the textual split.
        assert(m.backspace()&&d.lines==original&&m.cursor()==position);
        assert(m.undo()&&d.lines.size()==3);
        assert(m.redo()&&d.lines==original);
    }

    // Enter at the start of the first line inserts a blank predecessor and
    // focuses it, allowing line 5 to be added before an existing line 10.
    {
        Document d;History h;
        d.lines={{10,"PRINT 1"},{20,"END"}};
        const auto original=d.lines;
        EditorModel m(d,&h);m.configure_viewport(33,41);assert(m.begin());
        assert(m.split_line(5));
        assert(d.lines.size()==3&&d.lines.at(5).empty());
        assert(d.lines.at(10)=="PRINT 1"&&m.line_number()==5&&m.cursor()==0);
        assert(m.undo()&&d.lines==original&&m.line_number()==10&&m.cursor()==0);
        assert(m.redo()&&d.lines.at(5).empty()&&m.line_number()==5);
    }

    // The same operation works between existing lines but never crosses the
    // previous/current number boundaries.
    {
        Document d;History h;
        d.lines={{5,"A"},{10,"B"},{20,"C"}};
        EditorModel m(d,&h);assert(m.begin());assert(m.goto_line(10));
        assert(m.move_home()&&m.split_line(7));
        assert(d.lines.at(7).empty()&&d.lines.at(10)=="B");
        assert(m.undo()&&m.line_number()==10);
        const auto restored=d.lines;
        for(int n : {4,5,10,11,-1}) {
            assert(!m.split_line(n)&&d.lines==restored);
        }
    }

    Document d;History h;d.lines={{100,"ABC"},{101,"DEF"}};
    EditorModel m(d,&h);assert(m.begin());assert(m.move_right());
    const auto initial=d.lines;
    for(int n : {99,100,101,102,-1})assert(!m.split_line(n)&&d.lines==initial);
    assert(m.undo_count()==0&&!m.changed());
    d.lines={{INT32_MAX,"LAST"}};assert(m.begin());assert(m.move_end());
    assert(!m.split_line(INT32_MAX));
    d.lines={{0,"ZERO"}};assert(m.begin());assert(!m.split_line(-1));
    assert(!m.split_line(0));
    d.lines={{10,"ABC"}};d.capacity=1;assert(m.begin());assert(m.move_right());
    assert(!m.split_line(20)&&d.lines.size()==1&&std::string(m.body())=="ABC"&&m.cursor()==1);
}
void join_tests() {
    for(std::size_t limit : {191u,2047u}) {
        Document d;History h;d.limit=limit;
        d.lines={{10,std::string(limit-3,'A')},{20," B "}};
        const auto initial=d.lines;
        EditorModel m(d,&h);m.configure_viewport(33,41);assert(m.begin());assert(m.move_end());
        assert(m.delete_char());assert(d.lines.size()==1&&d.lines.at(10).size()==limit);
        assert(d.lines.at(10)==initial.at(10)+initial.at(20));
        assert(m.undo()&&d.lines==initial);assert(m.redo());assert(m.undo());
        assert(m.goto_line(20));assert(m.move_home());
        assert(m.backspace()&&d.lines.at(10).size()==limit&&m.cursor()==limit-3);
        assert(m.undo()&&d.lines==initial);
        d.lines[20]=" B  ";assert(m.begin());assert(m.move_end());const auto too_long=d.lines;
        assert(!m.delete_char()&&d.lines==too_long&&m.undo_count()==0);
        assert(std::strstr(m.error(),"TOO LONG"));
        assert(m.goto_line(20)&&m.move_home());assert(!m.backspace()&&d.lines==too_long);
    }
    Document d;History h;d.lines={{10,"REM HELLO"},{20,"PRINT 2"}};
    EditorModel m(d,&h);assert(m.begin());assert(m.move_end());assert(m.delete_char());
    assert(d.lines.at(10)=="REM HELLOPRINT 2"); // no colon or space inserted
    assert(m.undo());d.lines={{10,""},{20,""}};assert(m.begin());
    assert(m.delete_char()&&d.lines.size()==1&&d.lines.at(10).empty());
    assert(m.undo()&&d.lines.size()==2);
}
void failure_and_dirty() {
    Document d;History h;d.lines={{10,"AB"},{30,"CD"}};
    EditorModel m(d,&h);assert(m.begin());assert(m.insert_char('X'));
    assert(std::string(m.body())=="XAB");const auto stored=d.lines;
    d.reject_pair=true;
    assert(!m.split_line(20)&&d.lines==stored&&std::string(m.body())=="XAB"&&m.line_dirty());
    assert(m.undo_count()==1);
    d.reject_pair=false;assert(m.split_line(20));
    assert(d.lines.at(10)=="X"&&d.lines.at(20)=="AB");
    const auto split=d.lines;
    d.reject_pair=true;assert(!m.undo()&&d.lines==split&&m.undo_count()==2);
    d.reject_pair=false;assert(m.undo()&&d.lines.at(10)=="XAB");
    assert(m.undo()&&d.lines.at(10)=="AB"&&!m.changed());
    assert(m.redo());m.mark_saved();assert(!m.changed());
    assert(m.split_line(20));assert(m.undo()&&!m.changed());
    assert(m.insert_char('Z')&&m.changed()&&m.redo_count()==0);
}
int main(){split_tests();join_tests();failure_and_dirty();std::puts("Editor structure: PASS");}
