#include "program_store.hpp"
#include "file_management.hpp"
#include "psram.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
namespace {bool available=true,locked=false;int write_failure=0,rename_failure=0;}
extern "C" {
std::size_t __real_fwrite(const void*,std::size_t,std::size_t,FILE*);
int __real_rename(const char*,const char*);
std::size_t __wrap_fwrite(const void* p,std::size_t s,std::size_t n,FILE* f) {
    if(write_failure>0&&--write_failure==0){errno=EIO;return 0;}return __real_fwrite(p,s,n,f);
}
int __wrap_rename(const char* a,const char* b){if(rename_failure>0&&--rename_failure==0){errno=EIO;return -1;}return __real_rename(a,b);}
}
namespace rmb::storage {
bool init(){return ::available&&!locked;}bool available(){return ::available;}
bool remount(){return init();}
bool card_present(){return ::available;}bool firmware_owns_card(){return true;}
bool try_lock(){if(locked)return false;locked=true;return true;}void unlock(){assert(locked);locked=false;}
const char* last_error(){return "INJECTED STORAGE BUSY";}
}
namespace rmb::psram {void test_set_available(bool);void test_fail_write_after(int);}
using namespace rmb;
std::string listing(ProgramStore& p){std::string s;for(std::size_t i=0;i<p.size();++i){std::int32_t n;const char* b;std::size_t l;assert(p.read_line_text(i,n,b,l));s+=std::to_string(n)+" "+std::string(b,l)+"\n";}return s;}
#include "full_screen_editor.hpp"
#include "psram_editor_history.hpp"
std::string plain(ProgramStore& p) {
    std::string s;for(std::size_t i=0;i<p.size();++i) {
        std::int32_t id;const char* b;std::size_t l;
        assert(p.read_line_text(i,id,b,l));assert(id==static_cast<std::int32_t>(i+1));
        s.append(b,l);s+='\n';
    }return s;
}
int main(){
    char dir[]="/tmp/cpb-structured-source-XXXXXX";assert(mkdtemp(dir));
    const std::string root=std::string(dir)+"/";
    for(int backend=0;backend<3;++backend) {
        psram::test_set_available(backend!=0);
        ProgramStore p;p.set_root(root.c_str());
        assert(p.initialize(backend==2?ProgramStorageMode::SdCard:ProgramStorageMode::InternalRam));
        assert(p.new_program(ProgramSourceMode::Structured));
        const char* rows[]={"  PRINT 1",""};
        assert(p.replace_source_rows(0,0,rows,2));
        const std::string expected="  PRINT 1\n\n";
        assert(plain(p)==expected);assert(p.save("STRUCT"));
        {std::ifstream f(root+"STRUCT.BAS");std::string s((std::istreambuf_iterator<char>(f)),{});assert(s==expected);}
        assert(p.load("STRUCT"));assert(p.source_mode()==ProgramSourceMode::Structured);
        assert(plain(p)==expected);
        {std::ofstream f(root+"MIX.BAS");f<<"10 PRINT 1\nPRINT 2\n";}
        const auto revision=p.revision();
        assert(!p.load("MIX"));assert(std::string(p.error())=="MIXED SOURCE MODE");
        assert(p.revision()==revision&&plain(p)==expected);
        {std::ofstream f(root+"BLANK.BAS");f<<"\n   \n";}
        assert(p.load("BLANK"));assert(p.source_mode()==ProgramSourceMode::Structured);
        assert(plain(p)=="\n   \n");
        assert(p.load("STRUCT"));
        std::filesystem::create_directories(root+"PARENT");
        assert(p.save("PARENT/SOURCE"));
        assert(file_management::rename_directory(root.c_str(),"PARENT","MOVED",p.filename())==file_management::Result::Success);
        assert(p.note_source_renamed("PARENT","MOVED"));
        assert(p.source_mode()==ProgramSourceMode::Structured&&plain(p)==expected);
        assert(file_management::rename_file(root.c_str(),"MOVED/SOURCE.BAS","MOVED/RENAMED.BAS",p.filename())==file_management::Result::Success);
        assert(p.note_source_renamed("MOVED/SOURCE.BAS","MOVED/RENAMED.BAS"));
        assert(p.source_mode()==ProgramSourceMode::Structured&&plain(p)==expected);
        assert(p.save("STRUCT"));std::filesystem::remove_all(root+"MOVED");
        const std::string too_long(p.line_length_capacity()+1,'X');
        const char* invalid[]={too_long.c_str()};assert(!p.replace_source_rows(0,1,invalid,1));
        assert(plain(p)==expected);
        if(backend==1) {
            for(int fail=1;fail<=2;++fail) {
                psram::test_fail_write_after(fail);const char* replacement[]={"CHANGED"};
                const auto rev=p.revision();assert(!p.replace_source_rows(0,1,replacement,1));
                assert(plain(p)==expected&&p.revision()==rev);
            }
        }
        if(backend==2) {
            assert(p.suspend_for_usb());assert(p.resume_after_usb());
            assert(p.source_mode()==ProgramSourceMode::Structured&&plain(p)==expected);
            ProgramStore recovered;recovered.set_root(root.c_str());
            assert(recovered.initialize(ProgramStorageMode::SdCard));
            assert(recovered.source_mode()==ProgramSourceMode::Structured&&plain(recovered)==expected);
            assert(p.switch_mode(ProgramStorageMode::InternalRam));
            assert(p.source_mode()==ProgramSourceMode::Structured&&plain(p)==expected);
            assert(p.switch_mode(ProgramStorageMode::SdCard));
            assert(p.source_mode()==ProgramSourceMode::Structured&&plain(p)==expected);
        }
        // Real ProgramStore adapter: split/insert/join and PSRAM Undo/Redo.
        ProgramStoreEditorDocument d(p);PsramEditorHistory h;
        if(backend!=0)assert(h.begin());
        auto m=std::make_unique<EditorModel>(d,backend!=0?&h:nullptr);
        assert(m->begin());assert(m->move_right());assert(m->move_right());
        assert(m->split_line(0));assert(plain(p)=="  \nPRINT 1\n\n");
        if(backend!=0){assert(m->undo()&&plain(p)==expected);assert(m->redo());}
        assert(m->backspace()&&plain(p)==expected);
        assert(m->move_home());assert(m->split_line(0));assert(plain(p)=="\n  PRINT 1\n\n");
        if(backend!=0){assert(m->undo()&&plain(p)==expected);assert(m->redo());}
        assert(m->delete_line());assert(plain(p)==expected);
        // EOL Delete joins exactly, including indentation, without inserted text.
        assert(m->move_home());assert(m->move_end());
        assert(m->delete_char());assert(plain(p)=="  PRINT 1\n");
        if(backend!=0){assert(m->undo()&&plain(p)==expected);assert(m->redo());}
        m.reset();h.end();
        // Pending typed text must survive line-start Enter and structural Undo.
        assert(p.new_program(ProgramSourceMode::Structured));
        const char* abc[]={"ABC"};assert(p.replace_source_rows(0,0,abc,1));
        if(backend!=0)assert(h.begin());
        m=std::make_unique<EditorModel>(d,backend!=0?&h:nullptr);assert(m->begin());
        assert(m->insert_char('X'));assert(m->move_home());
        assert(m->split_line(0)&&plain(p)=="\nXABC\n");
        if(backend!=0) {
            assert(m->undo()&&plain(p)=="XABC\n");assert(m->redo()&&plain(p)=="\nXABC\n");
            assert(m->undo()&&plain(p)=="XABC\n");assert(m->undo()&&plain(p)=="ABC\n");
            assert(m->redo()&&plain(p)=="XABC\n");
        } else assert(m->delete_line());
        assert(m->move_end());assert(m->insert_char('Y'));
        assert(m->move_left());assert(m->move_left());
        assert(m->split_line(0)&&plain(p)=="XAB\nCY\n");
        if(backend!=0)assert(m->undo()&&plain(p)=="XABCY\n");
        m.reset();h.end();
        // Tab is spaces, one history edit, and round-trips on every backend.
        assert(p.new_program(ProgramSourceMode::Structured));
        const char* tab_source[]={"  PRINT 1"};
        assert(p.replace_source_rows(0,0,tab_source,1));
        if(backend!=0)assert(h.begin());
        m=std::make_unique<EditorModel>(d,backend!=0?&h:nullptr);
        m->configure_viewport(33,52);assert(m->begin());
        assert(m->move_right()&&m->move_right());
        assert(m->insert_tab()&&m->cursor()==4&&m->commit());
        assert(plain(p)=="    PRINT 1\n");
        if(backend!=0) {
            assert(m->undo_count()==1);
            assert(m->undo()&&plain(p)=="  PRINT 1\n");
            assert(m->redo()&&plain(p)=="    PRINT 1\n");
        }
        assert(p.save("TAB"));assert(p.load("TAB"));
        assert(plain(p)=="    PRINT 1\n");
        assert(plain(p).find('\t')==std::string::npos);
        m.reset();h.end();
        // Exact backend boundaries, overflow non-destructive, complete source round-trip.
        assert(p.new_program(ProgramSourceMode::Structured));
        const std::string boundary="REM "+std::string(p.line_length_capacity()-4,'X');
        const char* full[]={boundary.c_str()};assert(p.replace_source_rows(0,0,full,1));
        assert(p.save("BOUNDARY"));assert(p.load("BOUNDARY"));assert(plain(p)==boundary+"\n");
        const auto capacity=p.line_capacity();
        {std::ofstream f(root+"ROWS.BAS");for(std::size_t row=0;row<capacity;++row)f<<"REM row\n";}
        assert(p.load("ROWS")&&p.size()==capacity);
        const auto before=plain(p);const auto full_revision=p.revision();
        const char* extra[]={""};assert(!p.replace_source_rows(capacity,0,extra,1));
        assert(p.revision()==full_revision&&plain(p)==before);
        assert(p.save("ROWS2"));assert(p.load("ROWS2"));assert(plain(p)==before);
        assert(p.new_program(ProgramSourceMode::ClassicNumbered));
        {std::ofstream f(root+"CLASSIC.BAS");f<<"10 PRINT 1\n20 END\n";}
        assert(p.load("CLASSIC"));assert(p.source_mode()==ProgramSourceMode::ClassicNumbered);
    }
    // Corrupt session mode cannot delete a valid unnumbered editing snapshot.
    {
        const std::string damaged=root+"damaged/";std::filesystem::create_directory(damaged);
        std::ofstream(damaged+"RMBP0001.BAS")<<"  PRINT 7\n\n";
        std::ofstream(damaged+"RMBASIC.SES")<<"version=2\nwork=RMBP0001.BAS\nfile=STRUCT.BAS\ndirty=1\nsource_mode=9\n";
        ProgramStore recovered;recovered.set_root(damaged.c_str());
        assert(recovered.initialize(ProgramStorageMode::SdCard));
        assert(!recovered.session_recovered()&&recovered.size()==0);
        assert(recovered.load("RECOVER0000"));
        assert(recovered.source_mode()==ProgramSourceMode::Structured&&plain(recovered)=="  PRINT 7\n\n");
    }
    std::filesystem::remove_all(root);std::puts("Structured source/editor: PASS");
}
