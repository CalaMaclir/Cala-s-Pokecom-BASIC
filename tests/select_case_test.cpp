#include "language93_test_support.hpp"

int main() {
    for(const auto& fixture:std::initializer_list<std::pair<const char*,const char*>>{
        {"control-data-select.bas","ONE\r\nTWO OR THREE\r\nTWO OR THREE\r\nFOUR PLUS\r\nFOUR PLUS\r\nSUM=15\r\nRESTORE=1\r\n"},
        {"exit-cleanup.bas","EXIT AT=3\r\n1\r\n2\r\n3\r\n"},
        {"function-read-select-exit.bas","4\r\n5\r\n6\r\n"},
        {"select-string-once.bas","MATCH\r\nCALLS=1\r\n"}}) {
        std::ifstream file(std::string("examples/v093/")+fixture.first);assert(file.good());
        const std::string text(std::istreambuf_iterator<char>(file),{});
        language93_case(text,fixture.second);
    }
    {
        std::ifstream file("examples/v093/data-classic.bas");assert(file.good());
        Language93Session fixture;assert(fixture.source.new_program(rmb::ProgramSourceMode::ClassicNumbered));
        std::string line;while(std::getline(file,line)) {
            char* end=nullptr;const auto n=std::strtol(line.c_str(),&end,10);while(*end==' ')++end;
            assert(n>0&&fixture.source.set_line(n,end));
        }
        assert(fixture.compiler.compile(fixture.source,*fixture.code).ok);fixture.check_runs("3\r\nCPB|A,B\r\n1\r\n");
    }
    for(int value:{-10,0,1,2,3,4,5,10,11,16,20,21}) {
        const std::string selector="SELECT CASE "+std::to_string(value)+"\n";
        language93_case(selector+"CASE -10\nPRINT 1\nCASE 1,2,3\nPRINT 2\nCASE 5 TO 10,16,20\nPRINT 3\nCASE ELSE\nPRINT 4\nEND SELECT\nPRINT 9",
            std::to_string(value==-10?1:value>=1&&value<=3?2:(value>=5&&value<=10)||value==16||value==20?3:4)+"\r\n9\r\n");
    }
    for(int value:{9,10,15,20,21}) {
        language93_case("SELECT CASE "+std::to_string(value)+"\nCASE 10 TO 20\nPRINT 1\nEND SELECT\nPRINT 2",
            std::string(value>=10&&value<=20?"1\r\n":"")+"2\r\n");
    }
    language93_case("SELECT CASE 1\nCASE 2 TO 1\nPRINT 9\nCASE 1\nPRINT 1\nEND SELECT", "1\r\n");
    language93_case("SELECT CASE 16\nCASE 1,5 TO 10,&H10,20\nPRINT 1\nCASE 16\nPRINT 2\nEND SELECT", "1\r\n");
    language93_case("SELECT CASE 2.5\nCASE -1 TO 2.5\nPRINT 1\nCASE 2.5\nPRINT 9\nEND SELECT", "1\r\n");
    language93_case("SELECT CASE 1\nCASE 1\nCASE 1\nPRINT 9\nCASE ELSE\nPRINT 8\nEND SELECT\nPRINT 1", "1\r\n");
    language93_case("SELECT CASE \"Y\"\nCASE \"YES\",\"Y\",\"OK\"\nPRINT 1\nCASE ELSE\nPRINT 2\nEND SELECT", "1\r\n");
    language93_case("A$=\"A\":B$=\"B\"\nSELECT CASE A$+B$\nCASE \"AB\"\nPRINT 1\nCASE ELSE\nPRINT 2\nEND SELECT", "1\r\n");
    language93_case("SELECT CASE \"\"\nCASE \"\"\nPRINT 1\nEND SELECT\nSELECT CASE \"A,B\"\nCASE \"A,B\"\nPRINT 2\nEND SELECT", "1\r\n2\r\n");
    // Literal-only criteria do not rotate the VM scratch ring while the selector is live.
    std::string scratch="SELECT CASE LEFT$(\"TARGET!\",6)\n";
    for(int i=0;i<40;++i)scratch+="CASE \"MISS"+std::to_string(i)+"\"\nPRINT 9\n";
    scratch+="CASE \"TARGET\"\nPRINT 1\nEND SELECT";language93_case(scratch,"1\r\n");
    language93_case("SELECT CASE 1\nCASE 1\nSELECT CASE 10\nCASE 10\nPRINT 1\nCASE ELSE\nPRINT 8\nEND SELECT\nCASE ELSE\nPRINT 9\nEND SELECT", "1\r\n");
    language93_case("IF 1 THEN\nSELECT CASE 1\nCASE 1\nIF 1 THEN\nPRINT 1\nEND IF\nEND SELECT\nEND IF", "1\r\n");
    language93_case("FOR I=1 TO 3\nSELECT CASE I\nCASE 1\nFOR J=1 TO 2\nPRINT J\nNEXT J\nCASE ELSE\nPRINT I\nEND SELECT\nNEXT I", "1\r\n2\r\n2\r\n3\r\n");
    language93_case("DO\nA=A+1\nSELECT CASE A\nCASE 1\nDO\nPRINT 1\nEXIT DO\nLOOP\nCASE 2\nEXIT DO\nEND SELECT\nLOOP", "1\r\n");
    language93_case("PRINT F()\nPRINT COUNT\nEND\nFUNCTION F()\nGLOBAL COUNT\nSELECT CASE G()\nCASE 1\nRETURN 1\nCASE 2\nRETURN 2\nCASE ELSE\nRETURN 3\nEND SELECT\nEND FUNCTION\n"
        "FUNCTION G()\nGLOBAL COUNT\nCOUNT=COUNT+1\nRETURN 2\nEND FUNCTION", "2\r\n1\r\n");
    language93_case("SELECT CASE F$()\nCASE \"ONE\"\nPRINT 9\nCASE \"AB\"\nPRINT 1\nEND SELECT\nPRINT COUNT\nEND\nFUNCTION F$()\nGLOBAL COUNT\nCOUNT=COUNT+1\nRETURN \"A\"+\"B\"\nEND FUNCTION", "1\r\n1\r\n");
    language93_case("PRINT F(10)\nEND\nFUNCTION F(N)\nSELECT CASE N\nCASE 0\nRETURN 0\nCASE ELSE\nRETURN F(N-1)+1\nEND SELECT\nEND FUNCTION", "10\r\n");
    // Three feature interactions, including FUNCTION-local READ + SELECT + EXIT.
    language93_case("DATA 1,2,3\nDO\nREAD A\nSELECT CASE A\nCASE 1\nPRINT \"ONE\"\nCASE 2\nEXIT DO\nCASE ELSE\nPRINT \"OTHER\"\nEND SELECT\nLOOP\nREAD B\nPRINT B", "ONE\r\n3\r\n");
    language93_case("SELECT CASE 1\nCASE 1\nFOR I=1 TO 10\nSELECT CASE I\nCASE 3\nEXIT FOR\nEND SELECT\nNEXT I\nPRINT I\nEND SELECT", "3\r\n");
    language93_case("DATA 1,2,3\nFOR K=1 TO 3\nPRINT F()\nNEXT K\nEND\nFUNCTION F()\nREAD A\nFOR I=1 TO 10\nSELECT CASE A\nCASE 1 TO 3\nEXIT FOR\nCASE ELSE\nRETURN 0\nEND SELECT\nNEXT I\nRETURN A+I\nEND FUNCTION", "2\r\n3\r\n4\r\n");
    for(const auto& bad:std::initializer_list<std::pair<const char*,const char*>>{
        {"CASE 1","CASE WITHOUT SELECT"},{"END SELECT","END SELECT WITHOUT SELECT"},
        {"SELECT CASE 1\nCASE 1","SELECT WITHOUT END SELECT"},
        {"SELECT CASE 1\nEND SELECT","SELECT WITHOUT CASE"},
        {"SELECT CASE 1\nPRINT 1\nCASE 1\nEND SELECT","EXPECTED CASE"},
        {"SELECT CASE 1\nCASE ELSE\nCASE ELSE\nEND SELECT","DUPLICATE CASE ELSE"},
        {"SELECT CASE 1\nCASE ELSE\nCASE 1\nEND SELECT","CASE AFTER CASE ELSE"},
        {"SELECT CASE 1\nCASE \"X\"\nEND SELECT","CASE TYPE MISMATCH"},
        {"SELECT CASE \"X\"\nCASE 1\nEND SELECT","CASE TYPE MISMATCH"},
        {"SELECT CASE \"X\"\nCASE \"A\" TO \"Z\"\nEND SELECT","STRING CASE RANGE NOT ALLOWED"},
        {"SELECT CASE 1\nCASE 1 TO \"X\"\nEND SELECT","CASE TYPE MISMATCH"},
        {"SELECT CASE 1\nCASE IS > 1\nEND SELECT","EXPECTED LITERAL"},
        {"SELECT CASE 1\nCASE A\nEND SELECT","EXPECTED LITERAL"},
        {"SELECT CASE 1\nCASE 1+1\nEND SELECT","CASE REQUIRES LITERALS"},
        {"SELECT CASE 1\nCASE 1,\nEND SELECT","EXPECTED LITERAL"},
        {"SELECT CASE 1\nCASE 1\nIF 1 THEN\nCASE 2\nEND IF\nEND SELECT","CROSSED CONTROL BLOCK"},
        {"IF 1 THEN\nSELECT CASE 1\nCASE 1\nEND IF\nEND SELECT","CROSSED CONTROL BLOCK"},
        {"FOR I=1 TO 2\nSELECT CASE I\nCASE 1\nNEXT I\nEND SELECT","CROSSED CONTROL BLOCK"},
        {"DO\nSELECT CASE 1\nCASE 1\nLOOP\nEND SELECT","CROSSED CONTROL BLOCK"},
        {"SELECT CASE 1\nCASE 1\nWHILE 1\nEND SELECT\nWEND","CROSSED CONTROL BLOCK"},
        {"SELECT CASE 1\nCASE 1\nFUNCTION F()\nRETURN 0\nEND FUNCTION\nEND SELECT","FUNCTION MUST BE TOP LEVEL"},
        {"END\nFUNCTION F()\nSELECT CASE 1\nCASE 1\nRETURN 0\nEND FUNCTION","UNTERMINATED BLOCK IN FUNCTION"}})
        language93_reject(bad.first,bad.second);
    Language93Session s;assert(!s.compile("SELECT CASE 1\nCASE 1\nEND SELECT",true).ok);
    std::string deep;for(int i=0;i<16;++i)deep+="SELECT CASE 1\nCASE 1\n";
    std::string close;for(int i=0;i<16;++i)close+="END SELECT\n";
    language93_case(deep+"PRINT 1\n"+close,"1\r\n");
    language93_reject(deep+"SELECT CASE 1\nCASE 1\nEND SELECT\n"+close,"SELECT NESTING TOO DEEP");
    // CASE bodies have balanced stack state: repeated dispatch cannot leak selectors.
    s.require_compile("FOR I=1 TO 10000\nSELECT CASE I\nCASE 1 TO 9999\nA=A+1\nCASE ELSE\nA=A+1\nEND SELECT\nNEXT I\nPRINT A");
    s.vm->set_profile_mode(rmb::VmProfileMode::Counts);s.check_runs("10000\r\n");
    assert(s.vm->profile_report().max_stack<=3);
    rmb::platform::stage3_break_polls=2;const auto broken=s.vm->run(*s.code);assert(broken.interrupted);
    rmb::platform::stage3_break_polls=0;s.check_runs("10000\r\n");
    s.require_compile("SELECT CASE 1/0\nCASE 1\nPRINT 1\nEND SELECT");s.check_runs("","DIVISION BY ZERO");
    s.require_compile("SELECT CASE 1\nCASE 1\nA=1/0\nEND SELECT");s.check_runs("","DIVISION BY ZERO");
    s.require_compile("SELECT CASE 1\nCASE 1\nPRINT 8\nEND SELECT");s.check_runs("8\r\n");
    // SELECT must not reduce either public variable capacity.
    std::string globals;for(int i=0;i<64;++i)globals+="V"+std::to_string(i)+"=1\n";
    s.require_compile(globals+"SELECT CASE V0\nCASE 1\nPRINT V63\nEND SELECT");
    assert(s.code->symbol_count==64);s.check_runs("1\r\n");
    std::string locals="PRINT F()\nEND\nFUNCTION F()\n";
    for(int i=0;i<128;++i)locals+="V"+std::to_string(i)+"=1\n";
    s.require_compile(locals+"SELECT CASE V0\nCASE 1\nRETURN V127\nCASE ELSE\nRETURN 0\nEND SELECT\nEND FUNCTION");
    assert(s.code->functions[0].local_count==128);s.check_runs("1\r\n");
    // Existing stack opcodes now have safe operand/underflow handling.
    for(auto opcode:{rmb::OpCode::DUP,rmb::OpCode::DROP}) {
        s.code->reset();s.code->code_count=2;s.code->code[0]={opcode,0,0,0,0};s.code->code[1]={rmb::OpCode::HALT,0,0,0,0};
        assert(rmb::validate_compiled_program(*s.code));s.run(*s.code,"","STACK UNDERFLOW");
        s.code->code[0].a=1;assert(!rmb::validate_compiled_program(*s.code));
    }
    std::puts("Stage 3E SELECT: PASS (literal/list/range, strings, single evaluation, nesting, interactions, capacity, cleanup)");
}
