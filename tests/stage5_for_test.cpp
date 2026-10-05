#include "language93_test_support.hpp"
#include "for_runtime.hpp"
#include <limits>

int main() {
#ifdef RMB_STAGE5_INTEGER_FOR_EXPERIMENT
    rmb::ForFrame f;f.end=10;f.step=2;
    rmb::initialize_integer_for(f,0);assert(f.integer_active);
    float n=0;assert(rmb::increment_integer_for(f,n)&&n==2);
    n=7;assert(rmb::increment_integer_for(f,n)&&n==9&&!f.integer_active);
    f.step=.25f;rmb::initialize_integer_for(f,0);assert(!f.integer_active);
    f.step=1;f.end=std::numeric_limits<float>::infinity();
    rmb::initialize_integer_for(f,0);assert(!f.integer_active);
    f.end=2147483648.0f;rmb::initialize_integer_for(f,0);assert(!f.integer_active);
    f.end=0;rmb::initialize_integer_for(f,std::numeric_limits<float>::quiet_NaN());assert(!f.integer_active);
    f.integer_active=true;f.integer_current=std::numeric_limits<std::int32_t>::max();
    f.integer_step=1;n=static_cast<float>(f.integer_current);
    assert(!rmb::increment_integer_for(f,n));
    f.integer_current=std::numeric_limits<std::int32_t>::min();f.integer_step=-1;
    n=static_cast<float>(f.integer_current);assert(!rmb::increment_integer_for(f,n));
#else
    static_assert(sizeof(rmb::ForFrame)==20,"Production FOR frame unchanged");
#endif
    // Optimizer ON/OFF, repeated RUN, cache restore, checked/generic VM.
    for(bool classic:{false,true}) {
        language93_case("S=0\nFOR I=1 TO 10\nS=S+I\nNEXT I\nPRINT S;\",\";I", "55,11\r\n",classic);
        language93_case("S=0\nFOR I=10 TO 1 STEP -1\nS=S+I\nNEXT\nPRINT S;\",\";I", "55,0\r\n",classic);
        language93_case("S=0\nFOR I=0 TO 1 STEP .1\nS=S+1\nNEXT I\nPRINT S", "10\r\n",classic);
        language93_case("S=0\nT=2\nFOR I=0 TO 10 STEP T\nS=S+I\nNEXT I\nPRINT S", "30\r\n",classic);
        language93_case("S=0\nFOR Y=1 TO 5\nFOR X=1 TO 4\nS=S+1\nNEXT X\nNEXT Y\nPRINT S", "20\r\n",classic);
        language93_case("FOR I=1 TO 2 STEP 0\nNEXT I", "",classic,"STEP CANNOT BE ZERO");
        language93_case("FOR I=1 TO 10\nI=I+1\nNEXT I\nPRINT I", "11\r\n",classic);
    }
    language93_case("PRINT F()\nEND\nFUNCTION F()\nS=0\nFOR I=1 TO 10\nS=S+I\nNEXT I\nRETURN S\nEND FUNCTION", "55\r\n");
    language93_case("G=0\nPRINT F()\nPRINT G\nEND\nFUNCTION F()\nGLOBAL G\nFOR G=1 TO 10\nS=S+G\nNEXT G\nRETURN S\nEND FUNCTION", "55\r\n11\r\n");
    // Recursive function loop state, local storage and caller restoration.
    language93_case("PRINT F(5)\nEND\nFUNCTION F(N)\nIF N=0 THEN RETURN 0\nS=0\nFOR I=1 TO 2\nS=S+F(N-1)+I\nNEXT I\nRETURN S\nEND FUNCTION", "93\r\n");
    Language93Session s;
    s.require_compile("FOR I=1 TO 100000\nNEXT I\nPRINT I");
    rmb::platform::stage3_break_polls=2;
    const auto interrupted=s.vm->run(*s.code);assert(!interrupted.ok&&interrupted.interrupted);
    rmb::platform::stage3_break_polls=0;s.check_runs("100001\r\n");
    s.require_compile("FOR I=1 TO 2\nPRINT F()\nNEXT I\nEND\nFUNCTION F()\nFOR J=1 TO 2\nA=1/0\nNEXT J\nRETURN 1\nEND FUNCTION");
    output.clear();const auto error=s.vm->run(*s.code);
    assert(!error.ok&&error.source_row==7&&error.call_source_row==2&&std::strcmp(error.function_name,"F")==0);
    s.check_runs("","DIVISION BY ZERO");
    s.require_compile("FOR I=1 TO 2\nEXIT FOR\nNEXT I\nPRINT I");s.check_runs("1\r\n");
    s.require_compile("DO\nFOR I=1 TO 2\nFOR J=1 TO 2\nEXIT DO\nNEXT J\nNEXT I\nLOOP\nFOR K=1 TO 2\nNEXT K\nPRINT K");s.check_runs("3\r\n");
    std::puts("Stage 5 FOR semantics / optimizer / call state / BREAK / errors / cache: PASS");
}
