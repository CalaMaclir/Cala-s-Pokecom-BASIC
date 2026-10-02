#pragma once
#include "mock_sdk.hpp"

constexpr int DMA_SIZE_8 = 0;
constexpr std::uint32_t DMA_CH0_TRANS_COUNT_COUNT_BITS = 0x0fffffffu;
constexpr std::uint32_t mock_dma_self_trigger_mode = 0x10000000u;

struct dma_channel_config {};
struct dma_channel_hw_t { volatile std::uint32_t transfer_count = 0; };
struct dma_hw_t { volatile std::uint32_t ints1 = 0; };

extern bool mock_dma_claim_available, mock_dma_claimed, mock_dma_active;
extern bool mock_dma_irq1_enabled, mock_dma_self_trigger;
extern std::uint8_t* mock_dma_write_base;
extern std::uint32_t mock_dma_write_index, mock_dma_reload_count;
extern dma_channel_hw_t mock_dma_channel_hw;
extern dma_hw_t mock_dma_hw;
#define dma_hw (&mock_dma_hw)

inline int dma_claim_unused_channel(bool) {
    if (!mock_dma_claim_available || mock_dma_claimed) return -1;
    mock_dma_claimed = true;
    return 0;
}
inline dma_channel_config dma_channel_get_default_config(int) { return {}; }
inline void channel_config_set_transfer_data_size(dma_channel_config*, int) {}
inline void channel_config_set_read_increment(dma_channel_config*, bool) {}
inline void channel_config_set_write_increment(dma_channel_config*, bool) {}
inline void channel_config_set_ring(dma_channel_config*, bool, unsigned) {}
inline void channel_config_set_dreq(dma_channel_config*, std::uint32_t) {}
inline std::uint32_t dma_encode_transfer_count_with_self_trigger(
    std::uint32_t count
) { return mock_dma_self_trigger_mode | count; }
inline void dma_channel_configure(
    int, const dma_channel_config*, volatile void* write_addr,
    const volatile void*, std::uint32_t encoded_count, bool trigger
) {
    mock_dma_write_base = static_cast<std::uint8_t*>(
        const_cast<void*>(static_cast<const volatile void*>(write_addr)));
    mock_dma_write_index = 0;
    mock_dma_reload_count = encoded_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
    mock_dma_self_trigger = (encoded_count & mock_dma_self_trigger_mode) != 0;
    mock_dma_channel_hw.transfer_count = encoded_count;
    mock_dma_active = trigger;
}
inline dma_channel_hw_t* dma_channel_hw_addr(int) {
    return &mock_dma_channel_hw;
}
inline bool dma_channel_get_irq1_status(int channel) {
    return (mock_dma_hw.ints1 & (1u << channel)) != 0;
}
inline void dma_channel_acknowledge_irq1(int channel) {
    mock_dma_hw.ints1 &= ~(1u << channel);
}
inline void dma_channel_set_irq1_enabled(int, bool enabled) {
    mock_dma_irq1_enabled = enabled;
}
inline void dma_channel_abort(int) { mock_dma_active = false; }
inline void dma_channel_unclaim(int) { mock_dma_claimed = false; }
