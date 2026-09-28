#include "menu_scroll.hpp"

#include <cassert>

int main() {
    using namespace rmb::menu_scroll;

    Move decoded = Move::Down;
    assert(decode_move_key(0xb5, false, decoded));
    assert(decoded == Move::Up);
    assert(decode_move_key(0xb6, true, decoded));
    assert(decoded == Move::PageDown);
    assert(decode_move_key(0xd6, false, decoded));
    assert(decoded == Move::PageUp);
    assert(decode_move_key(0xd7, false, decoded));
    assert(decoded == Move::PageDown);
    assert(!decode_move_key('x', false, decoded));

    State s;
    normalize(s, 48, 27);
    assert(s.selected == 0);
    assert(s.offset == 0);

    for (int i = 0; i < 30; ++i) move(s, Move::Down, 48, 27);
    assert(s.selected == 30);
    assert(s.offset == 4);

    for (int i = 0; i < 17; ++i) move(s, Move::Down, 48, 27);
    assert(s.selected == 47);
    assert(s.offset == 21);

    for (int i = 0; i < 28; ++i) move(s, Move::Up, 48, 27);
    assert(s.selected == 19);
    assert(s.offset == 19);

    normalize(s, 5, 27);
    assert(s.selected == 4);
    assert(s.offset == 0);

    s = State{};
    assert(move(s, Move::PageDown, 48, 10));
    assert(s.selected == 10);
    assert(s.offset == 1);
    assert(move(s, Move::PageDown, 48, 10));
    assert(s.selected == 20);
    assert(s.offset == 11);
    assert(move(s, Move::PageUp, 48, 10));
    assert(s.selected == 10);
    assert(s.offset == 10);
    assert(move(s, Move::PageUp, 48, 10));
    assert(s.selected == 0);
    assert(s.offset == 0);
    assert(!move(s, Move::PageUp, 48, 10));

    s.selected = 45;
    s.offset = 38;
    assert(move(s, Move::PageDown, 48, 10));
    assert(s.selected == 47);
    assert(s.offset == 38);

    s.selected = 3;
    s.offset = 2;
    assert(move(s, Move::PageDown, 5, 27));
    assert(s.selected == 4);
    assert(s.offset == 0);
    assert(!move(s, Move::PageDown, 5, 27));

    normalize(s, 0, 27);
    assert(s.selected == 0);
    assert(s.offset == 0);
    assert(!move(s, Move::PageDown, 0, 27));
    assert(!move(s, Move::PageUp, 10, 0));
    return 0;
}
