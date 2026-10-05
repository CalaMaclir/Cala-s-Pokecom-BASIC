#include "benchmark_platform.hpp"
#include "compiled_cache.hpp"
#include "local_numeric_fusion.hpp"
#include <memory>
#include <cmath>
#include <sstream>
namespace rmb { extern bool optimizer_test_disabled,vm_test_generic,vm_test_unverified; }
namespace rmb::platform { int stage3_break_polls=0; }
extern "C" void* __real_calloc(std::size_t,std::size_t);
extern "C" void __real_free(void*);
static bool track=false;static int fail_at=0,allocations=0,live=0;
static void* frames[32]={};
extern "C" void* __wrap_calloc(std::size_t n,std::size_t bytes) {
    if(track&&++allocations==fail_at)return nullptr;
    void* p=__real_calloc(n,bytes);
    if(track&&p){assert(live<32);frames[live++]=p;}
    return p;
}
extern "C" void __wrap_free(void* p) {
    for(int i=0;i<live;++i)if(frames[i]==p){frames[i]=frames[--live];break;}
    __real_free(p);
}
void load(rmb::ProgramStore& p,const std::string& text,bool classic=false) {
    assert(p.initialize(rmb::ProgramStorageMode::InternalRam));
    assert(p.new_program(classic?rmb::ProgramSourceMode::ClassicNumbered:rmb::ProgramSourceMode::Structured));
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        if(classic){char* end=nullptr;const auto number=std::strtol(line.c_str(),&end,10);while(*end==' ')++end;assert(p.set_line(number,end));}
        else {const char* row[]={line.c_str()};assert(p.replace_source_rows(p.size(),0,row,1));}
    }
}
struct Result { rmb::VmResult diagnostic;std::string text;std::uint32_t hash,pixels; };
Result execute(rmb::VM& vm,const rmb::CompiledProgram& il) {
    current_color=0;output.clear();graphics_hash=2166136261u;pixels=0;
    auto r=vm.run(il);return {r,output,graphics_hash,::pixels};
}
void equivalent(const std::string& text,bool classic=false,bool require_fusion=false,bool require_gray=false) {
    rmb::ProgramStore p;load(p,text,classic);rmb::BasicCompiler compiler;
    auto plain=std::make_unique<rmb::CompiledProgram>(),fast=std::make_unique<rmb::CompiledProgram>();
    rmb::optimizer_test_disabled=true;assert(compiler.compile(p,*plain).ok);
    rmb::optimizer_test_disabled=false;assert(compiler.compile(p,*fast).ok);
    assert(rmb::validate_compiled_program(*plain)&&rmb::validate_compiled_program(*fast));
    assert(plain->code_count==fast->code_count);
    assert(plain->source_row_count==fast->source_row_count);
    assert(!plain->source_row_count||!std::memcmp(plain->source_rows,fast->source_rows,plain->source_row_count*sizeof(rmb::SourceRowPc)));
    bool fused=false;for(std::size_t i=0;i<fast->code_count;++i)fused|=fast->code[i].code==rmb::OpCode::LOCAL_NUM_FUSED;
    assert(!require_fusion||fused);
    bool gray=false;for(std::size_t i=0;i<fast->code_count;++i)gray|=fast->code[i].code==rmb::OpCode::GRAY_PSET_MUL_INT||fast->code[i].code==rmb::OpCode::LOCAL_GRAY_PSET_MUL_INT;
    assert(!require_gray||gray);
    auto vm=std::make_unique<rmb::VM>();
    vm->set_profile_mode(rmb::VmProfileMode::Counts);
    auto a=execute(*vm,*plain);const auto before=vm->profile_report();
    auto b=execute(*vm,*fast);const auto after=vm->profile_report();
    assert(before.logical_ops==after.logical_ops);
    if(require_fusion)assert(after.dispatches<before.dispatches);
    assert(a.text==b.text&&a.hash==b.hash&&a.pixels==b.pixels);
    assert(a.diagnostic.ok==b.diagnostic.ok);
    assert(a.diagnostic.source_row==b.diagnostic.source_row);
    assert(a.diagnostic.call_source_row==b.diagnostic.call_source_row);
    assert(a.diagnostic.call_depth==b.diagnostic.call_depth);
    assert(!std::strcmp(a.diagnostic.message,b.diagnostic.message));
    assert(!std::strcmp(a.diagnostic.function_name,b.diagnostic.function_name));
    if(!a.diagnostic.ok)assert(a.diagnostic.pc==b.diagnostic.pc);
    rmb::CompiledProgramCache cache;auto restored=std::make_unique<rmb::CompiledProgram>();
    assert(cache.store(p.revision(),*fast));
    assert(cache.restore(p.revision(),*restored,p.source_mode()));
    assert(rmb::validate_compiled_program(*restored));
    auto hit=execute(*vm,*restored);
    assert(hit.text==b.text&&hit.hash==b.hash&&hit.pixels==b.pixels);
    assert(hit.diagnostic.pc==b.diagnostic.pc&&hit.diagnostic.source_row==b.diagnostic.source_row);
    rmb::vm_test_generic=true;auto generic=execute(*vm,*fast);rmb::vm_test_generic=false;
    assert(generic.text==b.text&&generic.hash==b.hash&&generic.pixels==b.pixels);
    assert(generic.diagnostic.pc==b.diagnostic.pc&&generic.diagnostic.source_row==b.diagnostic.source_row);
    assert(!std::strcmp(generic.diagnostic.message,b.diagnostic.message));
    rmb::vm_test_unverified=true;auto checked=execute(*vm,*fast);rmb::vm_test_unverified=false;
    assert(checked.text==b.text&&checked.hash==b.hash&&checked.pixels==b.pixels);
    assert(checked.diagnostic.pc==b.diagnostic.pc&&checked.diagnostic.source_row==b.diagnostic.source_row);
    assert(checked.diagnostic.call_source_row==b.diagnostic.call_source_row&&checked.diagnostic.call_depth==b.diagnostic.call_depth);
    assert(!std::strcmp(checked.diagnostic.message,b.diagnostic.message));
    cache.invalidate();
}
void gray_overflow_equivalent() {
    rmb::ProgramStore p;load(p,"PRINT F()\nEND\nFUNCTION F()\nA=3:B=4:X=0:Y=0\nG=INT(A*B)\nCOLOR G,G,G\nPSET X,Y\nRETURN G\nEND FUNCTION");
    rmb::BasicCompiler compiler;
    auto plain=std::make_unique<rmb::CompiledProgram>(),fast=std::make_unique<rmb::CompiledProgram>();
    rmb::optimizer_test_disabled=true;assert(compiler.compile(p,*plain).ok);
    rmb::optimizer_test_disabled=false;assert(compiler.compile(p,*fast).ok);
    for(auto* il:{plain.get(),fast.get()}) {
        const int prefix=190;
        assert(il->code_count+prefix<rmb::kMaxOps&&il->number_count>0);
        std::memmove(il->code+prefix,il->code,il->code_count*sizeof(rmb::Op));
        il->code_count+=prefix;
        for(int i=0;i<prefix;++i)il->code[i]={rmb::OpCode::PUSH_NUM,0,0,0,0};
        for(std::size_t i=prefix;i<il->code_count;++i)if(il->code[i].code==rmb::OpCode::JMP)il->code[i].a+=prefix;
        for(std::size_t i=0;i<il->function_count;++i){il->functions[i].entry_pc+=prefix;il->functions[i].end_pc+=prefix;}
        for(std::size_t i=0;i<il->source_row_count;++i)il->source_rows[i].pc+=prefix;
        assert(rmb::validate_compiled_program(*il));
    }
    auto vm=std::make_unique<rmb::VM>();const auto a=execute(*vm,*plain),b=execute(*vm,*fast);
    assert(!a.diagnostic.ok&&std::strstr(a.diagnostic.message,"STACK OVERFLOW"));
    assert(a.text==b.text&&a.hash==b.hash&&a.pixels==b.pixels);
    assert(a.diagnostic.pc==b.diagnostic.pc&&a.diagnostic.source_row==b.diagnostic.source_row);
    assert(a.diagnostic.call_source_row==b.diagnostic.call_source_row&&a.diagnostic.call_depth==b.diagnostic.call_depth);
    assert(!std::strcmp(a.diagnostic.message,b.diagnostic.message));
}

