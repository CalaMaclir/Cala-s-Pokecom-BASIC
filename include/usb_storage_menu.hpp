#pragma once

#include <cstddef>

namespace rmb::usb_storage_menu {

enum class State { Off, Active, Transition, Unavailable };

struct Model {
    const char* items[3];
    std::size_t count;
    std::size_t default_selection;
};

constexpr Model model(State state) {
    switch (state) {
    case State::Off:
        return {{"Enable USB Storage", "Back", nullptr}, 2, 0};
    case State::Active:
        return {{"Return SD to CPB", "Force Disconnect", "Back"}, 3, 0};
    case State::Unavailable:
        return {{"Return SD to CPB", "Back", nullptr}, 2, 0};
    case State::Transition:
    default:
        return {{"Back", nullptr, nullptr}, 1, 0};
    }
}

constexpr Model force_confirmation() {
    return {{"Cancel", "Force Disconnect", nullptr}, 2, 0};
}

} // namespace rmb::usb_storage_menu
