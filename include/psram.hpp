#pragma once

#include <cstddef>
#include <cstdint>

namespace rmb::psram {

constexpr std::size_t kMaximumProbeAttempts = 8;

enum class Client : std::uint8_t {
    None,
    EditorHistory,
    Diagnostic
};

struct ProbeAttempt {
    std::uint32_t bus_clock_hz = 0;
    bool falling_edge_fudge = false;
    bool fast_read = false;
    std::uint8_t manufacturer_id = 0;
    std::uint8_t known_good_die = 0;
    std::uint8_t electronic_id = 0;
    std::uint8_t readback[4] = {};
    bool passed = false;
};

struct DeviceInfo {
    bool initialized = false;
    bool available = false;
    std::uint32_t size_bytes = 0;
    std::uint32_t bus_clock_hz = 0;
    std::uint8_t manufacturer_id = 0;
    std::uint8_t known_good_die = 0;
    std::uint8_t electronic_id = 0;
    int pio_state_machine = -1;
    ProbeAttempt probe_attempts[kMaximumProbeAttempts] = {};
    std::size_t probe_attempt_count = 0;
    int selected_probe_attempt = -1;
    const char* error = "NOT INITIALIZED";
};

struct BenchmarkSample {
    std::uint32_t bytes = 0;
    std::uint32_t sequential_write_bytes_per_second = 0;
    std::uint32_t sequential_read_bytes_per_second = 0;
    std::uint32_t random_write_operations_per_second = 0;
    std::uint32_t random_read_operations_per_second = 0;
};

struct DiagnosticResult {
    bool ran = false;
    bool passed = false;
    std::uint32_t tested_bytes = 0;
    std::uint32_t error_count = 0;
    std::uint32_t first_failure = 0;
    std::uint32_t write_microseconds = 0;
    std::uint32_t read_microseconds = 0;
    std::uint32_t write_bytes_per_second = 0;
    std::uint32_t read_bytes_per_second = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint32_t actual_crc32 = 0;
    BenchmarkSample benchmarks[5] = {};
    std::size_t benchmark_count = 0;
    const char* failed_stage = "NOT RUN";
};

using ProgressCallback = void (*)(
    const char* stage,
    std::uint32_t completed,
    std::uint32_t total,
    void* context
);

bool init();
bool reprobe();
void clock_changed();
const DeviceInfo& info();
bool busy();
bool claim(
    Client client,
    std::uint32_t requested_bytes,
    std::uint32_t& base_address,
    std::uint32_t& allocated_bytes
);
void release(Client client);
Client owner();
std::uint32_t used_bytes();
bool read(std::uint32_t address, void* destination, std::size_t length);
bool write(std::uint32_t address, const void* source, std::size_t length);
bool fill(
    std::uint32_t address,
    std::uint8_t value,
    std::size_t length
);
bool run_diagnostic(
    std::uint32_t requested_bytes,
    DiagnosticResult& result,
    ProgressCallback progress = nullptr,
    void* context = nullptr
);

} // namespace rmb::psram
