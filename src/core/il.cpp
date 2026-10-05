#include "il.hpp"
#include "local_numeric_fusion.hpp"

#include <cstring>
#include <cstdlib>

namespace rmb {
// Validate restored operands as well as CRC. A correct CRC is not a PC/slot proof.
bool validate_compiled_program(const CompiledProgram& p) {
    if(p.code_count>kMaxOps||p.number_count>kNumberPoolSize||p.string_used<1||
       p.string_used>kStringPoolSize||p.symbol_count>kMaxSymbols||p.line_count>4096||
       p.source_row_count>1024||p.function_count>kMaxUserFunctions||
       p.data_count>kMaxDataItems||p.data_count>p.data_capacity||p.data_capacity>kMaxDataItems||
       (p.data_capacity&&!p.data_items)||
       (p.function_count&&!p.functions)||(p.source_row_count&&!p.source_rows)||
       (p.line_count>kMaxLineMap&&!p.extra_lines)||static_cast<unsigned>(p.source_mode)>1)return false;
    for(std::size_t i=0;i<p.function_count;++i) {
        const auto& fn=p.functions[i];
        if(fn.local_count>kMaxFunctionLocals||fn.parameter_count>kMaxFunctionParameters||
           fn.parameter_count>fn.local_count||fn.numeric_local_count+fn.string_local_count!=fn.local_count||
           (fn.local_count&&!fn.locals)||fn.entry_pc<0||fn.end_pc<=fn.entry_pc||
           static_cast<std::size_t>(fn.end_pc)>p.code_count||!std::memchr(fn.name,0,sizeof(fn.name)))return false;
        if(i&&p.functions[i-1].end_pc>fn.entry_pc)return false;
        for(std::size_t j=0;j<fn.local_count;++j) {
            const auto& local=fn.locals[j];
            if(local.slot>=(local.symbol.is_string?fn.string_local_count:fn.numeric_local_count)||
               !std::memchr(local.symbol.name,0,kSymbolNameLength))return false;
        }
    }
    for(std::size_t i=0;i<p.data_count;++i) {
        const auto item=p.data_items[i];
        if(item.type()==0) { if(item.index()>=p.number_count)return false; }
        else if(item.type()==1) {
            if(item.index()>=p.string_used||
               !std::memchr(p.string_pool+item.index(),0,p.string_used-item.index()))return false;
        } else return false;
    }
    auto pc=[&](int n){return n>=0&&static_cast<std::size_t>(n)<p.code_count;};
    auto slot=[&](int n){return n>=0&&static_cast<std::size_t>(n)<p.symbol_count;};
    auto owner=[&](int n) {
        for(std::size_t f=0;f<p.function_count;++f)
            if(n>=p.functions[f].entry_pc&&n<p.functions[f].end_pc)return static_cast<int>(f);
        return -1;
    };
    auto local_edge=[&](std::size_t from,int to) {
        return pc(to)&&owner(static_cast<int>(from))==owner(to);
    };
    for(std::size_t i=0;i<p.line_count;++i)if(p.line_at(i).pc<0||
        static_cast<std::size_t>(p.line_at(i).pc)>p.code_count)return false;
    for(std::size_t i=0;i<p.source_row_count;++i) {
        const auto& r=p.source_rows[i];
        if(r.row<=0||r.pc<0||static_cast<std::size_t>(r.pc)>p.code_count||
           r.function_id<-1||r.function_id>=static_cast<int>(p.function_count)||
           (i&&(r.row<=p.source_rows[i-1].row||r.pc<p.source_rows[i-1].pc)))return false;
    }
    for(std::size_t i=0;i<p.code_count;++i) {
        const auto& op=p.code[i];
        if(static_cast<unsigned>(op.code)>static_cast<unsigned>(OpCode::HALT))return false;
        const FunctionInfo* fn=nullptr;
        if(op.code>=OpCode::LOAD_LOCAL_NUM&&op.code<=OpCode::LOCAL_GRAY_PSET_MUL_INT&&
           op.code!=OpCode::CALL_USER&&op.code!=OpCode::RETURN_USER&&op.code!=OpCode::FUNCTION_FALLTHROUGH) {
            for(std::size_t f=0;f<p.function_count;++f)
                if(static_cast<int>(i)>=p.functions[f].entry_pc&&static_cast<int>(i)<p.functions[f].end_pc){fn=&p.functions[f];break;}
            if(!fn)return false;
        }
        switch(op.code) {
        case OpCode::DUP:case OpCode::DROP:
            if(op.flags||op.s||op.a||op.b)return false;
            break;
        case OpCode::READ_DATA_NUM:case OpCode::READ_DATA_STR:case OpCode::RESTORE_DATA:
            if(op.flags||op.s||op.a||op.b)return false;
            break;
        case OpCode::EXIT_LOOP:
            if(p.source_mode!=ProgramSourceMode::Structured||op.flags||op.s>16||
               op.b<0||op.b>op.s||!local_edge(i,op.a))return false;
            break;
        case OpCode::IDIV_NUM:case OpCode::SHL_NUM:case OpCode::SHR_NUM:case OpCode::BXOR_NUM:
            if(op.flags||op.s||op.a||op.b)return false;
            break;
        case OpCode::PUSH_NUM:
            if(op.a<0||static_cast<std::size_t>(op.a)>=p.number_count)return false;
            break;
        case OpCode::PUSH_STR:
            if(op.s>=p.string_used||!std::memchr(p.string_pool+op.s,0,p.string_used-op.s))return false;
            break;
        case OpCode::LOAD:case OpCode::STORE:case OpCode::LOAD_NUM:case OpCode::STORE_NUM:
        case OpCode::LOAD_STR:case OpCode::STORE_STR:case OpCode::LOAD_ARR:case OpCode::STORE_ARR:
        case OpCode::DIM_ARR:case OpCode::FOR_INIT:
            if(!slot(op.a))return false;
            break;
        case OpCode::FOR_INCR:
            if(op.a!=-1&&!slot(op.a))return false;
            break;
        case OpCode::JMP:case OpCode::JZ:case OpCode::GOSUB:
        case OpCode::CEQ_NUM_JZ:case OpCode::CNE_NUM_JZ:case OpCode::CLT_NUM_JZ:
        case OpCode::CLE_NUM_JZ:case OpCode::CGT_NUM_JZ:case OpCode::CGE_NUM_JZ:
        case OpCode::NOT_NUM_JZ:case OpCode::AND_NUM_JZ:case OpCode::OR_NUM_JZ:
        case OpCode::SQ2_GT_CONST_OR_JZ:case OpCode::SUMSQ_GT_CONST_JZ:
            if(!local_edge(i,op.a))return false;
            break;
        case OpCode::FOR_CHECK:if(!slot(op.a)||!local_edge(i,op.b))return false;
            break;
        case OpCode::COMPLEX_ITER_OR_JZ:case OpCode::COMPLEX_ITER_SUMSQ_JZ:
            if(!local_edge(i,op.b&0xfff)||static_cast<unsigned>((op.b>>12)&0xfff)>=p.number_count)return false;
            break;
        case OpCode::ON_GOTO:case OpCode::ON_GOSUB:
            if(op.a<=0||i+op.a>=p.code_count)return false;
            break;
        case OpCode::CALL_USER:
            if(op.a<0||static_cast<unsigned>(op.a)>=p.function_count||
               op.b!=p.functions[op.a].parameter_count)return false;
            break;
        case OpCode::LOAD_LOCAL_NUM:case OpCode::STORE_LOCAL_NUM:case OpCode::FOR_LOCAL_INIT:
        case OpCode::FOR_LOCAL_CHECK:
            if(op.a<0||static_cast<unsigned>(op.a)>=fn->numeric_local_count||
               (op.code==OpCode::FOR_LOCAL_CHECK&&!local_edge(i,op.b)))return false;
            break;
        case OpCode::FOR_LOCAL_INCR:
            if(op.a!=-1&&(op.a<0||static_cast<unsigned>(op.a)>=fn->numeric_local_count))return false;
            break;
        case OpCode::LOAD_LOCAL_STR:case OpCode::STORE_LOCAL_STR:
            if(op.a<0||static_cast<unsigned>(op.a)>=fn->string_local_count)return false;
            break;
        case OpCode::INPUT_LOCAL:
            if((op.b!=0&&op.b!=1)||op.a<0||
               static_cast<unsigned>(op.a)>=(op.b?fn->string_local_count:fn->numeric_local_count))return false;
            break;
        case OpCode::LOCAL_GRAY_PSET_MUL_INT:
            if(!valid_local_gray_span(p,i,fn->numeric_local_count,fn->end_pc))return false;
            break;
        case OpCode::LOCAL_NUM_FUSED:
            if(!valid_local_numeric(op,fn->numeric_local_count,p.number_count)||
               i+op.s>static_cast<std::size_t>(fn->end_pc))return false;
            break;
        default:break;
        }
    }
    return true;
}

CompiledProgram::~CompiledProgram() { reset(); }
bool CompiledProgram::reserve_lines(std::size_t count) {
    if(count<=kMaxLineMap) return true;
    extra_lines=static_cast<LinePc*>(std::calloc(count-kMaxLineMap,sizeof(LinePc)));
    return extra_lines!=nullptr;
}

void CompiledProgram::reset() {
    std::free(data_items);data_items=nullptr;data_count=data_capacity=0;
    for(std::size_t i=0;i<function_count;++i)std::free(functions[i].locals);
    std::free(functions);functions=nullptr;function_count=0;
    std::free(source_rows);source_rows=nullptr;source_row_count=0;
    source_mode=ProgramSourceMode::ClassicNumbered;
    std::free(extra_lines); extra_lines=nullptr;
    code_count = 0;
    number_count = 0;
    string_used = 1;
    string_pool[0] = '\0';
    symbol_count = 0;
    line_count = 0;
}

bool CompiledProgram::reserve_data(std::size_t count) {
    if(count>kMaxDataItems||count<data_count)return false;
    if(count==data_capacity)return true;
    if(!count) {std::free(data_items);data_items=nullptr;data_capacity=0;return true;}
    void* memory=std::realloc(data_items,count*sizeof(DataItem));
    if(!memory)return false;
    data_items=static_cast<DataItem*>(memory);data_capacity=count;return true;
}
bool CompiledProgram::append_data(DataItem item) {
    if(data_count==kMaxDataItems)return false;
    if(data_count==data_capacity&&!reserve_data(data_capacity?data_capacity*2:4))return false;
    data_items[data_count++]=item;return true;
}
bool CompiledProgram::compact_data() { return reserve_data(data_count); }

bool CompiledProgram::reserve_source_rows(std::size_t count) {
    if(count==0)return true;
    source_rows=static_cast<SourceRowPc*>(std::calloc(count,sizeof(SourceRowPc)));
    return source_rows!=nullptr;
}
int CompiledProgram::find_function(const char* name,bool base_name) const {
    const auto stem=[](const char* s) {auto n=std::strlen(s);return n&&s[n-1]=='$'?n-1:n;};
    for(std::size_t i=0;i<function_count;++i)
        if(base_name ? (stem(name)==stem(functions[i].name)&&!std::strncmp(name,functions[i].name,stem(name)))
                     : !std::strcmp(name,functions[i].name))return static_cast<int>(i);
    return -1;
}
int CompiledProgram::add_function(const char* name) {
    if(function_count>=kMaxUserFunctions)return -1;
    void* memory=std::realloc(functions,(function_count+1)*sizeof(FunctionInfo));
    if(!memory)return -1;
    functions=static_cast<FunctionInfo*>(memory);
    auto& fn=functions[function_count];fn=FunctionInfo{};
    std::strcpy(fn.name,name);const auto n=std::strlen(name);
    fn.returns_string=n&&name[n-1]=='$';
    return static_cast<int>(function_count++);
}
int CompiledProgram::add_local(std::size_t function,const char* name) {
    auto& fn=functions[function];
    for(std::size_t i=0;i<fn.local_count;++i)
        if(!std::strcmp(fn.locals[i].symbol.name,name))return static_cast<int>(i);
    if(fn.local_count>=kMaxFunctionLocals)return -1;
    void* memory=std::realloc(fn.locals,(fn.local_count+1)*sizeof(LocalSymbol));
    if(!memory)return -1;
    fn.locals=static_cast<LocalSymbol*>(memory);
    auto& local=fn.locals[fn.local_count];local=LocalSymbol{};
    std::strcpy(local.symbol.name,name);const auto n=std::strlen(name);
    local.symbol.is_string=n&&name[n-1]=='$';
    local.slot=local.symbol.is_string?fn.string_local_count++:fn.numeric_local_count++;
    return static_cast<int>(fn.local_count++);
}
bool CompiledProgram::emit(const Op& op) {
    if (code_count >= kMaxOps) return false;
    code[code_count++] = op;
    return true;
}

std::int32_t CompiledProgram::intern_number(BasicNumber value) {
    // Deduplicate by representation, not by floating-point comparison. This
    // preserves signed zero and avoids surprising NaN behaviour if such a
    // value is introduced by a future parser extension.
    for (std::size_t i = 0; i < number_count; ++i) {
        if (std::memcmp(&number_pool[i], &value, sizeof(value)) == 0) {
            return static_cast<std::int32_t>(i);
        }
    }

    if (number_count >= kNumberPoolSize) return -1;
    number_pool[number_count] = value;
    return static_cast<std::int32_t>(number_count++);
}

std::uint16_t CompiledProgram::intern_string(const char* text) {
    return text ? intern_string(text, std::strlen(text)) : 0;
}

std::uint16_t CompiledProgram::intern_string(
    const char* text,
    std::size_t length
) {
    if (!text) return 0;

    const std::size_t bytes = length + 1;
    if (string_used + bytes > kStringPoolSize || string_used > 0xffffu) {
        return 0xffffu;
    }

    const auto offset = static_cast<std::uint16_t>(string_used);
    std::memcpy(string_pool + string_used, text, length);
    string_pool[string_used + length] = '\0';
    string_used += bytes;
    return offset;
}

int CompiledProgram::find_or_add_symbol(const char* name) {
    if (!name || !*name) return -1;

    char normalized[kSymbolNameLength] = {};
    std::size_t n = 0;

    while (name[n] && n + 1 < sizeof(normalized)) {
        char c = name[n];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        normalized[n] = c;
        ++n;
    }

    for (std::size_t i = 0; i < symbol_count; ++i) {
        if (std::strcmp(symbols[i].name, normalized) == 0) {
            return static_cast<int>(i);
        }
    }

    if (symbol_count >= kMaxSymbols) return -1;

    Symbol& sym = symbols[symbol_count];
    std::strncpy(sym.name, normalized, kSymbolNameLength - 1);
    const std::size_t len = std::strlen(sym.name);
    sym.is_string = len > 0 && sym.name[len - 1] == '$';

    return static_cast<int>(symbol_count++);
}

int CompiledProgram::find_pc_for_line(std::int32_t line) const {
    if(source_mode==ProgramSourceMode::Structured)return -1;
    for (std::size_t i = 0; i < line_count; ++i) {
        if (line_at(i).line == line) return line_at(i).pc;
    }
    return -1;
}

} // namespace rmb
