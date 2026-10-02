#include "psram_editor_history.hpp"
#include "psram.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rmb::psram {
namespace {
std::vector<std::uint8_t> memory(8u * 1024u * 1024u);
Client client = Client::None;
DeviceInfo device{};
}
bool init() { device.available = true; device.size_bytes = memory.size(); return true; }
bool reprobe() { return init(); }
void clock_changed() {}
const DeviceInfo& info() { return device; }
bool busy() { return false; }
bool claim(Client requested, std::uint32_t bytes,
           std::uint32_t& base, std::uint32_t& allocated) {
    if (client != Client::None || requested == Client::None) return false;
    client = requested; base = 0;
    allocated = std::min<std::uint32_t>(bytes, memory.size() - 16u);
    return allocated != 0;
}
void release(Client requested) { if (client == requested) client = Client::None; }
Client owner() { return client; }
std::uint32_t used_bytes() { return client == Client::None ? 0 : 6u * 1024u * 1024u; }
bool read(std::uint32_t address, void* destination, std::size_t length) {
    if (address + length > memory.size()) return false;
    std::memcpy(destination, memory.data() + address, length); return true;
}
bool write(std::uint32_t address, const void* source, std::size_t length) {
    if (address + length > memory.size()) return false;
    std::memcpy(memory.data() + address, source, length); return true;
}
bool fill(std::uint32_t address, std::uint8_t value, std::size_t length) {
    if (address + length > memory.size()) return false;
    std::memset(memory.data() + address, value, length); return true;
}
bool run_diagnostic(std::uint32_t, DiagnosticResult&, ProgressCallback, void*) {
    return false;
}
void corrupt_first_record() { memory[0] ^= 0xffu; }
} // namespace rmb::psram

int main() {
    rmb::psram::init();
    rmb::PsramEditorHistory history;
    assert(history.begin() && history.ready());
    assert(history.allocated_bytes() <= 256u * 1024u);
    assert(history.allocated_bytes() >= 200u * 1024u);
    assert(rmb::psram::owner() == rmb::psram::Client::EditorHistory);

    rmb::EditorHistorySnapshot input{};
    input.state_id = 7;
    input.line_number = 120;
    input.line_index = 3;
    input.length = 5;
    input.cursor = 4;
    input.existed = true;
    std::memcpy(input.body, "HELLO", 6);
    assert(history.push_undo(input));
    assert(history.undo_count() == 1);

    rmb::EditorHistorySnapshot output{};
    assert(history.pop_undo(output));
    assert(output.state_id == 7 && output.line_number == 120);
    assert(output.cursor == 4 && !std::strcmp(output.body, "HELLO"));
    assert(history.push_redo(output) && history.redo_count() == 1);
    history.clear_redo(); assert(history.redo_count() == 0);

    // Two-line 4 KiB snapshots keep roughly 31 records per stack in 256 KiB.
    history.clear();
    for (std::uint64_t i = 0; i < 100; ++i) {
        input.state_id = i;
        assert(history.push_undo(input));
    }
    assert(history.undo_count() >= 30 && history.undo_count() <= 32);

    history.clear();
    assert(history.push_undo(input));
    rmb::psram::corrupt_first_record();
    assert(!history.pop_undo(output));
    history.end();
    assert(rmb::psram::owner() == rmb::psram::Client::None);
    std::puts("PSRAM editor history: PASS");
}
