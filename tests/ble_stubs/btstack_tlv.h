#pragma once
#include <cstdint>
struct btstack_tlv_t {
    int (*get_tag)(void*, std::uint32_t, std::uint8_t*, std::uint32_t);
    int (*store_tag)(void*, std::uint32_t, const std::uint8_t*, std::uint32_t);
    void (*delete_tag)(void*, std::uint32_t);
};
void btstack_tlv_get_instance(const btstack_tlv_t**, void**);
