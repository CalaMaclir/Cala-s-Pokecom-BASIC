#include "psram.hpp"
#include "psram_layout.hpp"
#include "psram_allocator.hpp"

#include <algorithm>
#include <cstring>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "psram_spi.pio.h"

namespace rmb::psram {
namespace {

constexpr uint kPinChipSelect = 20;
constexpr uint kPinClock = 21;
constexpr uint kPinMosi = 2;
constexpr uint kPinMiso = 3;
constexpr std::uint32_t kPicoCalcPsramBytes = 8u * 1024u * 1024u;
constexpr std::uint32_t kQuickTestBytes = 1024u * 1024u;
constexpr std::size_t kMaximumReadChunk = 31;
constexpr std::size_t kMaximumWriteChunk = 27;
constexpr std::uint32_t kReservedTailBytes = 16u;

struct DriverState {
    PIO pio = pio1;
    int state_machine = -1;
    uint program_offset = 0;
    const pio_program_t* program = nullptr;
    bool program_loaded = false;
    bool busy = false;
    bool fast_read = true;
    bool falling_edge_fudge = true;
    std::uint32_t target_clock_hz = 25000000u;
    detail::RegionAllocator<kAllocatableClientCount> allocations{};
    bool diagnostic_active = false;
    DeviceInfo info{};
};

DriverState state;

bool allocatable_client(Client client) {
    return client >= Client::EditorHistory && client <= Client::AudioCache;
}

std::size_t client_slot(Client client) {
    return static_cast<std::size_t>(client) - 1u;
}

void release_driver() {
    if (state.state_machine >= 0) {
        pio_sm_set_enabled(state.pio, state.state_machine, false);
        pio_sm_unclaim(state.pio, state.state_machine);
        state.state_machine = -1;
        state.info.pio_state_machine = -1;
    }
    if (state.program_loaded) {
        pio_remove_program(
            state.pio, state.program, state.program_offset);
        state.program_loaded = false;
        state.program = nullptr;
    }
    for (const uint pin : {kPinChipSelect, kPinClock, kPinMosi, kPinMiso}) {
        gpio_set_function(pin, GPIO_FUNC_NULL);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
}

float clock_divider() {
    const std::uint32_t system_hz = clock_get_hz(clk_sys);
    const float divider = static_cast<float>(system_hz) /
        static_cast<float>(2u * state.target_clock_hz);
    return divider < 1.0f ? 1.0f : divider;
}

void transfer(
    const std::uint8_t* source,
    std::size_t source_length,
    std::uint8_t* destination,
    std::size_t destination_length
) {
    std::size_t transmit_remaining = source_length;
    std::size_t receive_remaining = destination_length;
    io_rw_8* transmit = reinterpret_cast<io_rw_8*>(
        &state.pio->txf[state.state_machine]);
    while (transmit_remaining != 0) {
        if (!pio_sm_is_tx_fifo_full(state.pio, state.state_machine)) {
            *transmit = *source++;
            --transmit_remaining;
        }
    }
    const volatile std::uint8_t* receive = reinterpret_cast<const volatile std::uint8_t*>(
        &state.pio->rxf[state.state_machine]);
    while (receive_remaining != 0) {
        if (!pio_sm_is_rx_fifo_empty(state.pio, state.state_machine)) {
            *destination++ = *receive;
            --receive_remaining;
        }
    }
}

void command(std::uint8_t value) {
    const std::uint8_t transaction[] = {8, 0, value};
    transfer(transaction, sizeof(transaction), nullptr, 0);
}

bool read_raw(
    std::uint32_t address,
    void* destination,
    std::size_t length
) {
    auto* output = static_cast<std::uint8_t*>(destination);
    while (length != 0) {
        const std::size_t chunk = detail::chunk_length(
            length, kMaximumReadChunk);
        std::uint8_t transaction[7] = {
            static_cast<std::uint8_t>(state.fast_read ? 40 : 32),
            static_cast<std::uint8_t>(chunk * 8),
            static_cast<std::uint8_t>(state.fast_read ? 0x0b : 0x03),
            static_cast<std::uint8_t>(address >> 16),
            static_cast<std::uint8_t>(address >> 8),
            static_cast<std::uint8_t>(address),
            0
        };
        transfer(
            transaction, state.fast_read ? sizeof(transaction) : 6,
            output, chunk);
        address += static_cast<std::uint32_t>(chunk);
        output += chunk;
        length -= chunk;
    }
    return true;
}

bool write_raw(
    std::uint32_t address,
    const void* source,
    std::size_t length
) {
    const auto* input = static_cast<const std::uint8_t*>(source);
    while (length != 0) {
        const std::size_t chunk = detail::chunk_length(
            length, kMaximumWriteChunk);
        std::uint8_t transaction[2 + 4 + kMaximumWriteChunk] = {
            static_cast<std::uint8_t>((4 + chunk) * 8),
            0,
            0x02,
            static_cast<std::uint8_t>(address >> 16),
            static_cast<std::uint8_t>(address >> 8),
            static_cast<std::uint8_t>(address)
        };
        std::memcpy(transaction + 6, input, chunk);
        transfer(transaction, 6 + chunk, nullptr, 0);
        address += static_cast<std::uint32_t>(chunk);
        input += chunk;
        length -= chunk;
    }
    return true;
}

std::uint32_t size_from_electronic_id(std::uint8_t eid) {
    if (eid == 0x26) return 8u * 1024u * 1024u;
    switch (eid >> 5) {
    case 0: return 2u * 1024u * 1024u;
    case 1: return 4u * 1024u * 1024u;
    case 2:
    case 3: return 8u * 1024u * 1024u;
    case 4: return 16u * 1024u * 1024u;
    default: return 0;
    }
}

bool bounds_valid(std::uint32_t address, std::size_t length) {
    return detail::range_valid(state.info.size_bytes, address, length);
}

std::uint8_t address_pattern(std::uint32_t address) {
    std::uint32_t value = address + 0x9e3779b9u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return static_cast<std::uint8_t>(value ^ (value >> 8));
}

std::uint32_t crc32_update(
    std::uint32_t crc,
    const std::uint8_t* data,
    std::size_t length
) {
    while (length-- != 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u &
                (0u - (crc & 1u)));
        }
    }
    return crc;
}

void report_progress(
    ProgressCallback callback,
    const char* stage,
    std::uint32_t completed,
    std::uint32_t total,
    void* context
) {
    if (callback) callback(stage, completed, total, context);
}

bool record_mismatch(
    DiagnosticResult& result,
    const char* stage,
    std::uint32_t address
) {
    if (result.error_count == 0) result.first_failure = address;
    ++result.error_count;
    result.failed_stage = stage;
    return result.error_count < 64;
}

bool constant_pass(
    std::uint8_t value,
    std::uint32_t bytes,
    DiagnosticResult& result,
    ProgressCallback progress,
    void* context
) {
    const char* stage = value == 0x00 ? "PATTERN 00" :
        value == 0xff ? "PATTERN FF" :
        value == 0xaa ? "PATTERN AA" : "PATTERN 55";
    std::uint8_t buffer[128];
    std::memset(buffer, value, sizeof(buffer));
    report_progress(progress, stage, 0, bytes, context);
    for (std::uint32_t address = 0; address < bytes;) {
        const std::size_t chunk = std::min<std::size_t>(
            sizeof(buffer), bytes - address);
        if (!write_raw(address, buffer, chunk)) return false;
        address += static_cast<std::uint32_t>(chunk);
        if ((address & 0xffffu) == 0 || address == bytes)
            report_progress(progress, stage, address / 2, bytes, context);
    }
    for (std::uint32_t address = 0; address < bytes;) {
        const std::size_t chunk = std::min<std::size_t>(
            sizeof(buffer), bytes - address);
        if (!read_raw(address, buffer, chunk)) return false;
        for (std::size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != value &&
                !record_mismatch(result, stage, address + i)) return false;
        }
        address += static_cast<std::uint32_t>(chunk);
        if ((address & 0xffffu) == 0 || address == bytes)
            report_progress(progress, stage, bytes / 2 + address / 2,
                            bytes, context);
    }
    return result.error_count == 0;
}

