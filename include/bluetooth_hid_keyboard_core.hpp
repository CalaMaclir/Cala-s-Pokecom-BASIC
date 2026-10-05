#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::bluetooth_hid {

enum class KeyboardLayout : std::uint8_t {
    Us,
    Jis
};

inline const char* keyboard_layout_name(KeyboardLayout layout) {
    return layout == KeyboardLayout::Us ? "US" : "JIS";
}

class KeyboardCore {
public:
    static constexpr std::size_t kMaxKeys = 6;
    static constexpr std::size_t kQueueCapacity = 32;
    static constexpr std::uint32_t kRepeatDelayMs = 400;
    static constexpr std::uint32_t kRepeatIntervalMs = 50;

    void reset();
    void set_layout(KeyboardLayout layout) { layout_ = layout; }
    KeyboardLayout layout() const { return layout_; }

    void handle_report(
        std::uint8_t modifiers,
        const std::uint8_t* usages,
        std::size_t count,
        std::uint32_t now_ms
    );
    int read_key(std::uint32_t now_ms);

    bool caps_lock_enabled() const { return caps_lock_; }
    void set_caps_lock(bool enabled) { caps_lock_ = enabled; }
    bool shift_held() const;
    bool last_key_repeat() const { return last_key_repeat_; }
    std::uint32_t overflow_count() const { return overflow_count_; }

    static int translate_usage(
        KeyboardLayout layout,
        std::uint8_t usage,
        std::uint8_t modifiers,
        bool caps_lock
    );
    static bool repeatable(int code);

private:
    bool enqueue(int code, bool repeat = false);
    bool was_pressed(std::uint8_t usage) const;
    bool is_pressed(std::uint8_t usage) const;
    void service_repeat(std::uint32_t now_ms);

    int queue_[kQueueCapacity] = {};
    bool queue_repeat_[kQueueCapacity] = {};
    bool last_key_repeat_ = false;
    std::size_t queue_read_ = 0;
    std::size_t queue_write_ = 0;
    std::uint8_t last_keys_[kMaxKeys] = {};
    std::uint8_t key_count_ = 0;
    std::uint8_t modifiers_ = 0;
    std::uint8_t repeat_usage_ = 0;
    std::uint32_t repeat_due_ms_ = 0;
    std::uint32_t overflow_count_ = 0;
    KeyboardLayout layout_ = KeyboardLayout::Jis;
    bool caps_lock_ = false;
};

} // namespace rmb::bluetooth_hid
