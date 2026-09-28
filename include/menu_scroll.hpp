#pragma once

namespace rmb::menu_scroll {

struct State {
    int selected = 0;
    int offset = 0;
};

enum class Move {
    Up,
    Down,
    PageUp,
    PageDown
};

// PicoCalc's keyboard firmware translates Shift+Up/Down into the dedicated
// Page Up/Down key codes. Bluetooth keyboards may instead expose the Shift
// modifier alongside a normal arrow key, so accept both forms.
inline bool decode_move_key(int key, bool shift, Move& direction) {
    constexpr int key_up = 0xb5;
    constexpr int key_down = 0xb6;
    constexpr int key_page_up = 0xd6;
    constexpr int key_page_down = 0xd7;

    if (key == key_page_up || (key == key_up && shift)) {
        direction = Move::PageUp;
        return true;
    }
    if (key == key_page_down || (key == key_down && shift)) {
        direction = Move::PageDown;
        return true;
    }
    if (key == key_up) {
        direction = Move::Up;
        return true;
    }
    if (key == key_down) {
        direction = Move::Down;
        return true;
    }
    return false;
}

inline void normalize(State& state, int count, int visible) {
    if (count <= 0 || visible <= 0) {
        state.selected = 0;
        state.offset = 0;
        return;
    }

    if (state.selected < 0) state.selected = 0;
    if (state.selected >= count) state.selected = count - 1;

    const int max_offset = count > visible ? count - visible : 0;
    if (state.offset < 0) state.offset = 0;
    if (state.offset > max_offset) state.offset = max_offset;

    if (state.selected < state.offset) {
        state.offset = state.selected;
    } else if (state.selected >= state.offset + visible) {
        state.offset = state.selected - visible + 1;
    }

    if (state.offset > max_offset) state.offset = max_offset;
}

inline bool move(State& state, Move direction, int count, int visible) {
    if (count <= 0 || visible <= 0) {
        normalize(state, count, visible);
        return false;
    }

    const int before = state.selected;
    switch (direction) {
    case Move::Up:
        if (state.selected > 0) --state.selected;
        break;
    case Move::Down:
        if (state.selected + 1 < count) ++state.selected;
        break;
    case Move::PageUp:
        state.selected -= visible;
        if (state.selected < 0) state.selected = 0;
        break;
    case Move::PageDown:
        state.selected += visible;
        if (state.selected >= count) state.selected = count - 1;
        break;
    }

    normalize(state, count, visible);
    return state.selected != before;
}

inline int first(const State& state) {
    return state.offset;
}

inline int last_exclusive(const State& state, int count, int visible) {
    int end = state.offset + visible;
    if (end > count) end = count;
    if (end < state.offset) end = state.offset;
    return end;
}

} // namespace rmb::menu_scroll