bool walking_bit_pass(
    std::uint32_t bytes,
    DiagnosticResult& result,
    ProgressCallback progress,
    void* context
) {
    constexpr const char* stage = "WALKING BITS";
    report_progress(progress, stage, 0, 48, context);
    const std::uint32_t locations[] = {
        0,
        bytes > 256 ? (bytes / 2u) & ~255u : 0,
        bytes > 256 ? bytes - 256u : 0
    };
    std::uint8_t expected[16];
    std::uint8_t actual[16];
    std::uint32_t step = 0;
    for (const std::uint32_t base : locations) {
        for (int invert = 0; invert < 2; ++invert) {
            for (int bit = 0; bit < 8; ++bit) {
                std::memset(
                    expected, invert ? static_cast<std::uint8_t>(~(1u << bit))
                                     : static_cast<std::uint8_t>(1u << bit),
                    sizeof(expected));
                write_raw(base, expected, sizeof(expected));
                read_raw(base, actual, sizeof(actual));
                for (std::size_t i = 0; i < sizeof(actual); ++i) {
                    if (actual[i] != expected[i] &&
                        !record_mismatch(result, stage, base + i)) return false;
                }
                report_progress(progress, stage, ++step, 48, context);
            }
        }
    }
    return result.error_count == 0;
}

