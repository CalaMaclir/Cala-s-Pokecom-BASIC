// Timing and verification paths share compile/VM/cache operations.
// Identical harness copied to Stage 1, Stage 2 and current in Actions.
#include "benchmark_platform.hpp"
#include "compiled_cache.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
using Clock=std::chrono::steady_clock;
std::uint32_t fingerprint(const std::string& s) {
    std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;
}
template<class F> std::vector<double> times(F f,int repetitions,int trials) {
    for(int i=0;i<3;++i)f();
    std::vector<double> values;
    for(int t=0;t<trials;++t) {
        const auto start=Clock::now();for(int i=0;i<repetitions;++i)f();
        values.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count()/repetitions);
    }
    return values;
}
void array(std::ostream& out,const std::vector<double>& values) {
    out<<"[";for(std::size_t i=0;i<values.size();++i){if(i)out<<",";out<<values[i];}out<<"]";
}
template<class F> double median_us(F f,int repetitions) {
    for(int i=0;i<3;++i)f();
    std::vector<double> values;
    for(int trial=0;trial<9;++trial) {
        const auto begin=Clock::now();for(int i=0;i<repetitions;++i)f();
        values.push_back(std::chrono::duration<double,std::micro>(Clock::now()-begin).count()/repetitions);
    }
    std::sort(values.begin(),values.end());return values[4];
}
int classic_short(int argc,char** argv) {
    std::ostringstream json;json<<std::setprecision(12)<<"{\"cases\":[";
    const char* names[]={"arithmetic","numeric-functions","mandel_text"};
    const char* programs[]={
        "10 S=0\n20 FOR I=1 TO 5000\n30 S=S+I*0.25\n40 NEXT I\n50 PRINT S\n60 END\n",
        "10 S=0\n20 FOR I=1 TO 1000\n30 S=S+SIN(I*0.001)+SQR(I)+ABS(-I)\n40 NEXT I\n50 PRINT S\n60 END\n",
        nullptr};
    for(int k=0;k<3;++k) {
        rmb::ProgramStore program;
        assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        std::string text;
        if(programs[k])text=programs[k];
        else {std::ifstream file("examples/mandel_text.bas");assert(file.good());text.assign(std::istreambuf_iterator<char>(file),{});}
        std::istringstream source(text);std::string line;
        while(std::getline(source,line)) {
            char* end=nullptr;const auto n=std::strtol(line.c_str(),&end,10);
            if(n>0){while(*end==' ')++end;assert(program.set_line(n,end));}
        }
        rmb::BasicCompiler compiler;auto il=std::make_unique<rmb::CompiledProgram>();
        auto compile=[&](){const auto result=compiler.compile(program,*il);assert(result.ok);};
        compile();const double compile_us=median_us(compile,300);
        auto vm=std::make_unique<rmb::VM>();
        auto execute=[&](){output.clear();const auto result=vm->run(*il);assert(result.ok);};
        execute();const auto expected=fingerprint(output);
        const double vm_us=median_us(execute,15);assert(fingerprint(output)==expected);
        vm->set_profile_mode(rmb::VmProfileMode::Counts);execute();
        const auto& profile=vm->profile_report();assert(profile.valid&&profile.run_ok);
        if(k)json<<",";
        json<<"{\"name\":\""<<names[k]<<"\",\"compile_us\":"<<compile_us
            <<",\"vm_us\":"<<vm_us<<",\"dispatches\":"<<profile.dispatches
            <<",\"logical_ops\":"<<profile.logical_ops<<",\"ops\":"<<il->code_count
            <<",\"output_hash\":"<<expected<<"}";
    }
    json<<"]}\n";std::cout<<json.str();
    if(argc>1){std::ofstream report(argv[1]);assert(report.good());report<<json.str();}
    return 0;
}

int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--classic-short")return classic_short(argc-1,argv+1);
    const bool verify=argc>1&&(std::string(argv[1])=="--verify-only"||std::string(argv[1])=="--verify-classic");
    const bool verify_classic=argc>1&&std::string(argv[1])=="--verify-classic";
    const bool paired=argc==3&&std::string(argv[1])=="--paired-case";
    const bool measure=argc>1&&std::string(argv[1])=="--measure";
    const int trials=verify?0:(measure?9:1), cr=measure?3000:1, cache_r=measure?100:1;
    std::vector<std::string> files={"examples/mandel_text.bas","examples/picocalc_mand.bas"};
    for(const char* name:{"arithmetic","numeric","fractal"}) {
        files.push_back(std::string("examples/stage3/")+name+"-classic.bas");
#ifndef CPB_STAGE1
        for(const char* mode:{"colon","rows","function"})
            files.push_back(std::string("examples/stage3/")+name+"-"+mode+".bas");
#endif
    }
#ifndef CPB_STAGE1
    files.push_back("examples/stage3/tiny-function.bas");
