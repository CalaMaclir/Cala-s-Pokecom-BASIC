#pragma once
namespace rmb {
inline bool show_restored_editing_session(bool recovered, bool dirty) {
    return recovered && dirty;
}
} // namespace rmb
