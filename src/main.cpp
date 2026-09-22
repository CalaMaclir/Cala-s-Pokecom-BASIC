#include "platform.hpp"
#include "repl.hpp"

int main() {
    rmb::platform::init();

    // Keep the program store in static storage (BSS), not on the small
    // embedded runtime stack.
    static rmb::Repl repl;
    repl.run();

    return 0;
}
