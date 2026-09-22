#pragma once
#include <cstddef>
#include <cstdint>

struct blockdevice_t {
    int (*read)(blockdevice_t*, void*, std::uint64_t, std::size_t);
    int (*program)(blockdevice_t*, const void*, std::uint64_t, std::size_t);
    int (*sync)(blockdevice_t*);
    std::uint64_t (*size)(blockdevice_t*);
};
struct spi_inst_t {};
extern spi_inst_t* spi0;
blockdevice_t* blockdevice_sd_create(
    spi_inst_t*, unsigned, unsigned, unsigned, unsigned,
    std::uint32_t, bool);
