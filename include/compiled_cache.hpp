#pragma once

#include <cstdint>

#include "il.hpp"

namespace rmb {

class CompiledProgramCache {
public:
    bool restore(std::uint64_t source_revision, CompiledProgram& output,
                 ProgramSourceMode mode=ProgramSourceMode::ClassicNumbered);
    bool store(std::uint64_t source_revision, const CompiledProgram& program);
    void invalidate();

    bool valid() const { return valid_; }
    std::uint64_t revision() const { return revision_; }
    std::uint32_t allocated_bytes() const { return allocated_bytes_; }
    std::uint32_t hits() const { return hits_; }
    std::uint32_t misses() const { return misses_; }

private:
    void release_region();

    std::uint32_t base_address_ = 0;
    std::uint32_t allocated_bytes_ = 0;
    std::uint64_t revision_ = 0;
    std::uint32_t hits_ = 0;
    std::uint32_t misses_ = 0;
    bool valid_ = false;
};

} // namespace rmb
