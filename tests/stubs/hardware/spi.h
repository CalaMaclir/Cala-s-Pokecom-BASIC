#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
using uint = unsigned int;
struct spi_inst_t {};
inline spi_inst_t test_spi;
inline spi_inst_t* spi1 = &test_spi;
constexpr int SPI_MSB_FIRST = 0;
constexpr int SPI_CPOL_0 = 0;
constexpr int SPI_CPHA_0 = 0;
inline uint spi_init(spi_inst_t*, uint hz) { return hz; }
inline uint spi_set_baudrate(spi_inst_t*, uint hz) { return hz; }
inline void spi_set_format(spi_inst_t*, uint, int, int, int) {}
inline int spi_write_blocking(spi_inst_t*, const uint8_t*, std::size_t n) { return n; }
inline int spi_read_blocking(spi_inst_t*, uint8_t, uint8_t* dst, std::size_t n) {
    std::memset(dst, 0, n); return n;
}
