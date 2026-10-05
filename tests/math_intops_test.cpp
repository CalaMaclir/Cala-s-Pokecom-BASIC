#include "benchmark_platform.hpp"
#include "compiled_cache.hpp"
#include "integer_numeric.hpp"
#include "extended_numeric_math.hpp"
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <iostream>
namespace rmb { extern bool optimizer_test_disabled, vm_test_generic, vm_test_unverified; }
namespace rmb::platform { int stage3_break_polls=0; }

static void expression(const char* text, float expected, const char* error=nullptr) {
    for (int mode=0; mode<4; ++mode) {
        for (bool optimize : {false, true}) {
            rmb::optimizer_test_disabled = !optimize;
            rmb::ProgramStore source;
            assert(source.initialize(rmb::ProgramStorageMode::InternalRam));
            const bool classic = mode==1;
            assert(source.new_program(classic ? rmb::ProgramSourceMode::ClassicNumbered :
                                              rmb::ProgramSourceMode::Structured));
            std::string program = mode==3
                ? std::string("PRINT F()\nEND\nFUNCTION F()\nA=3:B=4:C=A+B\nRETURN ")+text+"\nEND FUNCTION"
                : std::string("PRINT ")+text;
            std::istringstream stream(program); std::string row; int number=10;
            while (std::getline(stream,row)) {
                if (classic) assert(source.set_line(number,row.c_str()));
                else {const char* rows[]={row.c_str()}; assert(source.replace_source_rows(source.size(),0,rows,1));}
                number+=10;
            }
            auto il=std::make_unique<rmb::CompiledProgram>(); rmb::BasicCompiler compiler;
            const auto compiled=mode==0 ? compiler.compile_direct(program.c_str(),*il) : compiler.compile(source,*il);
            if (!compiled.ok) {std::cerr<<text<<" compile: "<<compiled.message<<"\n"; assert(compiled.ok);}
            assert(rmb::validate_compiled_program(*il));
            auto vm=std::make_unique<rmb::VM>();
            auto check=[&](const rmb::CompiledProgram& code) {
                output.clear(); const auto result=vm->run(code);
                if (error) {
                    if (result.ok || std::strncmp(result.message,error,std::strlen(error)))
                        std::cerr<<text<<" expected "<<error<<" got "<<result.message<<"\n";
                    assert(!result.ok && !std::strncmp(result.message,error,std::strlen(error)));
                    if (mode==3) assert(result.source_row==5 && result.call_depth==1);
                } else {
                    if (!result.ok) std::cerr<<text<<" run: "<<result.message<<"\n";
                    assert(result.ok); char* end=nullptr;
                    const float actual=std::strtof(output.c_str(),&end);
                    assert(end!=output.c_str());
                    if (std::fabs(actual-expected)>1e-5f*std::max(1.0f,std::fabs(expected)))
                        std::cerr<<text<<" expected "<<expected<<" got "<<actual<<"\n";
                    assert(std::fabs(actual-expected)<=1e-5f*std::max(1.0f,std::fabs(expected)));
                }
                return std::string(result.message)+"|"+output;
            };
            const auto first=check(*il); assert(check(*il)==first);
            rmb::CompiledProgramCache cache; auto hit=std::make_unique<rmb::CompiledProgram>();
            assert(cache.store(source.revision(),*il));
            assert(cache.restore(source.revision(),*hit,il->source_mode));
            assert(check(*hit)==first); cache.invalidate();
            rmb::vm_test_unverified=true; assert(check(*il)==first); rmb::vm_test_unverified=false;
            rmb::vm_test_generic=true; assert(check(*il)==first); rmb::vm_test_generic=false;
            // Exercise actual generic CALLFN as well as optimized FN1/FN2 paths.
            for (std::size_t i=0;i<il->code_count;++i) {
                auto& op=il->code[i];
                if (op.code==rmb::OpCode::FN1_NUM || op.code==rmb::OpCode::FN2_NUM)
                    op.code=rmb::OpCode::CALLFN;
            }
            assert(check(*il)==first);
        }
    }
    rmb::optimizer_test_disabled=false;
}

