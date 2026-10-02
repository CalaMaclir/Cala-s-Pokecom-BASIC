#include "local_numeric_fusion.hpp"
#include "compiled_cache.hpp"

#include "psram.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdlib>
#include <cstring>

namespace rmb {
namespace {

constexpr std::uint32_t kMagic = 0x43504348u; // "CPCH"
constexpr std::uint16_t kFormatVersion = 3;
constexpr std::size_t kMaximumCachedLines = 4096;

constexpr std::size_t kFunctionHeaderBytes=offsetof(FunctionInfo,locals);
struct CacheHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t format_version = kFormatVersion;
    std::uint16_t header_bytes = 0;
    std::uint64_t source_revision = 0;
    std::uint32_t code_count = 0;
    std::uint32_t number_count = 0;
    std::uint32_t string_used = 0;
    std::uint32_t symbol_count = 0;
    std::uint32_t line_count = 0;
    ProgramSourceMode source_mode = ProgramSourceMode::ClassicNumbered;
    std::uint32_t source_row_count = 0, function_count = 0, local_count = 0;
    std::uint32_t payload_crc32 = 0;
};

std::uint32_t crc32_update(
    std::uint32_t crc,
    const void* data,
    std::size_t length
) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (length-- != 0) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u &
                (0u - (crc & 1u)));
        }
    }
    return crc;
}

bool valid_counts(const CacheHeader& header) {
    return header.magic == kMagic &&
        header.format_version == kFormatVersion &&
        header.header_bytes == sizeof(CacheHeader) &&
        header.code_count <= kMaxOps &&
        header.number_count <= kNumberPoolSize &&
        header.string_used >= 1 &&
        header.string_used <= kStringPoolSize &&
        header.symbol_count <= kMaxSymbols &&
        header.line_count <= kMaximumCachedLines &&
        static_cast<unsigned>(header.source_mode)<=1 &&
        header.source_row_count<=1024 && header.function_count<=kMaxUserFunctions &&
        header.local_count<=kMaxUserFunctions*kMaxFunctionLocals;
}

std::size_t payload_bytes(const CacheHeader& header) {
    return static_cast<std::size_t>(header.code_count) * sizeof(Op) +
        static_cast<std::size_t>(header.number_count) * sizeof(BasicNumber) +
        static_cast<std::size_t>(header.string_used) +
        static_cast<std::size_t>(header.symbol_count) * sizeof(Symbol) +
        static_cast<std::size_t>(header.line_count) * sizeof(LinePc) +
        header.source_row_count*sizeof(SourceRowPc) + header.function_count*kFunctionHeaderBytes +
        header.local_count*sizeof(LocalSymbol);
}

std::uint32_t payload_crc(const CompiledProgram& program) {
    std::uint32_t crc = 0xffffffffu;
    crc = crc32_update(
        crc, program.code, program.code_count * sizeof(Op));
    crc = crc32_update(
        crc, program.number_pool,
        program.number_count * sizeof(BasicNumber));
    crc = crc32_update(
        crc, program.string_pool, program.string_used);
    crc = crc32_update(
        crc, program.symbols, program.symbol_count * sizeof(Symbol));

    const std::size_t first =
        std::min(program.line_count, kMaxLineMap);
    crc = crc32_update(
        crc, program.lines, first * sizeof(LinePc));
    if (program.line_count > first) {
        crc = crc32_update(
            crc,
            program.extra_lines,
            (program.line_count - first) * sizeof(LinePc));
    }
    crc=crc32_update(crc,program.source_rows,program.source_row_count*sizeof(SourceRowPc));
    for(std::size_t i=0;i<program.function_count;++i) {
        crc=crc32_update(crc,&program.functions[i],kFunctionHeaderBytes);
        crc=crc32_update(crc,program.functions[i].locals,program.functions[i].local_count*sizeof(LocalSymbol));
    }
    return crc ^ 0xffffffffu;
}

