#pragma once
#include "mock_sdk.hpp"

constexpr int DMA_IRQ_1 = 1;
constexpr int PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY = 0;
extern void (*mock_dma_irq_handler)();

inline void irq_add_shared_handler(int, void (*handler)(), int) {
    mock_dma_irq_handler = handler;
}
inline void irq_remove_handler(int, void (*handler)()) {
    if (mock_dma_irq_handler == handler) mock_dma_irq_handler = nullptr;
}
