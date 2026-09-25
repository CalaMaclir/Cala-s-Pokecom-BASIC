#include "program_store.hpp"
#include "session_notice.hpp"
#include "program_file_guard.hpp"
#include "storage_recovery.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
bool card=true,mounted=true,locked=false;
}
namespace rmb::storage {
bool init(){return card&&mounted&&!locked;}
bool available(){return mounted;}
bool card_present(){return card;}
bool remount(){if(locked)return false;mounted=card;return mounted;}
bool try_lock(){if(locked||!card||!mounted)return false;locked=true;return true;}
void unlock(){assert(locked);locked=false;}
bool firmware_owns_card(){return true;}
const char* last_error(){return locked?"STORAGE BUSY":"SD NOT AVAILABLE";}
}

using namespace rmb;
namespace fs=std::filesystem;

static std::string make_root() {
    char temp[]="/tmp/rmb-session-XXXXXX";
    assert(mkdtemp(temp));
    return std::string(temp)+"/";
}
static void write_text(const std::string& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary);file<<text;assert(file.good());
}
static std::string read_text(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
static std::string listing(const ProgramStore& program) {
    std::string out;
    for(std::size_t i=0;i<program.size();++i) {
        std::int32_t number=0;const char* text=nullptr;std::size_t length=0;
        assert(program.read_line_text(i,number,text,length));
        out+=std::to_string(number)+" ";
        out.append(text,length);out+="\n";
    }
    return out;
}
static std::string session_work(const std::string& root) {
    const std::string text=read_text(root+"RMBASIC.SES");
    const auto begin=text.find("work=");
    assert(begin!=std::string::npos);
    const auto end=text.find('\n',begin);
    return text.substr(begin+5,end-(begin+5));
}
static std::size_t work_count(const std::string& root) {
    std::size_t count=0;
    for(const auto& entry:fs::directory_iterator(root)) {
        const auto name=entry.path().filename().string();
        if(name.size()==12&&name.rfind("RMBP",0)==0&&
           name.substr(8)==".BAS")++count;
    }
    return count;
}

int main() {
    assert(!show_restored_editing_session(false, false)); // fresh UNTITLED
    assert(!show_restored_editing_session(true, false));  // clean saved file
    assert(show_restored_editing_session(true, true));   // dirty named or UNTITLED
    // A fresh workspace has no restored notification.
    {
        const auto root=make_root();
        ProgramStore p; p.set_root(root.c_str());
        assert(p.initialize(ProgramStorageMode::SdCard));
        assert(!show_restored_editing_session(p.session_recovered(),p.is_dirty()));
        fs::remove_all(root);
    }
    // A saved named session may be restored internally but is not dirty.
    {
        const auto root=make_root();
        {
            ProgramStore p; p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.set_line(10,"PRINT 1"));
            assert(p.save("CLEAN"));
        }
        {
            ProgramStore p; p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(!std::strcmp(p.filename(),"CLEAN.BAS"));
            assert(!show_restored_editing_session(p.session_recovered(),p.is_dirty()));
        }
        fs::remove_all(root);
    }
    // Named dirty workspace, filename and dirty bit survive power loss.
    {
        const auto root=make_root();
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.set_line(10,"PRINT 1"));
            assert(p.save("MEGADEMO"));
            assert(p.set_line(20,"PRINT 2"));
            assert(p.is_dirty());
        }
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.session_recovered());
            assert(show_restored_editing_session(p.session_recovered(),p.is_dirty()));
            assert(!std::strcmp(p.filename(),"MEGADEMO.BAS"));
            assert(p.is_dirty());
            assert(listing(p)=="10 PRINT 1\n20 PRINT 2\n");
            assert(work_count(root)==1);
        }
        fs::remove_all(root);
    }

    // UNTITLED has an empty session filename and is restored as dirty.
    {
        const auto root=make_root();
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.set_line(100,"REM UNSAVED"));
            assert(p.filename()[0]==0);
        }
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::Auto));
            assert(p.session_recovered()&&p.filename()[0]==0&&p.is_dirty());
            assert(show_restored_editing_session(p.session_recovered(),p.is_dirty()));
            assert(listing(p)=="100 REM UNSAVED\n");
        }
        fs::remove_all(root);
    }

    // A dirty session preserves an externally loaded 2047-character SD line.
    {
        const auto root=make_root();
        const std::string tail="END_SENTINEL";
        const std::string body=
            "REM "+std::string(2047-4-tail.size(),'L')+tail;
        const std::string expected="10 "+body+"\n20 PRINT 2\n";
        write_text(root+"LONG.BAS","10 "+body+"\n");
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.load("LONG"));
            assert(p.set_line(20,"PRINT 2")&&p.is_dirty());
            assert(listing(p)==expected);
        }
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::Auto));
            assert(p.session_recovered()&&p.is_dirty());
            assert(listing(p)==expected);
            assert(p.save("LONGRECOVER"));
            assert(read_text(root+"LONGRECOVER.BAS")==expected);
            assert(p.load("LONGRECOVER")&&listing(p)==expected);
        }
        fs::remove_all(root);
    }

    // A SafeFileWriter backup is a valid last committed recovery point.
    {
        const auto root=make_root();
        std::string expected;
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.set_line(10,"PRINT 7"));expected=listing(p);
        }
        fs::rename(root+"RMBASIC.SES",root+"RMBSES.BAK");
        {
            ProgramStore p;p.set_root(root.c_str());
            assert(p.initialize(ProgramStorageMode::SdCard));
            assert(p.session_recovered()&&listing(p)==expected);
            assert(fs::exists(root+"RMBASIC.SES"));
            assert(!fs::exists(root+"RMBSES.BAK"));
        }
        fs::remove_all(root);
    }

    // Invalid metadata, missing work and corrupted work all fall back safely.
    for(int failure=0;failure<3;++failure) {
        const auto root=make_root();
        if(failure==0) {
            write_text(root+"RMBASIC.SES","not-a-session\n");
            write_text(root+"RMBP0001.BAS","10 PRINT 1\n");
        } else if(failure==1) {
            write_text(root+"RMBASIC.SES",
                "version=1\nwork=RMBP0001.BAS\nfile=LOST.BAS\ndirty=1\n");
        } else {
            write_text(root+"RMBP0001.BAS","THIS IS NOT BASIC\n");
            write_text(root+"RMBASIC.SES",
                "version=1\nwork=RMBP0001.BAS\nfile=BROKEN.BAS\ndirty=1\n");
        }
        ProgramStore p;p.set_root(root.c_str());
        assert(p.initialize(ProgramStorageMode::SdCard));
        assert(!p.session_recovered()&&p.size()==0&&!p.is_dirty());
        assert(!show_restored_editing_session(p.session_recovered(),p.is_dirty()));
        assert(p.filename()[0]==0&&work_count(root)==1);
        fs::remove_all(root);
    }

    // Cleanup protects both the live work and the session reference, removes
    // exact orphan work names, and never removes normal BASIC programs.
    {
        const auto root=make_root();
        ProgramStore p;p.set_root(root.c_str());
        assert(p.initialize(ProgramStorageMode::SdCard));
        assert(p.set_line(10,"PRINT 1"));
        const auto protected_work=session_work(root);
        write_text(root+"RMBP9998.BAS","10 ORPHAN\n");
        write_text(root+"USER.BAS","10 PRINT 9\n");
        assert(p.cleanup_orphans());
        assert(fs::exists(root+protected_work));
        assert(!fs::exists(root+"RMBP9998.BAS"));
        assert(fs::exists(root+"USER.BAS"));

        assert(p.switch_mode(ProgramStorageMode::InternalRam));
        write_text(root+"RMBP9997.BAS","20 ORPHAN\n");
        assert(p.cleanup_orphans());
        assert(fs::exists(root+protected_work)); // session-only protection
        assert(!fs::exists(root+"RMBP9997.BAS"));
        fs::remove_all(root);
    }

    // FILES/DIR visibility and remount safety policy.
    assert(!program_files::visible_in_directory("RMBP0001.BAS"));
    assert(!program_files::visible_in_directory("RMBEDIT.TMP"));
    assert(!program_files::visible_in_directory("RMBSES.BAK"));
    for(const char* name:{"MEGADEMO.BAS","AUTORUN.WAV","SCREEN0001.BMP",
                          "RMBASIC.CFG","RMBASIC.BAK","RMBASIC.SES"})
        assert(program_files::visible_in_directory(name));

    using storage_recovery::ProgramAction;
    assert(!storage_recovery::reload_after_remount(false));
    assert(storage_recovery::reload_after_remount(true));
    assert(storage_recovery::program_action(
        false,ProgramStorageMode::Auto,ProgramBackend::Ram,true)==ProgramAction::None);
    assert(storage_recovery::program_action(
        true,ProgramStorageMode::Auto,ProgramBackend::Ram,true)==ProgramAction::AskDirtyRam);
    assert(storage_recovery::program_action(
        true,ProgramStorageMode::SdCard,ProgramBackend::Ram,false)==ProgramAction::RecoverSession);
    assert(storage_recovery::program_action(
        true,ProgramStorageMode::InternalRam,ProgramBackend::Sd,false)==ProgramAction::UseInternalRam);

    std::puts("Session recovery, orphan cleanup, visibility and remount policy passed");
}
