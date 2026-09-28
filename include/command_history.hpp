#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace rmb {

class CommandHistory {
public:
    static constexpr std::size_t kCapacity = 24;
    static constexpr std::size_t kEntrySize = 224;

    void append(const char* command) {
        if (!command || !*command) {
            reset_navigation();
            return;
        }
        if (count_ != 0 && std::strcmp(entry(count_ - 1), command) == 0) {
            reset_navigation();
            return;
        }

        std::size_t slot = 0;
        if (count_ < kCapacity) {
            slot = physical(count_++);
        } else {
            slot = start_;
            start_ = (start_ + 1) % kCapacity;
        }
        std::snprintf(entries_[slot], kEntrySize, "%s", command);
        reset_navigation();
    }

    bool previous(const char* current, char* output, std::size_t capacity) {
        if (!output || capacity == 0 || count_ == 0) return false;
        if (!navigating_) {
            std::snprintf(draft_, sizeof(draft_), "%s", current ? current : "");
            position_ = count_;
            navigating_ = true;
        }
        if (position_ == 0) return false;
        --position_;
        copy(entry(position_), output, capacity);
        return true;
    }

    bool next(char* output, std::size_t capacity) {
        if (!output || capacity == 0 || !navigating_) return false;
        if (position_ + 1 < count_) {
            ++position_;
            copy(entry(position_), output, capacity);
            return true;
        }
        if (position_ + 1 == count_) {
            position_ = count_;
            navigating_ = false;
            copy(draft_, output, capacity);
            return true;
        }
        return false;
    }

    void reset_navigation() {
        navigating_ = false;
        position_ = count_;
        draft_[0] = '\0';
    }

    std::size_t size() const { return count_; }
    const char* at(std::size_t index) const {
        return index < count_ ? entry(index) : "";
    }

private:
    static void copy(const char* source, char* output, std::size_t capacity) {
        std::snprintf(output, capacity, "%s", source ? source : "");
    }
    std::size_t physical(std::size_t logical) const {
        return (start_ + logical) % kCapacity;
    }
    const char* entry(std::size_t logical) const {
        return entries_[physical(logical)];
    }

    char entries_[kCapacity][kEntrySize] = {};
    char draft_[kEntrySize] = {};
    std::size_t start_ = 0;
    std::size_t count_ = 0;
    std::size_t position_ = 0;
    bool navigating_ = false;
};

} // namespace rmb
