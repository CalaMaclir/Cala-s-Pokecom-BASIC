#pragma once
#include "mock_sdk.hpp"

struct uart_hw_t { volatile std::uint32_t dr = 0; };
extern uart_hw_t mock_uart_hw;
extern std::uint32_t mock_uart_baud;

inline uart_hw_t* uart_get_hw(int) { return &mock_uart_hw; }
inline std::uint32_t uart_get_dreq(int, bool) { return 0; }
inline std::uint32_t uart_set_baudrate(int, std::uint32_t baud) {
    mock_uart_baud = baud;
    return baud;
}
inline void uart_set_irq_enables(int, bool, bool) {}
