#define CPB_IMAGE_IO_TEST
#include "language93_test_support.hpp"
#include <tuple>
#include "soak_support.hpp"
namespace rmb { int LineEditor::last_special_key() { return 0; } }

int main() {
    using namespace rmb;
    unsigned saves=0, loads=0;
    std::string saved, loaded;
    std::array<int,4> region{};
    int x=0,y=0;
    storage::image_save=[&](const char* name,int x1,int y1,int x2,int y2) {
        ++saves; saved=name; region={x1,y1,x2,y2};return true;
    };
    storage::image_load=[&](const char* name,int px,int py) {
        ++loads;loaded=name;x=px;y=py;return true;
    };
    for(bool classic:{false,true}) {
        language93_case("A$=\"TEST\"\nLOADIMAGE A$+\".BMP\",20,10\nSAVEIMAGE \"COPY\"\n", "",classic);
        assert(loaded=="TEST.BMP"&&x==20&&y==10&&saved=="COPY");
        assert((region==std::array<int,4>{0,0,319,319}));
        language93_case("LOADIMAGE \"TEST.BMP\"\nSAVE IMAGE \"COPY\",1,2,3,4\n", "",classic);
        assert(x==0&&y==0&&(region==std::array<int,4>{1,2,3,4}));
        language93_case("LOADIMAGE \"TEST\",-100,400\n", "",classic);assert(x==-100&&y==400);
        // Both spellings produce identical IL slots, pools and GSAVE statement kind.
        Language93Session a,b;
        a.require_compile("SAVEIMAGE \"SAME\",1,2,3,4",classic);
        b.require_compile("SAVE IMAGE \"SAME\",1,2,3,4",classic);
        assert(a.code->code_count==b.code->code_count);
        for(std::size_t i=0;i<a.code->code_count;++i) {
            const auto& p=a.code->code[i];const auto& q=b.code->code[i];
            assert(p.code==q.code&&p.a==q.a&&p.b==q.b&&p.s==q.s);
        }
        language93_case("LOADIMAGE \"T\",0", "",classic,"LOADIMAGE: ARGUMENT COUNT");
        language93_case("LOADIMAGE 42,0,0", "",classic,"LOADIMAGE: FILENAME REQUIRED");
        language93_case("LOADIMAGE \"T\",\"X\",0", "",classic,"LOADIMAGE: BAD COORDINATE");
        language93_case("LOADIMAGE \"T\",1E30,0", "",classic,"LOADIMAGE: BAD COORDINATE");
        language93_case("SAVEIMAGE \"T\",1E30,0,10,10", "",classic,"SAVEIMAGE: BAD REGION");
        language93_case("SAVEIMAGE \"T\",0", "",classic,"SAVEIMAGE: ARGUMENT COUNT");
        language93_case("SAVE IMAGE 1", "",classic,"SAVEIMAGE: FILENAME REQUIRED");
        language93_reject("SAVE \"program\"","",classic);
        language93_reject("LOAD \"program\"","",classic);
    }
    for (const char* filename : {"examples/v094/image-roundtrip-classic.bas", "examples/v094/image-roundtrip-structured.bas", "tests/soak/image-classic.bas", "tests/soak/image-structured.bas"}) {
        Language93Session session; session.source.set_root("./");
        if (!session.source.load(filename)) { std::cerr << filename << ": " << session.source.error() << "\n"; assert(false); }
        assert(session.compiler.compile(session.source,*session.code).ok);
        assert(validate_compiled_program(*session.code));
    }
    // REPL direct dispatch separates names at word boundaries and retains program SAVE.
    char directory[]="/tmp/cpb-image-repl-XXXXXX";assert(mkdtemp(directory));
    Repl repl;assert(repl.program_.initialize(ProgramStorageMode::InternalRam));
    const std::string program_root=std::string(directory)+"/";
    repl.program_.set_root(program_root.c_str());
    auto command=[&](const char* text){char line[256];std::strcpy(line,text);output.clear();repl.process_line(line);};
    command("10 PRINT 42");
    const auto n=saves;
    command("SAVE TEST");assert(output.find("SAVED ")!=std::string::npos&&saves==n);
    command("SAVE");assert(output.find("SAVED ")!=std::string::npos&&saves==n);
    command("NEW");command("LOAD TEST");command("LIST");assert(output.find("10 PRINT 42")!=std::string::npos);
    command("RUN");assert(output.find("42")!=std::string::npos);
    command("SAVEIMAGE \"IMAGE\"");assert(saves==n+1);
    command("SAVE IMAGE \"IMAGE\"");assert(saves==n+2);
    command("LOADIMAGE \"IMAGE\",20,10");assert(loaded=="IMAGE"&&x==20&&y==10);
    command("HELP");assert(output.find("LOADIMAGE")!=std::string::npos&&output.find("compatibility")!=std::string::npos);
    repl.compiled_cache_.invalidate(); // Release the singleton cache client before isolated VM sessions.
    const auto resources = cpb_soak::resources();
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) for (bool classic : {false, true}) {
        const auto before = saves;
        language93_case("SAVEIMAGE \"SAME\",1,2,3,4\nSAVE IMAGE \"SAME\",1,2,3,4\nLOADIMAGE \"SAME\",2,3\n", "", classic);
        assert(saves == before + 24 && x == 2 && y == 3);
        assert((region == std::array<int,4>{1,2,3,4}));
    }
    cpb_soak::record("image-statements", cpb_soak::iterations(), resources);
    std::filesystem::remove_all(directory);
    std::puts("Image statements, identical compatibility IL, Classic/Structured/cache/direct/REPL: PASS");
}