// Validate restored operands as well as CRC. A correct CRC is not a PC/slot proof.
bool valid_program(const CompiledProgram& p) {
    auto pc=[&](int n){return n>=0&&static_cast<std::size_t>(n)<p.code_count;};
    auto slot=[&](int n){return n>=0&&static_cast<std::size_t>(n)<p.symbol_count;};
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
        if(op.code>=OpCode::LOAD_LOCAL_NUM&&op.code<=OpCode::LOCAL_NUM_FUSED&&
           op.code!=OpCode::CALL_USER&&op.code!=OpCode::RETURN_USER&&op.code!=OpCode::FUNCTION_FALLTHROUGH) {
            for(std::size_t f=0;f<p.function_count;++f)
                if(static_cast<int>(i)>=p.functions[f].entry_pc&&static_cast<int>(i)<p.functions[f].end_pc){fn=&p.functions[f];break;}
            if(!fn)return false;
        }
        switch(op.code) {
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
            if(!pc(op.a))return false;
            break;
        case OpCode::FOR_CHECK:if(!slot(op.a)||!pc(op.b))return false;
            break;
        case OpCode::COMPLEX_ITER_OR_JZ:case OpCode::COMPLEX_ITER_SUMSQ_JZ:
            if(!pc(op.b&0xfff)||static_cast<unsigned>((op.b>>12)&0xfff)>=p.number_count)return false;
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
               (op.code==OpCode::FOR_LOCAL_CHECK&&!pc(op.b)))return false;
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
        case OpCode::LOCAL_NUM_FUSED:
            if(!valid_local_numeric(op,fn->numeric_local_count,p.number_count)||
               i+op.s>static_cast<std::size_t>(fn->end_pc))return false;
            break;
        default:break;
        }
    }
    return true;
}

bool write_bytes(
    std::uint32_t& address,
    const void* data,
    std::size_t bytes
) {
    if (bytes == 0) return true;
    if (!psram::write(address, data, bytes)) return false;
    address += static_cast<std::uint32_t>(bytes);
    return true;
}

bool read_bytes(
    std::uint32_t& address,
    void* data,
    std::size_t bytes
) {
    if (bytes == 0) return true;
    if (!psram::read(address, data, bytes)) return false;
    address += static_cast<std::uint32_t>(bytes);
    return true;
}

} // namespace

void CompiledProgramCache::release_region() {
    if (allocated_bytes_ != 0)
        psram::release(psram::Client::CompiledCache);
    base_address_ = 0;
    allocated_bytes_ = 0;
    revision_ = 0;
    valid_ = false;
}

void CompiledProgramCache::invalidate() {
    release_region();
}

bool CompiledProgramCache::store(
    std::uint64_t source_revision,
    const CompiledProgram& program
) {
    CacheHeader header;
    header.header_bytes = static_cast<std::uint16_t>(sizeof(CacheHeader));
    header.source_revision = source_revision;
    header.code_count = static_cast<std::uint32_t>(program.code_count);
    header.number_count = static_cast<std::uint32_t>(program.number_count);
    header.string_used = static_cast<std::uint32_t>(program.string_used);
    header.symbol_count = static_cast<std::uint32_t>(program.symbol_count);
    header.line_count = static_cast<std::uint32_t>(program.line_count);
    header.source_mode=program.source_mode;
    header.source_row_count=static_cast<std::uint32_t>(program.source_row_count);
    header.function_count=static_cast<std::uint32_t>(program.function_count);
    for(std::size_t i=0;i<program.function_count;++i)header.local_count+=program.functions[i].local_count;

    if (!valid_counts(header)) return false;
    if (program.line_count > kMaxLineMap && !program.extra_lines) return false;
    if ((program.source_row_count&&!program.source_rows)||(program.function_count&&!program.functions)) return false;
    if (!valid_program(program)) return false;

    header.payload_crc32 = payload_crc(program);
    const std::size_t payload = payload_bytes(header);
    if (payload >
        std::numeric_limits<std::uint32_t>::max() - sizeof(CacheHeader))
        return false;
    const std::uint32_t requested =
        static_cast<std::uint32_t>(sizeof(CacheHeader) + payload);

    release_region();
    if (!psram::init()) return false;

    std::uint32_t base = 0;
    std::uint32_t allocated = 0;
    if (!psram::claim(
            psram::Client::CompiledCache,
            requested,
            base,
            allocated) ||
        allocated < requested) {
        return false;
    }

    base_address_ = base;
    allocated_bytes_ = allocated;

    std::uint32_t address = base_address_;
    bool ok = write_bytes(address, &header, sizeof(header)) &&
        write_bytes(
            address, program.code,
            program.code_count * sizeof(Op)) &&
        write_bytes(
            address, program.number_pool,
            program.number_count * sizeof(BasicNumber)) &&
        write_bytes(
            address, program.string_pool,
            program.string_used) &&
        write_bytes(
            address, program.symbols,
            program.symbol_count * sizeof(Symbol));

    const std::size_t first =
        std::min(program.line_count, kMaxLineMap);
    ok = ok && write_bytes(
        address, program.lines, first * sizeof(LinePc));
    if (ok && program.line_count > first) {
        ok = write_bytes(
            address,
            program.extra_lines,
            (program.line_count - first) * sizeof(LinePc));
    }

    ok=ok&&write_bytes(address,program.source_rows,program.source_row_count*sizeof(SourceRowPc));
    for(std::size_t i=0;ok&&i<program.function_count;++i) {
        ok=write_bytes(address,&program.functions[i],kFunctionHeaderBytes)&&
           write_bytes(address,program.functions[i].locals,program.functions[i].local_count*sizeof(LocalSymbol));
    }
    if (!ok) {
        release_region();
        return false;
    }

    revision_ = source_revision;
    valid_ = true;
    return true;
}

