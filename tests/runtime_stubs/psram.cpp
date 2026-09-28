#include "psram.hpp"

namespace rmb::psram {
namespace {
DeviceInfo device{};
}
bool init() { device.initialized = true; return false; }
bool reprobe() { return false; }
void clock_changed() {}
const DeviceInfo& info() { return device; }
bool busy() { return false; }
bool claim(Client, std::uint32_t, std::uint32_t&, std::uint32_t&) {
    return false;
}
void release(Client) {}
Client owner() { return Client::None; }
std::uint32_t used_bytes() { return 0; }
bool read(std::uint32_t, void*, std::size_t) { return false; }
bool write(std::uint32_t, const void*, std::size_t) { return false; }
bool fill(std::uint32_t, std::uint8_t, std::size_t) { return false; }
bool run_diagnostic(
    std::uint32_t,
    DiagnosticResult& result,
    ProgressCallback,
    void*
) {
    result = {};
    return false;
}
} // namespace rmb::psram