bool benchmark_address_pattern(
    std::uint32_t bytes,
    DiagnosticResult& result,
    ProgressCallback progress,
    void* context
) {
    constexpr const char* write_stage = "ADDRESS WRITE";
    constexpr const char* read_stage = "ADDRESS READ/CRC";
    std::uint8_t buffer[128];
    std::uint32_t expected_crc = 0xffffffffu;
    report_progress(progress, write_stage, 0, bytes, context);
    const std::uint64_t write_start = time_us_64();
    for (std::uint32_t address = 0; address < bytes;) {
        const std::size_t chunk = std::min<std::size_t>(
            sizeof(buffer), bytes - address);
        for (std::size_t i = 0; i < chunk; ++i)
            buffer[i] = address_pattern(address + i);
        expected_crc = crc32_update(expected_crc, buffer, chunk);
        write_raw(address, buffer, chunk);
        address += static_cast<std::uint32_t>(chunk);
        if ((address & 0xffffu) == 0 || address == bytes)
            report_progress(progress, write_stage, address, bytes, context);
    }
    result.write_microseconds = static_cast<std::uint32_t>(
        time_us_64() - write_start);

    std::uint32_t actual_crc = 0xffffffffu;
    report_progress(progress, read_stage, 0, bytes, context);
    const std::uint64_t read_start = time_us_64();
    for (std::uint32_t address = 0; address < bytes;) {
        const std::size_t chunk = std::min<std::size_t>(
            sizeof(buffer), bytes - address);
        read_raw(address, buffer, chunk);
        actual_crc = crc32_update(actual_crc, buffer, chunk);
        for (std::size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != address_pattern(address + i) &&
                !record_mismatch(result, read_stage, address + i)) return false;
        }
        address += static_cast<std::uint32_t>(chunk);
        if ((address & 0xffffu) == 0 || address == bytes)
            report_progress(progress, read_stage, address, bytes, context);
    }
    result.read_microseconds = static_cast<std::uint32_t>(
        time_us_64() - read_start);
    result.expected_crc32 = expected_crc ^ 0xffffffffu;
    result.actual_crc32 = actual_crc ^ 0xffffffffu;
    if (result.expected_crc32 != result.actual_crc32) {
        record_mismatch(result, "CRC32", 0);
        return false;
    }
    if (result.write_microseconds != 0) {
        result.write_bytes_per_second = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bytes) * 1000000u) /
            result.write_microseconds);
    }
    if (result.read_microseconds != 0) {
        result.read_bytes_per_second = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bytes) * 1000000u) /
            result.read_microseconds);
    }
    return result.error_count == 0;
}