bool CompiledProgramCache::restore(
    std::uint64_t source_revision,
    CompiledProgram& output,ProgramSourceMode mode
) {
    if (!valid_ || revision_ != source_revision) {
        if (valid_ && revision_ != source_revision)
            release_region();
        ++misses_;
        return false;
    }

    CacheHeader header;
    std::uint32_t address = base_address_;
    if (!read_bytes(address, &header, sizeof(header)) ||
        !valid_counts(header) ||
        header.source_revision != source_revision || header.source_mode!=mode) {
        release_region();
        ++misses_;
        return false;
    }

    const std::size_t required =
        sizeof(CacheHeader) + payload_bytes(header);
    if (required > allocated_bytes_) {
        release_region();
        ++misses_;
        return false;
    }

    output.reset();
    if (!output.reserve_lines(header.line_count) || !output.reserve_source_rows(header.source_row_count)) {
        output.reset();
        ++misses_;
        return false;
    }

    output.code_count = header.code_count;
    output.number_count = header.number_count;
    output.string_used = header.string_used;
    output.symbol_count = header.symbol_count;
    output.line_count = header.line_count;
    output.source_mode=header.source_mode;output.source_row_count=header.source_row_count;

    bool ok = read_bytes(
            address, output.code,
            output.code_count * sizeof(Op)) &&
        read_bytes(
            address, output.number_pool,
            output.number_count * sizeof(BasicNumber)) &&
        read_bytes(
            address, output.string_pool,
            output.string_used) &&
        read_bytes(
            address, output.symbols,
            output.symbol_count * sizeof(Symbol));

    const std::size_t first =
        std::min(output.line_count, kMaxLineMap);
    ok = ok && read_bytes(
        address, output.lines, first * sizeof(LinePc));
    if (ok && output.line_count > first) {
        ok = read_bytes(
            address,
            output.extra_lines,
            (output.line_count - first) * sizeof(LinePc));
    }

    ok=ok&&read_bytes(address,output.source_rows,output.source_row_count*sizeof(SourceRowPc));
    if(ok&&header.function_count) {
        output.functions=static_cast<FunctionInfo*>(std::calloc(header.function_count,sizeof(FunctionInfo)));
        ok=output.functions!=nullptr;
        if(ok)output.function_count=header.function_count;
    }
    std::size_t total_locals=0;
    for(std::size_t i=0;ok&&i<output.function_count;++i) {
        auto& fn=output.functions[i];
        ok=read_bytes(address,&fn,kFunctionHeaderBytes);
        ok=ok&&fn.local_count<=kMaxFunctionLocals&&fn.parameter_count<=kMaxFunctionParameters&&
            fn.parameter_count<=fn.local_count&&fn.numeric_local_count+fn.string_local_count==fn.local_count&&
            fn.entry_pc>=0&&fn.end_pc>fn.entry_pc&&static_cast<std::size_t>(fn.end_pc)<=output.code_count&&
            std::memchr(fn.name,0,sizeof(fn.name));
        total_locals+=fn.local_count;
        if(!ok||total_locals>header.local_count){ok=false;break;}
        if(fn.local_count) {
            fn.locals=static_cast<LocalSymbol*>(std::calloc(fn.local_count,sizeof(LocalSymbol)));
            ok=fn.locals&&read_bytes(address,fn.locals,fn.local_count*sizeof(LocalSymbol));
        }
    }
    ok=ok&&total_locals==header.local_count;
    if (!ok || payload_crc(output) != header.payload_crc32) {
        output.reset();
        release_region();
        ++misses_;
        return false;
    }

    // Inspect typed fields only after CRC, so random bool-byte corruption is a miss.
    for(std::size_t i=0;i<output.function_count;++i) {
        const auto& fn=output.functions[i];
        for(std::size_t j=0;j<fn.local_count;++j) {
            const auto& local=fn.locals[j];
            if(local.slot>=(local.symbol.is_string?fn.string_local_count:fn.numeric_local_count)||
               !std::memchr(local.symbol.name,0,kSymbolNameLength)) {
                output.reset();release_region();++misses_;return false;
            }
        }
    }
    if(!valid_program(output)){output.reset();release_region();++misses_;return false;}
    ++hits_;
    return true;
}

} // namespace rmb
