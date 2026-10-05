#include "language93_test_support.hpp"
#include <functional>
#include <cstddef>

extern "C" void* __real_realloc(void*,std::size_t);
static int fail_realloc_after=0;
extern "C" void* __wrap_realloc(void* memory,std::size_t bytes) {
    if(fail_realloc_after>0&&--fail_realloc_after==0)return nullptr;
    return __real_realloc(memory,bytes);
}

struct CacheImageHeader {
    std::uint32_t magic;std::uint16_t version,header_bytes;std::uint64_t revision;
    std::uint32_t code_count,number_count,string_used,symbol_count,line_count;
    rmb::ProgramSourceMode mode;
    std::uint32_t rows,functions,locals,data_count,crc;
};
static std::uint32_t crc32(const std::uint8_t* bytes,std::size_t length) {
    std::uint32_t crc=0xffffffffu;
    while(length--) {crc^=*bytes++;for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
    return crc^0xffffffffu;
}
static void corrupt_cache(Language93Session& s,const std::function<void(CacheImageHeader&,std::vector<std::uint8_t>&)>& edit,bool fix_crc=true) {
    rmb::CompiledProgramCache cache;auto restored=std::make_unique<rmb::CompiledProgram>();
    assert(cache.store(s.source.revision(),*s.code));
    const auto region=rmb::psram::allocation(rmb::psram::Client::CompiledCache);
    CacheImageHeader header{};assert(rmb::psram::read(region.base_address,&header,sizeof(header)));
    assert(header.version==6&&header.header_bytes==sizeof(header));
    const auto size=header.code_count*sizeof(rmb::Op)+header.number_count*sizeof(rmb::BasicNumber)+
        header.string_used+header.symbol_count*sizeof(rmb::Symbol)+header.line_count*sizeof(rmb::LinePc)+
        header.rows*sizeof(rmb::SourceRowPc)+header.functions*offsetof(rmb::FunctionInfo,locals)+
        header.locals*sizeof(rmb::LocalSymbol)+header.data_count*sizeof(rmb::DataItem);
    std::vector<std::uint8_t> payload(size);
    assert(rmb::psram::read(region.base_address+header.header_bytes,payload.data(),payload.size()));
    edit(header,payload);
    if(fix_crc)header.crc=crc32(payload.data(),payload.size());
    assert(rmb::psram::write(region.base_address,&header,sizeof(header)));
    assert(rmb::psram::write(region.base_address+header.header_bytes,payload.data(),payload.size()));
    // Even a prepopulated destination must be cleared on every failed restore.
    assert(restored->append_data({0}));
    assert(!cache.restore(s.source.revision(),*restored,s.code->source_mode));
    assert(!restored->data_items&&!restored->data_count&&!restored->code_count);
    assert(!cache.valid()&&!rmb::psram::allocation(rmb::psram::Client::CompiledCache).active);
}

int main() {
    for(bool classic:{false,true}) {
        language93_case("DATA 1,2,3\nREAD A,B,C\nPRINT A;\",\";B;\",\";C", "1,2,3\r\n",classic);
        language93_case("DATA -1,+2.5,1.0E3,&H10,-&HFF,.5,-0\nREAD A,B,C,D,E,F,G\nPRINT A;\",\";B;\",\";C;\",\";D;\",\";E;\",\";F;\",\";G", "-1,2.5,1000,16,-255,0.5,-0\r\n",classic);
        language93_case("DATA \"ABC\",\"\",\"A,B\",\"HELLO WORLD\"\nREAD A$,B$,C$,D$\nPRINT A$;\"|\";B$;\"|\";C$;\"|\";D$", "ABC||A,B|HELLO WORLD\r\n",classic);
        language93_case("DATA 1\nEND\nDATA 2,3", "",classic);
        language93_case("READ A,B,C\nDATA 1\nDATA 2:DATA 3\nPRINT A;B;C", "123\r\n",classic);
        language93_case("DATA 1,2,\"S\",\"T\"\nDIM A(2),B$(2,2)\nREAD A(1),A(2),B$(1,1),B$(1,2)\nPRINT A(1);A(2);B$(1,1);B$(1,2)", "12ST\r\n",classic);
        language93_case("DATA 5,6\nREAD A\nRESTORE\nREAD B,C\nPRINT A;B;C", "556\r\n",classic);
        language93_case("RESTORE\nREAD A", "",classic,"OUT OF DATA");
        language93_case("DATA 1\nREAD A,B", "",classic,"OUT OF DATA");
        language93_case("DATA 1\nREAD A$", "",classic,"READ TYPE MISMATCH");
        language93_case("DATA \"1\"\nREAD A", "",classic,"READ TYPE MISMATCH");
        language93_case("DATA 1\nDIM A(2)\nREAD A(3)", "",classic,"SUBSCRIPT OUT OF RANGE");
        for(const char* bad:{"DATA A+1","DATA SIN(1)","DATA PI","DATA X","DATA HELLO","DATA 1,", "DATA", "DATA 1+2","DATA (1)","DATA -\"X\"","DATA &H"}) {
            Language93Session s;assert(!s.compile(bad,classic).ok);assert(!s.code->data_items&&!s.code->data_count);
        }
        for(const char* bad:{"RESTORE 1000","RESTORE LABEL","RESTORE FUNCTION"})language93_reject(bad,"RESTORE TAKES NO ARGUMENT",classic);
    }
    // Classic stream is line-number order even when lines were entered in reverse.
    Language93Session ordered;
    assert(ordered.source.new_program(rmb::ProgramSourceMode::ClassicNumbered));
    assert(ordered.source.set_line(50,"PRINT A;B;C"));assert(ordered.source.set_line(30,"DATA 3"));
    assert(ordered.source.set_line(10,"DATA 1"));assert(ordered.source.set_line(40,"READ A,B,C"));
    assert(ordered.source.set_line(20,"DATA 2"));assert(ordered.compiler.compile(ordered.source,*ordered.code).ok);
    ordered.check_runs("123\r\n");
    language93_case("IF 0 THEN\nDATA 1\nEND IF\nFOR I=5 TO 1\nDATA 2\nNEXT I\nREAD A,B\nPRINT A;B", "12\r\n");
    language93_case("DATA 1,2,\"LOCAL\",3,\"GLOBAL\"\nA=50\nPRINT F();\",\";A;\",\";S$\nEND\n"
        "FUNCTION F()\nGLOBAL A,S$\nREAD B,C,T$,A,S$\nPRINT T$\nRETURN B+C\nEND FUNCTION", "LOCAL\r\n3,3,GLOBAL\r\n");
    language93_case("DATA 7,8\nPRINT F();G()\nRESTORE\nPRINT F()\nEND\nFUNCTION F()\nREAD A\nRETURN A\nEND FUNCTION\nFUNCTION G()\nREAD A\nRETURN A\nEND FUNCTION", "78\r\n7\r\n");
    language93_reject("END\nFUNCTION F()\nDATA 1\nRETURN 0\nEND FUNCTION","DATA NOT ALLOWED IN FUNCTION");
    language93_reject("END\nFUNCTION F()\nREAD A(1)\nRETURN 0\nEND FUNCTION","ARRAYS NOT SUPPORTED IN FUNCTION");
    Language93Session s;
    for(const char* direct:{"DATA 1","READ A","RESTORE"}) {
        const auto result=s.compiler.compile_direct(direct,*s.code);
        assert(!result.ok&&std::strstr(result.message,"REQUIRE STORED PROGRAM"));
        assert(!s.code->data_items&&!s.code->data_count&&!s.code->code_count);
    }
    s.require_compile("DATA 9,\"LAST\"\nREAD A,B$\nPRINT A;B$");
    output.clear();const auto direct_result=s.vm->run_direct(*s.code);
    assert(!direct_result.ok&&std::strstr(direct_result.message,"REQUIRE STORED PROGRAM"));
    s.check_runs("9LAST\r\n");
    corrupt_cache(s,[](auto& h,auto&){h.data_count=rmb::kMaxDataItems+1;});
    corrupt_cache(s,[](auto&,auto& p){p[p.size()-4]=0;p[p.size()-3]=0x80;}); // invalid tag 2
    corrupt_cache(s,[](auto&,auto& p){p[p.size()-4]=0xff;p[p.size()-3]=0x3f;}); // number index
    corrupt_cache(s,[](auto&,auto& p){p[p.size()-2]=0xff;p[p.size()-1]=0x7f;}); // string offset
    corrupt_cache(s,[](auto& h,auto& p){p[h.code_count*sizeof(rmb::Op)+h.number_count*sizeof(rmb::BasicNumber)+h.string_used-1]='X';}); // missing NUL
    corrupt_cache(s,[](auto& h,auto& p){
        for(std::size_t i=0;i<h.code_count;++i) {
            rmb::Op op;std::memcpy(&op,p.data()+i*sizeof(op),sizeof(op));
            if(op.code==rmb::OpCode::READ_DATA_NUM){op.a=1;std::memcpy(p.data()+i*sizeof(op),&op,sizeof(op));break;}
        }
    });
    corrupt_cache(s,[](auto& h,auto& p){
        for(std::size_t i=0;i<h.code_count;++i) {
            rmb::Op op;std::memcpy(&op,p.data()+i*sizeof(op),sizeof(op));
            if(op.code==rmb::OpCode::READ_DATA_STR){op.flags=1;std::memcpy(p.data()+i*sizeof(op),&op,sizeof(op));break;}
        }
    });
    corrupt_cache(s,[](auto&,auto& p){p.back()^=1;},false); // CRC also covers DATA refs
    s.require_compile("DATA 1\nDO\nFOR I=1 TO 2\nEXIT DO\nNEXT I\nLOOP\nREAD A\nRESTORE\nPRINT A");
    corrupt_cache(s,[](auto& h,auto& p){
        for(std::size_t i=0;i<h.code_count;++i) {
            rmb::Op op;std::memcpy(&op,p.data()+i*sizeof(op),sizeof(op));
            if(op.code==rmb::OpCode::EXIT_LOOP){op.a=9999;std::memcpy(p.data()+i*sizeof(op),&op,sizeof(op));break;}
        }
    });
    corrupt_cache(s,[](auto& h,auto& p){
        for(std::size_t i=0;i<h.code_count;++i) {
            rmb::Op op;std::memcpy(&op,p.data()+i*sizeof(op),sizeof(op));
            if(op.code==rmb::OpCode::RESTORE_DATA){op.b=1;std::memcpy(p.data()+i*sizeof(op),&op,sizeof(op));break;}
        }
    });
    // DATA memory is exact after compile/cache restore: 0/typical/large counts.
    s.require_compile("PRINT 1");assert(!s.code->data_items&&!s.code->data_capacity);
    s.require_compile("DATA 1,2,3");assert(s.code->data_count==3&&s.code->data_capacity==3);
    std::string large;for(int row=0;row<64;++row){large+="DATA ";for(int n=0;n<64;++n)large+=n?",1":"1";large+='\n';}
    large+="READ A\nPRINT A";s.require_compile(large);
    assert(s.code->data_count==4096&&s.code->data_capacity==4096);s.check_runs("1\r\n");
    assert(!s.compile(large+"\nDATA 1").ok);assert(!s.code->data_items&&!s.code->data_count);
    // BREAK and errors discard the cursor along with all run-local state.
    s.require_compile("DATA 1,2\nFOR I=1 TO 5000\nRESTORE\nREAD A,B\nNEXT I\nPRINT A;B");
    rmb::platform::stage3_break_polls=2;const auto broken=s.vm->run(*s.code);assert(broken.interrupted);
    rmb::platform::stage3_break_polls=0;s.check_runs("12\r\n");
    s.require_compile("DATA 1\nREAD A,B");s.check_runs("","OUT OF DATA");
    s.require_compile("DATA 7\nREAD A\nPRINT A");s.check_runs("7\r\n");
    // Initial DATA allocation and final compaction failures free partial tables.
    for(int failure:{1,2}) {
        assert(s.source.new_program(rmb::ProgramSourceMode::ClassicNumbered));
        assert(s.source.set_line(10,"DATA 1,2,3"));
        fail_realloc_after=failure;const auto result=s.compiler.compile(s.source,*s.code);fail_realloc_after=0;
        assert(!result.ok&&!s.code->data_items&&!s.code->data_count&&!s.code->code_count);
    }
    s.require_compile("DATA 1\nREAD A\nPRINT A");
    rmb::CompiledProgramCache failed;auto restored=std::make_unique<rmb::CompiledProgram>();
    assert(failed.store(s.source.revision(),*s.code));fail_realloc_after=1;
    assert(!failed.restore(s.source.revision(),*restored,s.code->source_mode));fail_realloc_after=0;
    assert(!failed.valid()&&!restored->data_items);
    assert(!rmb::psram::allocation(rmb::psram::Client::CompiledCache).active);
    std::puts("Stage 3D DATA/READ/RESTORE: PASS (both modes, arrays, functions, cursor reset, format 6 CRC/structural validation)");
}
