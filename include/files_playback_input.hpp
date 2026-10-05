#pragma once
#include <bitset>
namespace rmb::files_input {
enum class Result { Unhandled, Stop, TogglePause, Consumed };
// A handled press and its repeats belong to one layer, even after Stop.
// An independent press (repeat=false) can immediately perform the next action.
class PlaybackInput {
    std::bitset<256> consumed_keys_;
public:
    Result handle(int key, bool repeat, bool preview_active, bool requires_storage = false) {
        if (key < 0 || key > 255) return Result::Unhandled;
        const auto identity = static_cast<unsigned>(key >= 'A' && key <= 'Z' ? key-'A'+'a' : key);
        if (repeat && consumed_keys_.test(identity)) return Result::Consumed;
        if (!repeat) consumed_keys_.reset(identity);
        if (!preview_active) return Result::Unhandled;
        Result result = Result::Unhandled;
        switch (key) {
        case ' ': result = Result::TogglePause; break;
        case 0xb1: case 0x1b: case 8: case 127: case 3:
        case 'p': case 'P': case 0x0a: case 0x0d:
        case 0xb4: case 0xb7: case 0xd2: case 0xd4:
        case 'r': case 'R': case 'e': case 'E': case 'n': case 'N':
        case 'm': case 'M': case 'f': case 'F':
            result = Result::Stop; break;
        default:
            // F10 uses 0x90 in the MCU, HID and terminal decoders.
            if ((key >= 0x81 && key <= 0x89) || key == 0x90) result = Result::Stop;
            break;
        }
        if (requires_storage && result == Result::Unhandled) result = Result::Stop;
        if (result != Result::Unhandled) consumed_keys_.set(identity);
        return result;
    }
};
}