bool benchmark_sizes(
    std::uint32_t tested_bytes,
    DiagnosticResult& result,
    ProgressCallback progress,
    void* context
) {
    constexpr std::uint32_t sizes[] = {
        1024u, 16u * 1024u, 64u * 1024u,
        256u * 1024u, 1024u * 1024u
    };
    constexpr std::uint32_t random_operations = 256;
    std::uint8_t buffer[128];
    result.benchmark_count = 0;
    report_progress(progress, "BENCHMARK", 0, 5, context);
    for (const std::uint32_t requested : sizes) {
        if (requested > tested_bytes) break;
        auto& sample = result.benchmarks[result.benchmark_count++];
        sample.bytes = requested;
        const std::uint64_t write_start = time_us_64();
        for (std::uint32_t address = 0; address < requested;) {
            const std::size_t chunk = std::min<std::size_t>(
                sizeof(buffer), requested - address);
            for (std::size_t i = 0; i < chunk; ++i)
                buffer[i] = address_pattern(address + i);
            write_raw(address, buffer, chunk);
            address += static_cast<std::uint32_t>(chunk);
        }
        const std::uint32_t write_us = static_cast<std::uint32_t>(
            time_us_64() - write_start);
        const std::uint64_t read_start = time_us_64();
        for (std::uint32_t address = 0; address < requested;) {
            const std::size_t chunk = std::min<std::size_t>(
                sizeof(buffer), requested - address);
            read_raw(address, buffer, chunk);
            address += static_cast<std::uint32_t>(chunk);
        }
        const std::uint32_t read_us = static_cast<std::uint32_t>(
            time_us_64() - read_start);
        if (write_us != 0) {
            sample.sequential_write_bytes_per_second =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(requested) * 1000000u) /
                    write_us);
        }
        if (read_us != 0) {
            sample.sequential_read_bytes_per_second =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(requested) * 1000000u) /
                    read_us);
        }

        std::uint32_t random_state = 0x43b0d7e5u;
        const std::uint64_t random_write_start = time_us_64();
        for (std::uint32_t operation = 0;
             operation < random_operations; ++operation) {
            random_state = random_state * 1664525u + 1013904223u;
            const std::uint32_t address =
                (random_state % (requested - 3u)) & ~3u;
            for (std::size_t i = 0; i < 4; ++i)
                buffer[i] = address_pattern(address + i);
            write_raw(address, buffer, 4);
        }
        const std::uint32_t random_write_us = static_cast<std::uint32_t>(
            time_us_64() - random_write_start);
        random_state = 0x43b0d7e5u;
        const std::uint64_t random_read_start = time_us_64();
        for (std::uint32_t operation = 0;
             operation < random_operations; ++operation) {
            random_state = random_state * 1664525u + 1013904223u;
            const std::uint32_t address =
                (random_state % (requested - 3u)) & ~3u;
            read_raw(address, buffer, 4);
            for (std::size_t i = 0; i < 4; ++i) {
                if (buffer[i] != address_pattern(address + i) &&
                    !record_mismatch(
                        result, "RANDOM VERIFY", address + i)) return false;
            }
        }
        const std::uint32_t random_read_us = static_cast<std::uint32_t>(
            time_us_64() - random_read_start);
        if (random_write_us != 0) {
            sample.random_write_operations_per_second =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(random_operations) *
                     1000000u) / random_write_us);
        }
        if (random_read_us != 0) {
            sample.random_read_operations_per_second =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(random_operations) *
                     1000000u) / random_read_us);
        }
        report_progress(
            progress, "BENCHMARK",
            static_cast<std::uint32_t>(result.benchmark_count), 5, context);
    }
    return result.error_count == 0;
}

struct ProbeProfile {
    std::uint32_t bus_clock_hz;
    bool falling_edge_fudge;
    bool fast_read;
};

constexpr ProbeProfile kProbeProfiles[kMaximumProbeAttempts] = {
    {25000000u, true,  true},
    {25000000u, false, true},
    {25000000u, false, false},
    {25000000u, true,  false},
    {12500000u, true,  true},
    {12500000u, false, true},
    {12500000u, false, false},
    { 6250000u, false, false}
};

