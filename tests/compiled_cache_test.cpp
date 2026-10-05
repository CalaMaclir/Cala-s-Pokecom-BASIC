#include "compiled_cache.hpp"
#include "psram.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    rmb::CompiledProgram source;
    source.code[source.code_count++] = {
        rmb::OpCode::PUSH_NUM, 0, 0, 0, 0
    };
    source.code[source.code_count++] = {
        rmb::OpCode::HALT, 0, 0, 0, 0
    };
    source.number_pool[source.number_count++] = 42.0f;
    std::memcpy(source.string_pool, "\0HELLO\0", 7);
    source.string_used = 7;
    std::snprintf(source.symbols[0].name, rmb::kSymbolNameLength, "A");
    source.symbols[0].is_string = false;
    source.symbol_count = 1;

    constexpr std::size_t kLines = 300;
    assert(source.reserve_lines(kLines));
    source.line_count = kLines;
    for (std::size_t i = 0; i < kLines; ++i) {
        source.line_at(i).line = static_cast<std::int32_t>((i + 1) * 10);
        source.line_at(i).pc = static_cast<std::int32_t>(i % 2);
    }

    rmb::CompiledProgramCache cache;
    rmb::CompiledProgram restored;

    assert(!cache.restore(7, restored));
    assert(cache.misses() == 1);

    assert(cache.store(7, source));
    assert(cache.valid());
    assert(cache.allocated_bytes() > 0);
    assert(rmb::psram::allocation(
        rmb::psram::Client::CompiledCache).active);

    assert(cache.restore(7, restored));
    assert(cache.hits() == 1);
    assert(restored.code_count == source.code_count);
    assert(restored.number_count == source.number_count);
    assert(restored.string_used == source.string_used);
    assert(restored.symbol_count == source.symbol_count);
    assert(restored.line_count == source.line_count);
    assert(!std::memcmp(
        restored.code, source.code,
        source.code_count * sizeof(rmb::Op)));
    assert(!std::memcmp(
        restored.number_pool, source.number_pool,
        source.number_count * sizeof(rmb::BasicNumber)));
    assert(!std::memcmp(
        restored.string_pool, source.string_pool,
        source.string_used));
    assert(!std::memcmp(
        restored.symbols, source.symbols,
        source.symbol_count * sizeof(rmb::Symbol)));
    for (std::size_t i = 0; i < kLines; ++i) {
        assert(restored.line_at(i).line == source.line_at(i).line);
        assert(restored.line_at(i).pc == source.line_at(i).pc);
    }

    // A source revision mismatch can never reuse stale IL and releases the
    // stale cache region immediately.
    restored.reset();
    assert(!cache.restore(8, restored));
    assert(cache.misses() == 2);
    assert(!cache.valid());
    assert(!rmb::psram::allocation(
        rmb::psram::Client::CompiledCache).active);

    assert(cache.store(8, source));
    cache.invalidate();
    assert(!cache.valid());
    assert(!rmb::psram::allocation(
        rmb::psram::Client::CompiledCache).active);


    source.reset();source.source_mode=rmb::ProgramSourceMode::Structured;
    source.code_count=3;source.code[0].code=rmb::OpCode::JMP;source.code[0].a=2;
    source.code[1].code=rmb::OpCode::RETURN_USER;source.code[2].code=rmb::OpCode::HALT;
    assert(source.reserve_source_rows(2));source.source_row_count=2;
    source.source_rows[0]={1,0,-1};source.source_rows[1]={2,1,0};
    const int fn=source.add_function("F");assert(fn==0);
    source.functions[0].entry_pc=1;source.functions[0].end_pc=2;
    source.functions[0].parameter_count=1;source.functions[0].has_return=true;
    assert(source.add_local(0,"X")==0);assert(source.add_local(0,"S$")==1);
    assert(cache.store(10,source));
    assert(cache.restore(10,restored,rmb::ProgramSourceMode::Structured));
    assert(restored.source_mode==rmb::ProgramSourceMode::Structured&&restored.function_count==1);
    assert(restored.functions[0].local_count==2&&restored.functions[0].frame_bytes()==132);
    assert(restored.functions[0].locals[1].symbol.is_string);
    assert(restored.source_row_count==2&&restored.source_rows[1].row==2);
    assert(!cache.restore(10,restored,rmb::ProgramSourceMode::ClassicNumbered));
    assert(cache.store(11,source));
    const auto alloc=rmb::psram::allocation(rmb::psram::Client::CompiledCache);
    const std::uint16_t old_version=4;
    assert(rmb::psram::write(alloc.base_address+4,&old_version,sizeof(old_version)));
    assert(!cache.restore(11,restored,rmb::ProgramSourceMode::Structured));
    assert(cache.store(12,source));
    const auto region=rmb::psram::allocation(rmb::psram::Client::CompiledCache);
    std::uint16_t header_bytes=0;
    assert(rmb::psram::read(region.base_address+6,&header_bytes,sizeof(header_bytes)));
    const std::uint8_t corrupt_opcode=0xff;
    assert(rmb::psram::write(region.base_address+header_bytes,&corrupt_opcode,1));
    assert(!cache.restore(12,restored,rmb::ProgramSourceMode::Structured));
    // CRC-valid inputs with invalid PCs/slots must never become executable cache.
    source.code[0].a=9999;assert(!cache.store(13,source));source.code[0].a=2;
    source.code[1].code=rmb::OpCode::LOAD_LOCAL_NUM;source.code[1].a=128;
    assert(!cache.store(14,source));
    source.code[1].a=0;assert(cache.store(15,source));cache.invalidate();
    std::puts("CompiledProgram PSRAM cache: PASS");
}

