#pragma once

#include <cstddef>
#include <cstdint>
#include "hardware/i2c.h"
#include "pico/error.h"
#include "pico/stdlib.h"

namespace rmb::picocalc::keyboard::detail {

// SDK 2.3.1's read waits for TX FIFO space without checking its deadline.
// Reserve enough slots for ALL read commands before entering that function.
// I2C1 has one foreground owner; no ISR/core submits commands to this bus.
inline int bounded_i2c_read(
    i2c_inst_t* bus, std::uint8_t address, std::uint8_t* data,
    std::size_t length, std::uint32_t timeout_us
) {
    if (length == 0 || length > 16) return PICO_ERROR_GENERIC;
    const auto deadline = make_timeout_time_us(timeout_us);
    while (i2c_get_write_available(bus) < length) {
        if (time_reached(deadline)) return PICO_ERROR_TIMEOUT;
        tight_loop_contents();
    }
    if (time_reached(deadline)) return PICO_ERROR_TIMEOUT;
    // Preserve a single overall deadline, rather than starting another timeout.
    return i2c_read_blocking_until(bus, address, data, length, false, deadline);
}

} // namespace rmb::picocalc::keyboard::detail
