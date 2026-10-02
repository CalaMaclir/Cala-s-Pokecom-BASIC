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

inline bool test_spi_capture = false;
inline std::size_t test_spi_single_write_count = 0;
inline uint8_t test_spi_single_writes[16] = {};
inline void test_spi_begin_capture() {
    test_spi_single_write_count = 0;
    test_spi_capture = true;
}
inline void test_spi_end_capture() { test_spi_capture = false; }

inline int spi_write_blocking(
    spi_inst_t*,
    const uint8_t* data,
    std::size_t n
) {
    if (test_spi_capture && n == 1 &&
        test_spi_single_write_count <
            sizeof(test_spi_single_writes) / sizeof(test_spi_single_writes[0])) {
        test_spi_single_writes[test_spi_single_write_count++] = data[0];
    }
    return static_cast<int>(n);
}
inline int spi_read_blocking(spi_inst_t*, uint8_t, uint8_t* dst, std::size_t n) {
    std::memset(dst, 0, n); return n;
}