int main() {
    const float pi=3.14159265358979323846f;
    for (const auto& c : std::initializer_list<std::pair<const char*,float>>{
        {"LOG(1)",0},{"LOG(10)",1},{"LOG(100)",2},{"LOG(1000)",3},
        {"LN(1)",0},{"LN(EXP(1))",1},{"ASIN(0)",0},{"ASIN(1)",pi/2},{"ASIN(-1)",-pi/2},
        {"ACOS(1)",0},{"ACOS(0)",pi/2},{"ACOS(-1)",pi},
        {"ATAN2(0,1)",0},{"ATAN2(1,0)",pi/2},{"ATAN2(0,-1)",pi},{"ATAN2(-1,0)",-pi/2},
        {"ATAN2(1,2)",std::atan2(1.0f,2.0f)},{"ATAN2(-1,-1)",-3*pi/4},
        {"ATAN2(0,0)",0},{"ATN(1)",pi/4},{"DEG(ASIN(1))",90},
        {"17\\5",3},{"-17\\5",-3},{"17\\-5",-3},{"-17\\-5",3},{"17.9\\5.2",3},
        {"-17.9\\-5.2",3},{"-0.9\\5",0},
        {"1<<0",1},{"1<<1",2},{"1<<4",16},{"8>>1",4},{"-8>>1",-4},{"-9>>1",-5},
        {"-1>>0",-1},{"-1>>31",-1},{"5.9<<1.9",10},{"-0.5<<-0.5",0},
        {"(1<<31)=-2147483648",-1},{"(-1<<31)=-2147483648",-1},
        {"(-2147483648<<1)=0",-1},{"-2147483648>>31",-1},{"2147483520>>31",0},
        {"0 XOR 0",0},{"1 XOR 0",1},{"1 XOR 1",0},{"5 XOR 3",6},{"5.9 XOR 3.9",6},
        {"-1 XOR 3",-4},{"(1=1) XOR (1=0)",-1},{"(1=1) XOR (1=1)",0},
        {"2 AND 1",-1},{"0 AND 3",0},{"2 OR 0",-1},{"0 OR 0",0},{"NOT 0",-1},{"NOT 2",0},
        {"2+10\\3*4",14},{"2+(10\\(3*4))",2},{"20\\3 MOD 4",2},
        {"1+2<<3",24},{"1+(2<<3)",17},{"1<<2+1",8},{"(1<<2)+1",5},
        {"16>>1>>1",4},{"8>>1<5",-1},{"1<<2<=4",-1},{"1<<2>3",-1},{"1<<2>=4",-1},
        {"1<<2<>5",-1},{"1<2",-1},{"1<=1",-1},{"2>1",-1},{"2>=2",-1},
        {"6 XOR 1 AND 1",-7},{"(6 XOR 1) AND 1",-1},
        {"1 OR 1 XOR -1",-1},{"(1 OR 1) XOR -1",0},
        {"1=1 XOR 1=0 AND 0 OR 0",-1},{"1 XOR 3 XOR 7",5},
        {"(16777216 XOR 1)=16777216",-1},{"(16777216 XOR 1) XOR 16777216",0},
        {"((2147483520 XOR 127))=2147483648",-1},
        {"XORVALUE+LOGVALUE+LNUMBER+ASINVALUE",0}}) expression(c.first,c.second);
    for (const char* c : {"LOG(0)","LOG(-1)","LN(0)","LN(-1)","ASIN(1.01)","ASIN(-1.01)",
                          "ACOS(1.01)","ACOS(-1.01)","LOG(1e40)","LN(1e40)","ATAN2(1e40,0)"})
        expression(c,0,"DOMAIN ERROR");
    for (const char* c : {"1\\0","1\\0.9","1\\-0.9"}) expression(c,0,"DIVISION BY ZERO");
    expression("-2147483648\\-1",0,"INTEGER OVERFLOW");
    for (const char* c : {"1<<-1","1<<32","1<<33","1>>-1","1>>32","1>>33"})
        expression(c,0,"SHIFT COUNT ERROR");
    for (const char* c : {"2147483648\\1","-2147483904\\1","1e40\\2", "2147483648<<1",
                          "1<<2147483648","2147483648>>1","1>>1e40","2147483648 XOR 1",
                          "(2147483520 XOR 127) XOR 0"}) expression(c,0,"INTEGER RANGE ERROR");

    std::int32_t converted=0;
    for(float f : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(),2147483648.0f,-2147483904.0f}) {
        assert(!rmb::basic_to_int32(f,converted));
        for (auto op : {rmb::OpCode::IDIV_NUM,rmb::OpCode::SHL_NUM,rmb::OpCode::SHR_NUM,rmb::OpCode::BXOR_NUM}) {
            float result=0;
            assert(!std::strcmp(rmb::evaluate_integer_numeric(op,f,1,result),"INTEGER RANGE ERROR"));
            assert(!std::strcmp(rmb::evaluate_integer_numeric(op,1,f,result),"INTEGER RANGE ERROR"));
        }
        float result=0;
        for(int id : {rmb::FnId::ASIN,rmb::FnId::ACOS})
            assert(!rmb::evaluate_extended_unary(id,f,result));
        if (!std::isfinite(f)) {
            for(int id : {rmb::FnId::LOG,rmb::FnId::LN})
                assert(!rmb::evaluate_extended_unary(id,f,result));
            assert(!rmb::evaluate_atan2(f,1,result)); assert(!rmb::evaluate_atan2(1,f,result));
        }
    }
    assert(rmb::basic_to_int32(-2147483648.0f,converted)&&converted==INT32_MIN);
    assert(rmb::basic_to_int32(2147483520.0f,converted)&&converted==2147483520);
    assert(rmb::basic_to_int32(-1.9f,converted)&&converted==-1);

    rmb::BasicCompiler compiler; auto il=std::make_unique<rmb::CompiledProgram>();
    auto vm=std::make_unique<rmb::VM>();
    assert(compiler.compile_direct("XORVALUE=2:LOGVALUE=3:LNUMBER=4:ASINVALUE=5:PRINT XORVALUE+LOGVALUE+LNUMBER+ASINVALUE",*il).ok);
    output.clear(); assert(vm->run(*il).ok&&output=="14\r\n");
    for (const char* s : {"PRINT 1<<<2","PRINT 1><2","PRINT 1<<","PRINT 1 XOR", "PRINT 1>>=2",
                         "PRINT \"x\"\\1","PRINT 1 XOR \"x\"","PRINT \"x\"<<1","PRINT \"x\">>1"})
        assert(!compiler.compile_direct(s,*il).ok);
    for (const char* s : {"PRINT LN()","PRINT ASIN(1,2)","PRINT ATAN2(1)","PRINT ATAN2(1,2,3)"}) {
        assert(compiler.compile_direct(s,*il).ok); assert(!vm->run(*il).ok);
    }
    // Dedicated opcode validation and safe stack/type errors for externally supplied IL.
    for (auto op : {rmb::OpCode::IDIV_NUM,rmb::OpCode::SHL_NUM,rmb::OpCode::SHR_NUM,rmb::OpCode::BXOR_NUM}) {
        il->reset(); il->code_count=2;il->code[0]={op,0,0,0,0};il->code[1]={rmb::OpCode::HALT,0,0,0,0};
        assert(rmb::validate_compiled_program(*il));assert(!vm->run(*il).ok);
        il->code[0].a=1;assert(!rmb::validate_compiled_program(*il));
    }
    std::puts("Stage 3A/3B math/integer: PASS (Direct, Classic, Structured, FUNCTION; optimized/generic/cache)");
}
