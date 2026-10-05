#pragma once

namespace rmb::input_hotkeys {
// Local key events, outside printable ASCII. System values stay compatible.
inline constexpr int Screenshot = 0xe2;
inline constexpr int Sleep = 0xe3;
inline constexpr int Editor = 0xe4;
inline constexpr int Run = 0xe5;
inline constexpr int ControlCenter = 0xe6;
inline constexpr int Match = 0xe7;
inline constexpr int Outdent = 0xe8;
inline constexpr int ShiftTab = 0xe9;

inline int alt_letter(int letter) {
    if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
    switch (letter) {
    case 's': return Screenshot;
    case 'p': return Sleep;
    case 'e': return Editor;
    case 'r': return Run;
    case 'c': return ControlCenter;
    case 'm': return Match;
    case 'u': return Outdent;
    default: return -1;
    }
}
inline bool prompt_action(int key) {
    return key == Editor || key == Run || key == ControlCenter;
}
inline bool workflow_event(int key) {
    return key >= Editor && key <= ShiftTab;
}
} // namespace rmb::input_hotkeys