int main() {
    // Keyword initial filtering keeps case/whitespace and identifier boundaries.
    equivalent("10 letter=3:printer=4\n20 remnant=letter+printer\n30 print remnant\n40 end\n",true);
    equivalent("  a=2\n  b=3\n  print a+b\n  end\n");
    equivalent("A=3:B=4:X=0:Y=0\nG=INT(A*B):COLOR G,G,G:PSET X,Y\nPRINT G\nEND",false,false,true);
    equivalent("A=3\nB=4\nX=0\nY=0\nG=INT(A*B)\nCOLOR G,G,G\nREM source map only\nPSET X,Y\nPRINT G\nEND",false,false,true);
    equivalent("10 A=3:B=4:G=7:X=0:Y=0\n20 GOTO 40\n30 G=INT(A*B):COLOR G,G,G\n40 PSET X,Y\n50 PRINT G:END",true);
    // Local graphics: clamping, fractional/negative coordinates, aliasing and high slots.
    equivalent("PRINT F()\nEND\nFUNCTION F()\nA=3:B=4:X=-0.7:Y=1.9\nG=INT(A*B)\nCOLOR G,G,G\nPSET X,Y\nRETURN G\nEND FUNCTION",false,true,true);
    for(const char* product:{"-2*3","100*10","0.7*3"})
        equivalent(std::string("PRINT F()\nEND\nFUNCTION F()\nA=")+product+"\nB=1:X=0:Y=0\nX=INT(A*B):COLOR X,X,X:PSET X,Y\nRETURN X\nEND FUNCTION",false,true,true);
    std::string gray_high="PRINT F()\nEND\nFUNCTION F()\n";
    for(int i=0;i<128;++i)gray_high+="V"+std::to_string(i)+"="+std::to_string(i)+"\n";
    gray_high+="V127=INT(V64*V126):COLOR V127,V127,V127:PSET V127,V0\nRETURN V127\nEND FUNCTION";
    equivalent(gray_high,false,true,true);
    equivalent("PRINT F()\nEND\nFUNCTION F()\nA=2:B=3:X=0:Y=0\nG=INT(A*B)\nIF A>0 THEN\nCOLOR G,G,G\nEND IF\nPSET X,Y\nRETURN G\nEND FUNCTION");
    gray_overflow_equivalent();
    // All expression patterns, self assignment and evaluation order; no FMA.
    equivalent("PRINT F()\nEND\nFUNCTION F()\nA=2:B=3:C=5:D=7:E=11\n"
        "X=A*B-C*D+E\nY=A+B*C\nZ=A*B+C\nQ=2*A*B+C\nA=A+B\nB=B-C\nC=C*D\n"
        "PRINT X,Y,Z,Q,A,B,C\nPRINT A+3,B-4,5*C,A+B,A-B,A*B\nRETURN X+Y+Z+Q\nEND FUNCTION",false,true);
    // A rounding boundary distinguishes separate float multiply/add from FMA.
    const float ra=1.0f+std::ldexp(1.0f,-23),rb=1.0f-std::ldexp(1.0f,-23);
    volatile float rounded_product=ra*rb;
    assert(rounded_product-1.0f==0.0f&&std::fma(ra,rb,-1.0f)!=0.0f);
    const std::string rounding="PRINT ROUNDING()\nEND\nFUNCTION ROUNDING()\n"
        "A=1.00000011920928955078125\nB=0.99999988079071044921875\nC=-1\n"
        "R=C+A*B\nRETURN R=0\nEND FUNCTION";
    equivalent(rounding,false,true);
    rmb::ProgramStore rounding_program;load(rounding_program,rounding);
    auto rounding_il=std::make_unique<rmb::CompiledProgram>();rmb::BasicCompiler rounding_compiler;
    assert(rounding_compiler.compile(rounding_program,*rounding_il).ok);
    auto rounding_vm=std::make_unique<rmb::VM>();
    assert(execute(*rounding_vm,*rounding_il).text=="-1\r\n");
    // Actual flow entries and returns may not be swallowed.
    equivalent("PRINT F(3)\nEND\nFUNCTION F(N)\nS=0\nFOR I=1 TO N\n"
        "IF I=1 THEN\nS=S+2*I\nELSEIF I=2 THEN\nS=S+3*I\nELSE\nS=S+4*I\nEND IF\nNEXT I\n"
        "DO\nS=S-1\nLOOP UNTIL S<10\nWHILE S>5\nS=S-1\nWEND\nRETURN S\nEND FUNCTION",false,true);
    equivalent("10 A=2:B=3\n20 GOSUB 100\n30 ON 2 GOTO 60,70\n40 END\n60 PRINT 6:END\n70 PRINT A:END\n100 A=A+B:RETURN\n",true);
    equivalent("10 A=2:B=3\n20 ON 1 GOSUB 100,110\n30 PRINT A:END\n100 A=A*B:RETURN\n110 A=A-B:RETURN\n",true);
    // Original error PC, physical row and caller survive earlier fusions.
    for(const char* expression:{"1/0","SQR(-1)","LOG(0)"})
        equivalent(std::string("PRINT F()\nEND\nFUNCTION F()\nA=2\nB=3\nC=A+B\nD=")+expression+"\nRETURN C\nEND FUNCTION",false,true);
    equivalent("PRINT FACT(16)\nEND\nFUNCTION FACT(N)\nIF N<=1 THEN\nRETURN 1\nEND IF\nRETURN N*FACT(N-1)\nEND FUNCTION",false,true);
    equivalent("PRINT FACT(17)\nEND\nFUNCTION FACT(N)\nIF N<=1 THEN\nRETURN 1\nEND IF\nRETURN N*FACT(N-1)\nEND FUNCTION");
    equivalent("PRINT JOIN$(\"A\")+JOIN$(\"B\")\nEND\nFUNCTION JOIN$(S$)\nT$=S$+\"!\"\nRETURN T$\nEND FUNCTION");
    equivalent("G=3:PRINT F(2):PRINT G\nEND\nFUNCTION F(N)\nGLOBAL G\n\n  REM keep source rows\n  A=N+G\nIF A>0 THEN\nIF N=2 THEN\nG=A*2\nEND IF\nEND IF\nRETURN A\nEND FUNCTION",false,true);
    // Slots 64..127 must never alias a global 6-bit operand.
    std::string high="G=777:PRINT F():PRINT G\nEND\nFUNCTION F()\n";
    for(int i=0;i<128;++i)high+="V"+std::to_string(i)+"="+std::to_string(i)+"\n";
    high+="V127=V64+V126*V127\nPRINT V0,V64,V126\nRETURN V127\nEND FUNCTION";
    equivalent(high,false,true);
    rmb::Op op;op.code=rmb::OpCode::LOCAL_NUM_FUSED;op.flags=static_cast<unsigned>(rmb::LocalNumericPattern::Move);op.s=2;
    op.a=127|(64<<7);assert(rmb::valid_local_numeric(op,128,0));
    assert(!rmb::valid_local_numeric(op,127,0));op.a|=1<<14;assert(!rmb::valid_local_numeric(op,128,0));
    // Successful return, nested OOM and BREAK release every active frame.
    rmb::ProgramStore p;load(p,"PRINT F(12)\nEND\nFUNCTION F(N)\nIF N=0 THEN\nRETURN 1\nEND IF\nRETURN F(N-1)+1\nEND FUNCTION");
    auto il=std::make_unique<rmb::CompiledProgram>();rmb::BasicCompiler compiler;assert(compiler.compile(p,*il).ok);
    auto vm=std::make_unique<rmb::VM>();
    track=true;fail_at=2;allocations=0;auto oom=execute(*vm,*il);track=false;
    assert(!oom.diagnostic.ok&&!std::strcmp(oom.diagnostic.message,"OUT OF MEMORY AT ROW 7")&&oom.diagnostic.source_row==7&&oom.diagnostic.call_depth==1&&allocations==2&&live==0);
    track=true;fail_at=0;allocations=0;assert(execute(*vm,*il).diagnostic.ok);track=false;assert(live==0);
    load(p,"PRINT F()\nEND\nFUNCTION F()\nS=0\nDO\nS=S+1\nLOOP\nRETURN S\nEND FUNCTION");
    assert(compiler.compile(p,*il).ok);
    rmb::platform::stage3_break_polls=2;track=true;allocations=0;
    auto interrupted=execute(*vm,*il);track=false;
    assert(interrupted.diagnostic.interrupted&&live==0);
    rmb::platform::stage3_break_polls=0;
    load(p,"PRINT 42\nEND");assert(compiler.compile(p,*il).ok);assert(execute(*vm,*il).diagnostic.ok);
    // Public IL mutation must invalidate the RUN-scoped proof automatically.
    load(p,"PRINT F()\nEND\nFUNCTION F()\nA=1:B=2:C=A+B\nRETURN C\nEND FUNCTION");
    assert(compiler.compile(p,*il).ok&&rmb::validate_compiled_program(*il));
    bool corrupted=false;
    for(std::size_t i=0;i<il->code_count;++i)if(il->code[i].code==rmb::OpCode::LOCAL_NUM_FUSED) {
        il->code[i].s=65535;corrupted=true;break;
    }
    assert(corrupted&&!rmb::validate_compiled_program(*il));
    auto bad=execute(*vm,*il);assert(!bad.diagnostic.ok&&std::strstr(bad.diagnostic.message,"BAD LOCAL FUSION"));
    rmb::CompiledProgramCache bad_cache;assert(!bad_cache.store(p.revision(),*il));
    assert(compiler.compile(p,*il).ok&&execute(*vm,*il).diagnostic.ok);
    // Cross-FUNCTION control flow cannot earn the RUN-scoped proof. Raw IL
    // still takes the checked runtime path and reports the local-frame error.
    load(p,"PRINT SMALL()\nEND\nFUNCTION SMALL()\nRETURN 1\nEND FUNCTION\n"
        "FUNCTION BIG()\nA=1:B=2:C=A+B\nRETURN C\nEND FUNCTION");
    assert(compiler.compile(p,*il).ok&&il->function_count==2);
    const auto entry=il->functions[0].entry_pc;
    int other=-1;
    for(int i=il->functions[1].entry_pc;i<il->functions[1].end_pc;++i)
        if(il->code[i].code==rmb::OpCode::LOCAL_NUM_FUSED){other=i;break;}
    assert(other>=0);
    il->code[entry]={rmb::OpCode::JMP,0,0,other,0};
    assert(!rmb::validate_compiled_program(*il));
    auto wrong_frame=execute(*vm,*il);
    assert(!wrong_frame.diagnostic.ok&&std::strstr(wrong_frame.diagnostic.message,"BAD LOCAL FUSION"));
    // Invalid parameter metadata must fail before writing outside a new frame.
    load(p,"PRINT F(1)\nEND\nFUNCTION F(N)\nRETURN N\nEND FUNCTION");
    assert(compiler.compile(p,*il).ok);
    il->functions[0].locals[0].slot=128;
    assert(!rmb::validate_compiled_program(*il));
    auto bad_parameter=execute(*vm,*il);
    assert(!bad_parameter.diagnostic.ok&&std::strstr(bad_parameter.diagnostic.message,"BAD LOCAL SLOT"));
    // A corrupt retained graphics tail must not be trusted on a raw RUN.
    load(p,"PRINT F()\nEND\nFUNCTION F()\nA=2:B=3:X=0:Y=0\nG=INT(A*B):COLOR G,G,G:PSET X,Y\nRETURN G\nEND FUNCTION");
    assert(compiler.compile(p,*il).ok);
    bool gray_corrupted=false;
    for(std::size_t i=0;i<il->code_count;++i)if(il->code[i].code==rmb::OpCode::LOCAL_GRAY_PSET_MUL_INT) {
        il->code[i+1].a=128;gray_corrupted=true;break;
    }
    assert(gray_corrupted&&!rmb::validate_compiled_program(*il));
    auto bad_gray=execute(*vm,*il);
    assert(!bad_gray.diagnostic.ok&&std::strstr(bad_gray.diagnostic.message,"BAD LOCAL GRAPHICS FUSION"));
    std::puts("Stage 3 optimizer, diagnostics, slots, cache and cleanup: PASS");
}