#endif
    if(paired)files={argv[2]};
    if(verify_classic)files.erase(std::remove_if(files.begin(),files.end(),[](const std::string& f){return f.find("stage3/")!=std::string::npos&&f.find("-classic.bas")==std::string::npos;}),files.end());
    std::cout<<std::setprecision(12);
    if(!paired)std::cout<<"{\"trials\":"<<trials<<",\"compile_repetitions\":"<<cr
             <<",\"cache_repetitions\":"<<cache_r<<",\"warmup\":"<<(verify?0:3)<<",\"cases\":[";
    bool comma=false;
    for(const auto& file:files) {
        std::ifstream source(file);assert(source.good());
        rmb::ProgramStore program;assert(program.initialize(rmb::ProgramStorageMode::InternalRam));
        const bool classic=file.find("classic")!=std::string::npos||(file.find("stage3/")==std::string::npos&&file.find("v093/performance/")==std::string::npos);
#ifndef CPB_STAGE1
        assert(program.new_program(classic?rmb::ProgramSourceMode::ClassicNumbered:rmb::ProgramSourceMode::Structured));
#endif
        std::string line;
        while(std::getline(source,line)) {
            if(classic) {
                char* end=nullptr;auto n=std::strtol(line.c_str(),&end,10);
                while(*end==' ')++end;assert(n>0&&program.set_line(n,end));
            } else {
#ifndef CPB_STAGE1
                const char* row[]={line.c_str()};assert(program.replace_source_rows(program.size(),0,row,1));
#endif
            }
        }
        auto il=std::make_unique<rmb::CompiledProgram>();rmb::BasicCompiler compiler;
        auto compile=[&](){const auto r=compiler.compile(program,*il);
            if(!r.ok)std::cerr<<file<<": "<<r.message<<"\n";assert(r.ok);};
        compile();
        auto vm=std::make_unique<rmb::VM>();
        auto execute=[&](){
            output.clear();pixels=0;graphics_hash=2166136261u;current_color=0;
            const auto r=vm->run(*il);if(!r.ok)std::cerr<<file<<": "<<r.message<<"\n";assert(r.ok);
        };
        execute();
        const auto oh=fingerprint(output),gh=graphics_hash,px=pixels;
        const int vr=measure?(file.find("fractal")!=std::string::npos||file.find("picocalc")!=std::string::npos?2:100):1;
        const auto ct=(paired||verify)?std::vector<double>{}:times(compile,cr,trials);
        const auto vt=(paired||verify)?std::vector<double>{}:times(execute,vr,trials);
        assert(fingerprint(output)==oh&&graphics_hash==gh&&pixels==px);
        rmb::CompiledProgramCache cache;
        auto miss=[&](){cache.invalidate();compile();assert(cache.store(program.revision(),*il));};
        auto hit=[&](){
#ifdef CPB_STAGE1
            assert(cache.restore(program.revision(),*il));
#else
            assert(cache.restore(program.revision(),*il,program.source_mode()));
#endif
        };
        if(verify){miss();execute();assert(fingerprint(output)==oh&&graphics_hash==gh&&pixels==px);hit();}
        const auto mt=(paired||verify)?std::vector<double>{}:times(miss,cache_r,trials);
        const auto ht=(paired||verify)?std::vector<double>{}:times(hit,cache_r,trials);
        execute();assert(fingerprint(output)==oh&&graphics_hash==gh&&pixels==px);
        vm->set_profile_mode(rmb::VmProfileMode::Counts);execute();
        const auto profile=vm->profile_report();assert(profile.valid&&profile.run_ok);
        if(!paired&&comma)std::cout<<",";comma=true;
        std::cout<<"{\"name\":\""<<file<<"\",\"compile_raw_us\":";array(std::cout,ct);
        std::cout<<",\"vm_raw_us\":";array(std::cout,vt);
        std::cout<<",\"cache_miss_raw_us\":";array(std::cout,mt);
        std::cout<<",\"cache_hit_raw_us\":";array(std::cout,ht);
        std::cout<<",\"vm_repetitions\":"<<vr<<",\"output_hash\":"<<oh<<",\"graphics_hash\":"<<gh
                 <<",\"pixels\":"<<px<<",\"ops\":"<<il->code_count<<",\"dispatches\":"<<profile.dispatches
                 <<",\"logical_ops\":"<<profile.logical_ops<<",\"op_counts\":[";
        for(std::size_t i=0;i<rmb::kVmProfileOpcodeSlots;++i){if(i)std::cout<<",";std::cout<<profile.op_count[i];}
        std::cout<<"]}";
        if(paired) {
            // One persistent process per revision/case, controlled serially by the
            // driver. PROFILE counts are complete before any timing command.
            std::cout<<"\n"<<std::flush;
            vm->set_profile_mode(rmb::VmProfileMode::Off);
            miss(); // Seed the hit path, including on a VM-only measurement.
            std::string metric;int repetitions=0,warmup=0;
            while(std::cin>>metric>>repetitions>>warmup) {
                assert(repetitions>0&&repetitions<=1000000);
                std::function<void()> action;
                if(metric=="compile")action=compile;
                else if(metric=="vm")action=execute;
                else if(metric=="cache_miss")action=miss;
                else {assert(metric=="cache_hit");action=hit;}
                if(warmup) {
                    for(int i=0;i<repetitions;++i)action();
                    std::cout<<"{\"warmup\":true}\n"<<std::flush;
                } else {
                    const auto sample=times(action,repetitions,1);
                    if(metric=="vm")assert(fingerprint(output)==oh&&graphics_hash==gh&&pixels==px);
                    std::cout<<"{\"us\":"<<sample[0]<<"}\n"<<std::flush;
                }
            }
        }
        cache.invalidate();
    }
    if(!paired)std::cout<<"]}\n";
    return 0;
}

