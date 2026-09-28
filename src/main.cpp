#include "platform.hpp"
#include "psram.hpp"
#include "repl.hpp"

int main() {
    rmb::platform::init();
    // Optional carrier PSRAM must never block normal SRAM-only startup.
    (void)rmb::psram::init();

    // Keep the program store in static storage (BSS), not on the small
    // embedded runtime stack.
    static rmb::Repl repl;
    repl.run();

    return 0;
}
