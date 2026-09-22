#include "usb_storage_menu.hpp"

#include <cassert>
#include <cstring>

namespace {
std::size_t count_item(const rmb::usb_storage_menu::Model& model, const char* text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < model.count; ++i)
        if (std::strcmp(model.items[i], text) == 0) ++count;
    return count;
}
}

int main() {
    using namespace rmb::usb_storage_menu;
    const auto off = model(State::Off);
    assert(off.count == 2 && count_item(off, "Back") == 1);
    assert(std::strcmp(off.items[0], "Enable USB Storage") == 0);

    const auto active = model(State::Active);
    assert(active.count == 3 && count_item(active, "Back") == 1);
    assert(std::strcmp(active.items[0], "Return SD to CPB") == 0);
    assert(std::strcmp(active.items[1], "Force Disconnect") == 0);

    const auto confirm = force_confirmation();
    assert(confirm.count == 2 && confirm.default_selection == 0);
    assert(std::strcmp(confirm.items[0], "Cancel") == 0);
    assert(std::strcmp(confirm.items[1], "Force Disconnect") == 0);
}
