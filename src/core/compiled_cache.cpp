#include "compiled_cache.hpp"

#include "psram.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rmb {
namespace {

constexpr std::uint32_t kMagic = 0x43504348u; // "CPCH"
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kMaximumCachedLines = 4096;

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
        header.line_count <= kMaximumCachedLines;
}

std::size_t payload_bytes(const CacheHeader& header) {
    return static_cast<std::size_t>(header.code_count) * sizeof(Op) +
        static_cast<std::size_t>(header.number_count) * sizeof(BasicNumber) +
        static_cast<std::size_t>(header.string_used) +
        static_cast<std::size_t>(header.symbol_count) * sizeof(Symbol) +
        static_cast<std::size_t>(header.line_count) * sizeof(LinePc);
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
    return crc ^ 0xffffffffu;
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

    if (!valid_counts(header)) return false;
    if (program.line_count > kMaxLineMap && !program.extra_lines)
        return false;

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
    CompiledProgram& output
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
        header.source_revision != source_revision) {
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
    if (!output.reserve_lines(header.line_count)) {
        ++misses_;
        return false;
    }

    output.code_count = header.code_count;
    output.number_count = header.number_count;
    output.string_used = header.string_used;
    output.symbol_count = header.symbol_count;
    output.line_count = header.line_count;

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

    if (!ok || payload_crc(output) != header.payload_crc32) {
        output.reset();
        release_region();
        ++misses_;
        return false;
    }

    ++hits_;
    return true;
}

} // namespace rmb
