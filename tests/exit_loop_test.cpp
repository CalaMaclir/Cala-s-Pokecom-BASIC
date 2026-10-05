#include "language93_test_support.hpp"

int main() {
    language93_case("FOR I=1 TO 100\nIF I=10 THEN\nEXIT FOR\nEND IF\nNEXT I\nPRINT I\nEND", "10\r\n");
    language93_case("DO\nA=A+1\nIF A>=10 THEN\nEXIT DO\nEND IF\nLOOP\nPRINT A", "10\r\n");
    language93_case("FOR Y=1 TO 3\nFOR X=1 TO 10\nIF X=5 THEN EXIT FOR\nNEXT X\nA=A+X\nNEXT Y\nPRINT A", "15\r\n");
    language93_case("DO\nY=Y+1\nDO\nX=X+1\nEXIT DO\nLOOP\nIF Y=3 THEN EXIT DO\nLOOP\nPRINT X", "3\r\n");
    language93_case("FOR I=1 TO 10\nDO\nEXIT FOR\nLOOP\nNEXT I\nPRINT I\nDO\nEXIT DO\nLOOP\nPRINT 2", "1\r\n2\r\n");
    for(const char* loops:{"FOR I=1 TO 100\n", "FOR I=1 TO 100\nFOR J=1 TO 100\n"}) {
        const bool nested=std::strstr(loops,"FOR J");
        language93_case(std::string("FOR K=1 TO 100\nDO\n")+loops+
            "IF I=3 THEN EXIT DO\n"+(nested?"NEXT J\n":"")+
            "NEXT I\nLOOP\nNEXT K\nFOR N=1 TO 3\nPRINT N\nNEXT N\nDO\nEXIT DO\nLOOP", "1\r\n2\r\n3\r\n");
    }
    language93_case("DO WHILE 1\nA=A+1\nIF A=3 THEN EXIT DO\nLOOP\nPRINT A\nDO\nA=A-1\nIF A=1 THEN EXIT DO\nLOOP UNTIL A=0\nPRINT A", "3\r\n1\r\n");
    language93_case("FOR K=1 TO 5\nA=A+F()\nB=B+G()\nNEXT K\nPRINT A;\",\";B\nEND\n"
        "FUNCTION F()\nFOR I=1 TO 10\nDO\nEXIT FOR\nLOOP\nNEXT I\nRETURN I\nEND FUNCTION\n"
        "FUNCTION G()\nDO\nFOR I=1 TO 10\nFOR J=1 TO 10\nEXIT DO\nNEXT J\nNEXT I\nLOOP\n"
        "FOR K=1 TO 3\nA=A+K\nNEXT K\nRETURN A\nEND FUNCTION", "5,30\r\n");
    language93_case("FOR I=5 TO 1\nEXIT FOR\nNEXT I\nFOR J=3 TO 1 STEP -1\nEXIT FOR\nNEXT J\nPRINT I;\",\";J", "5,3\r\n");
    // Recursive calls keep each caller's FOR base intact.
    language93_case("PRINT F(10)\nEND\nFUNCTION F(N)\nIF N=0 THEN RETURN 0\nFOR I=1 TO 2\nA=F(N-1)\nEXIT FOR\nNEXT I\nRETURN A+1\nEND FUNCTION", "10\r\n");
    for(const char* statement:{"EXIT FOR","EXIT DO"})language93_reject(statement,
        std::strstr(statement,"FOR")?"EXIT FOR WITHOUT FOR":"EXIT DO WITHOUT DO");
    language93_reject("EXIT WHILE","EXPECTED FOR OR DO");
    language93_reject("FOR I=1 TO 2\nEXIT FOR\nNEXT I","SYNTAX ERROR",true);
    language93_reject("DO\nEXIT DO\nLOOP","SYNTAX ERROR",true);
    language93_case("EXIT=1:SELECT=2:CASE=3\nPRINT EXIT+SELECT+CASE","6\r\n",true);
    // Existing single-line IF can skip a FOR opener. EXIT must reject the
    // inconsistent stack rather than silently popping an outer FOR frame.
    language93_case("FOR K=1 TO 2\nIF 0 THEN FOR I=1 TO 2\nEXIT FOR\nNEXT I\nNEXT K",
                    "",false,"FOR STACK UNDERFLOW");
    language93_case("FOR K=1 TO 2\nDO\nIF 0 THEN FOR I=1 TO 2\nEXIT DO\nNEXT I\nLOOP\nNEXT K",
                    "",false,"FOR STACK UNDERFLOW");
    language93_reject("FOR I=1 TO 2\nIF 1 THEN\nNEXT I\nEND IF","CROSSED CONTROL BLOCK");
    language93_reject("DO\nFOR I=1 TO 2\nLOOP\nNEXT I","CROSSED CONTROL BLOCK");
    language93_reject("FOR I=1 TO 2\nPRINT F()\nNEXT I\nEND\nFUNCTION F()\nEXIT FOR\nRETURN 1\nEND FUNCTION","EXIT FOR WITHOUT FOR");
    Language93Session session;
    session.require_compile("FOR K=1 TO 5000\nDO\nFOR I=1 TO 2\nEXIT DO\nNEXT I\nLOOP\nNEXT K\nPRINT 9");
    rmb::platform::stage3_break_polls=2;
    const auto interrupted=session.vm->run(*session.code);assert(!interrupted.ok&&interrupted.interrupted);
    rmb::platform::stage3_break_polls=0;session.check_runs("9\r\n");
    session.require_compile("DO\nFOR I=1 TO 2\nA=1/0\nEXIT DO\nNEXT I\nLOOP");
    session.check_runs("","DIVISION BY ZERO");
    session.require_compile("FOR I=1 TO 2\nEXIT FOR\nNEXT I\nPRINT 8");session.check_runs("8\r\n");
    session.require_compile("FOR K=1 TO 2\nPRINT F()\nNEXT K\nEND\nFUNCTION F()\nFOR I=1 TO 2\nEXIT FOR\nNEXT I\nRETURN I\nEND FUNCTION");
    for(std::size_t i=0;i<session.code->code_count;++i)if(session.code->code[i].code==rmb::OpCode::EXIT_LOOP) {
        auto& op=session.code->code[i];const auto saved=op;
        op.b=2;session.run(*session.code,"","FOR STACK UNDERFLOW");op=saved;
        for(int count:{-1,17}) {op.b=count;assert(!rmb::validate_compiled_program(*session.code));op=saved;}
        op.a=0;assert(!rmb::validate_compiled_program(*session.code));
        session.run(*session.code,"","BAD EXIT TARGET");op=saved;
        op.a=9999;assert(!rmb::validate_compiled_program(*session.code));op=saved;
        op.flags=1;assert(!rmb::validate_compiled_program(*session.code));op=saved;
        op.s=17;assert(!rmb::validate_compiled_program(*session.code));op=saved;
        op.s=0;assert(!rmb::validate_compiled_program(*session.code));op=saved;
    }
    session.check_runs("1\r\n1\r\n");
    // Old format is a safe miss, never new opcodes interpreted as old IL.
    rmb::CompiledProgramCache cache;auto restored=std::make_unique<rmb::CompiledProgram>();
    assert(cache.store(session.source.revision(),*session.code));
    const auto allocation=rmb::psram::allocation(rmb::psram::Client::CompiledCache);
    const std::uint16_t old=5;assert(rmb::psram::write(allocation.base_address+4,&old,sizeof(old)));
    assert(!cache.restore(session.source.revision(),*restored,rmb::ProgramSourceMode::Structured));
    assert(!cache.valid());
    std::puts("Stage 3C EXIT: PASS (nesting, call bases, optimizer, cache, BREAK/error rerun)");
}
