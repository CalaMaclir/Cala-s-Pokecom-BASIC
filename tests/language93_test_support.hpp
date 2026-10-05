#pragma once
#include "benchmark_platform.hpp"
#include "compiled_cache.hpp"
#include <memory>
#include <sstream>
#include <iostream>
namespace rmb { extern bool optimizer_test_disabled, vm_test_generic, vm_test_unverified; }
namespace rmb::platform { int stage3_break_polls=0; }

struct Language93Session {
    rmb::ProgramStore source;
    rmb::BasicCompiler compiler;
    std::unique_ptr<rmb::CompiledProgram> code=std::make_unique<rmb::CompiledProgram>();
    std::unique_ptr<rmb::VM> vm=std::make_unique<rmb::VM>();
    Language93Session() { assert(source.initialize(rmb::ProgramStorageMode::InternalRam)); }
    rmb::CompileResult compile(const std::string& text, bool classic=false) {
        assert(source.new_program(classic?rmb::ProgramSourceMode::ClassicNumbered:rmb::ProgramSourceMode::Structured));
        std::istringstream input(text);std::string row;int number=10;
        while(std::getline(input,row)) {
            if(classic)assert(source.set_line(number,row.c_str()));
            else {const char* rows[]={row.c_str()};assert(source.replace_source_rows(source.size(),0,rows,1));}
            number+=10;
        }
        return compiler.compile(source,*code);
    }
    void require_compile(const std::string& text,bool classic=false) {
        const auto result=compile(text,classic);
        if(!result.ok)std::cerr<<text<<"\ncompile: "<<result.message<<" row "<<result.row<<"\n";
        assert(result.ok);assert(rmb::validate_compiled_program(*code));
    }
    void run(const rmb::CompiledProgram& program,const std::string& expected,const char* error=nullptr) {
        output.clear();const auto result=vm->run(program);
        if(error) {
            if(result.ok||std::strncmp(result.message,error,std::strlen(error)))std::cerr<<"Expected "<<error<<" got "<<result.message<<"\n";
            assert(!result.ok&&!std::strncmp(result.message,error,std::strlen(error)));
        } else {
            if(!result.ok||output!=expected)std::cerr<<"run: "<<result.message<<" expected ["<<expected<<"] got ["<<output<<"]\n";
            assert(result.ok&&output==expected);
        }
    }
    void check_runs(const std::string& expected,const char* error=nullptr) {
        run(*code,expected,error);run(*code,expected,error);
        rmb::CompiledProgramCache cache;auto restored=std::make_unique<rmb::CompiledProgram>();
        assert(!cache.restore(source.revision(),*restored,code->source_mode));
        assert(cache.store(source.revision(),*code));
        assert(cache.restore(source.revision(),*restored,code->source_mode));
        run(*restored,expected,error);run(*restored,expected,error);cache.invalidate();
        rmb::vm_test_generic=true;run(*code,expected,error);rmb::vm_test_generic=false;
        rmb::vm_test_unverified=true;run(*code,expected,error);rmb::vm_test_unverified=false;
    }
};
static void language93_case(const std::string& text,const std::string& expected,bool classic=false,const char* error=nullptr) {
    for(bool optimize:{false,true}) {
        rmb::optimizer_test_disabled=!optimize;
        Language93Session session;session.require_compile(text,classic);session.check_runs(expected,error);
    }
    rmb::optimizer_test_disabled=false;
}
static void language93_reject(const std::string& text,const char* message,bool classic=false) {
    Language93Session session;const auto result=session.compile(text,classic);
    if(result.ok||!std::strstr(result.message,message))std::cerr<<text<<" expected "<<message<<" got "<<result.message<<"\n";
    assert(!result.ok&&std::strstr(result.message,message));
}
