#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace rmb::psram::detail {

struct Region {
    bool used = false;
    std::uint32_t base = 0;
    std::uint32_t size = 0;
};

template<std::size_t Slots>
class RegionAllocator {
public:
    explicit RegionAllocator(std::uint32_t capacity = 0)
        : capacity_(capacity) {}

    void reset(std::uint32_t capacity) {
        capacity_ = capacity;
        regions_ = {};
    }

    bool claim(
        std::size_t slot,
        std::uint32_t requested,
        std::uint32_t& base,
        std::uint32_t& allocated
    ) {
        base = 0;
        allocated = 0;
        if (slot >= Slots || requested == 0 || capacity_ == 0)
            return false;
        if (regions_[slot].used) return false;

        const std::uint32_t wanted = align_up(requested);
        std::uint32_t cursor = 0;
        while (cursor < capacity_) {
            std::uint32_t next = capacity_;
            bool blocked = false;
            for (const auto& region : regions_) {
                if (!region.used) continue;
                if (cursor >= region.base &&
                    cursor < region.base + region.size) {
                    cursor = region.base + region.size;
                    blocked = true;
                    break;
                }
                if (region.base > cursor)
                    next = std::min(next, region.base);
            }
            if (blocked) continue;
            if (wanted <= next - cursor) {
                regions_[slot] = {true, cursor, wanted};
                base = cursor;
                allocated = wanted;
                return true;
            }
            if (next >= capacity_) break;
            cursor = next;
        }
        return false;
    }

    void release(std::size_t slot) {
        if (slot < Slots) regions_[slot] = {};
    }

    bool active(std::size_t slot) const {
        return slot < Slots && regions_[slot].used;
    }

    Region region(std::size_t slot) const {
        return slot < Slots ? regions_[slot] : Region{};
    }

    std::uint32_t used_bytes() const {
        std::uint32_t total = 0;
        for (const auto& region : regions_)
            if (region.used) total += region.size;
        return total;
    }

    std::size_t active_count() const {
        std::size_t count = 0;
        for (const auto& region : regions_)
            if (region.used) ++count;
        return count;
    }

private:
    static constexpr std::uint32_t kAlignment = 16;

    static std::uint32_t align_up(std::uint32_t value) {
        return (value + kAlignment - 1u) & ~(kAlignment - 1u);
    }

    std::uint32_t capacity_ = 0;
    std::array<Region, Slots> regions_{};
};

} // namespace rmb::psram::detail