bool configure_driver(const ProbeProfile& profile) {
    state.fast_read = profile.fast_read;
    state.falling_edge_fudge = profile.falling_edge_fudge;
    state.target_clock_hz = profile.bus_clock_hz;
    state.program = profile.falling_edge_fudge
        ? &cpb_psram_spi_fudge_program
        : &cpb_psram_spi_program;

    if (!pio_can_add_program(state.pio, state.program)) return false;
    state.state_machine = pio_claim_unused_sm(state.pio, false);
    if (state.state_machine < 0) return false;
    state.program_offset = pio_add_program(state.pio, state.program);
    state.program_loaded = true;

    gpio_set_drive_strength(kPinChipSelect, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(kPinClock, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(kPinMosi, GPIO_DRIVE_STRENGTH_4MA);
    cpb_psram_spi_init(
        state.pio,
        static_cast<uint>(state.state_machine),
        state.program_offset,
        clock_divider(),
        profile.falling_edge_fudge,
        kPinChipSelect,
        kPinMosi,
        kPinMiso);
    state.info.pio_state_machine = state.state_machine;
    state.info.bus_clock_hz = profile.bus_clock_hz;

    command(0x66);
    sleep_us(50);
    command(0x99);
    sleep_us(150);
    return true;
}

bool run_probe_attempt(
    const ProbeProfile& profile,
    ProbeAttempt& attempt
) {
    attempt = {};
    attempt.bus_clock_hz = profile.bus_clock_hz;
    attempt.falling_edge_fudge = profile.falling_edge_fudge;
    attempt.fast_read = profile.fast_read;
    if (!configure_driver(profile)) return false;

    const std::uint8_t id_transaction[] = {32, 24, 0x9f, 0, 0, 0};
    std::uint8_t id[3] = {};
    transfer(id_transaction, sizeof(id_transaction), id, sizeof(id));
    attempt.manufacturer_id = id[0];
    attempt.known_good_die = id[1];
    attempt.electronic_id = id[2];

    constexpr std::uint32_t probe_address = kPicoCalcPsramBytes - 16u;
    const std::uint8_t expected[16] = {
        0x55,0xaa,0x00,0xff,0x96,0x69,0x3c,0xc3,
        0x12,0x34,0x56,0x78,0x87,0x65,0x43,0x21
    };
    std::uint8_t saved[16] = {};
    std::uint8_t actual[16] = {};
    read_raw(probe_address, saved, sizeof(saved));
    write_raw(probe_address, expected, sizeof(expected));
    read_raw(probe_address, actual, sizeof(actual));
    write_raw(probe_address, saved, sizeof(saved));
    std::memcpy(attempt.readback, actual, sizeof(attempt.readback));
    attempt.passed = std::memcmp(actual, expected, sizeof(actual)) == 0;
    return attempt.passed;
}

} // namespace

bool init() {
    if (state.info.initialized) return state.info.available;
    state.info.initialized = true;
    state.info.error = "PIO1 RESOURCE UNAVAILABLE";
    sleep_us(200);

    state.info.error = "ALL PSRAM PROFILES FAILED";
    for (std::size_t index = 0; index < kMaximumProbeAttempts; ++index) {
        auto& attempt = state.info.probe_attempts[index];
        state.info.probe_attempt_count = index + 1;
        if (!run_probe_attempt(kProbeProfiles[index], attempt)) {
            release_driver();
            continue;
        }

        state.info.selected_probe_attempt = static_cast<int>(index);
        state.info.manufacturer_id = attempt.manufacturer_id;
        state.info.known_good_die = attempt.known_good_die;
        state.info.electronic_id = attempt.electronic_id;
        const std::uint32_t id_size =
            size_from_electronic_id(attempt.electronic_id);
        const bool id_verified =
            attempt.manufacturer_id == 0x0d &&
            attempt.known_good_die == 0x5d && id_size != 0;
        state.info.size_bytes = id_verified
            ? id_size : kPicoCalcPsramBytes;
        const std::uint32_t allocatable =
            state.info.size_bytes > kReservedTailBytes
                ? state.info.size_bytes - kReservedTailBytes : 0u;
        state.allocations.reset(allocatable);
        state.diagnostic_active = false;
        state.info.available = true;
        state.info.error = id_verified ? "OK" : "OK (PROBE FALLBACK)";
        return true;
    }
    state.info.size_bytes = 0;
    return false;
}

bool reprobe() {
    if (state.busy || state.diagnostic_active ||
        state.allocations.active_count() != 0) return false;
    release_driver();
    state.allocations.reset(0);
    state.info = {};
    return init();
}

void clock_changed() {
    if (!state.info.available || state.state_machine < 0) return;
    pio_sm_set_clkdiv(
        state.pio,
        static_cast<uint>(state.state_machine),
        clock_divider());
    state.info.bus_clock_hz = state.target_clock_hz;
}

const DeviceInfo& info() {
    return state.info;
}

bool busy() {
    return state.busy;
}

bool claim(
    Client client,
    std::uint32_t requested_bytes,
    std::uint32_t& base_address,
    std::uint32_t& allocated_bytes
) {
    base_address = 0;
    allocated_bytes = 0;
    if (!allocatable_client(client) || requested_bytes == 0 ||
        !state.info.available || state.busy || state.diagnostic_active)
        return false;
    return state.allocations.claim(
        client_slot(client),
        requested_bytes,
        base_address,
        allocated_bytes);
}

void release(Client client) {
    if (!allocatable_client(client) || state.busy ||
        state.diagnostic_active) return;
    state.allocations.release(client_slot(client));
}

Client owner() {
    if (state.diagnostic_active) return Client::Diagnostic;
    if (state.allocations.active_count() != 1) return Client::None;
    for (std::size_t slot = 0; slot < kAllocatableClientCount; ++slot) {
        if (state.allocations.active(slot))
            return static_cast<Client>(slot + 1u);
    }
    return Client::None;
}

const char* client_name(Client client) {
    switch (client) {
    case Client::EditorHistory: return "EDITOR HISTORY";
    case Client::ProgramStore: return "PROGRAM STORE";
    case Client::SdCache: return "SD CACHE";
    case Client::DirectState: return "DIRECT STATE";
    case Client::CompiledCache: return "COMPILED CACHE";
    case Client::AudioCache: return "AUDIO CACHE";
    case Client::Diagnostic: return "DIAGNOSTIC";
    default: return "IDLE";
    }
}

std::size_t active_client_count() {
    return state.allocations.active_count() +
        (state.diagnostic_active ? 1u : 0u);
}

AllocationInfo allocation(Client client) {
    AllocationInfo out{};
    if (!allocatable_client(client)) return out;
    const auto region = state.allocations.region(client_slot(client));
    out.active = region.used;
    out.base_address = region.base;
    out.allocated_bytes = region.size;
    return out;
}

std::uint32_t used_bytes() {
    return state.allocations.used_bytes();
}

bool read(
    std::uint32_t address,
    void* destination,
    std::size_t length
) {
    if (!state.info.available || state.busy ||
        (!destination && length != 0) || !bounds_valid(address, length)) {
        return false;
    }
    state.busy = true;
    const bool ok = read_raw(address, destination, length);
    state.busy = false;
    return ok;
}

bool write(
    std::uint32_t address,
    const void* source,
    std::size_t length
) {
    if (!state.info.available || state.busy ||
        (!source && length != 0) || !bounds_valid(address, length)) {
        return false;
    }
    state.busy = true;
    const bool ok = write_raw(address, source, length);
    state.busy = false;
    return ok;
}

bool fill(
    std::uint32_t address,
    std::uint8_t value,
    std::size_t length
) {
    if (!state.info.available || state.busy ||
        !bounds_valid(address, length)) return false;
    state.busy = true;
    std::uint8_t buffer[128];
    std::memset(buffer, value, sizeof(buffer));
    bool ok = true;
    while (length != 0) {
        const std::size_t chunk = std::min(length, sizeof(buffer));
        if (!write_raw(address, buffer, chunk)) {
            ok = false;
            break;
        }
        address += static_cast<std::uint32_t>(chunk);
        length -= chunk;
    }
    state.busy = false;
    return ok;
}

bool run_diagnostic(
    std::uint32_t requested_bytes,
    DiagnosticResult& result,
    ProgressCallback progress,
    void* context
) {
    result = {};
    if (!state.info.available || state.busy ||
        state.diagnostic_active || state.allocations.active_count() != 0) {
        result.failed_stage =
            (state.diagnostic_active || state.allocations.active_count() != 0)
                ? "PSRAM IN USE" : state.info.error;
        return false;
    }
    state.diagnostic_active = true;
    const std::uint32_t bytes = std::min(
        requested_bytes == 0 ? kQuickTestBytes : requested_bytes,
        state.info.size_bytes);
    if (bytes < 256) {
        result.failed_stage = "TEST RANGE TOO SMALL";
        state.diagnostic_active = false;
        return false;
    }
    state.busy = true;
    result.ran = true;
    result.tested_bytes = bytes;
    bool ok = true;
    for (const std::uint8_t value : {0x00u, 0xffu, 0xaau, 0x55u}) {
        if (!constant_pass(value, bytes, result, progress, context)) {
            ok = false;
            break;
        }
    }
    if (ok) ok = walking_bit_pass(
        bytes, result, progress, context);
    if (ok) ok = benchmark_address_pattern(
        bytes, result, progress, context);
    if (ok) ok = benchmark_sizes(
        bytes, result, progress, context);
    result.passed = ok && result.error_count == 0;
    if (result.passed) result.failed_stage = "PASS";
    state.busy = false;
    state.diagnostic_active = false;
    return result.passed;
}

} // namespace rmb::psram
