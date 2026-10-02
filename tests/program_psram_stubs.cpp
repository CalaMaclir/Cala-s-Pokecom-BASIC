#include "psram.hpp"
#include "psram_allocator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rmb::psram {
namespace {
std::vector<std::uint8_t> memory(8u * 1024u * 1024u);
detail::RegionAllocator<kAllocatableClientCount> allocator(
    static_cast<std::uint32_t>(memory.size() - 16u));
bool enabled = true;
int fail_write_after = 0;
DeviceInfo device = [] {
    DeviceInfo value;
    value.initialized = true;
    value.available = true;
    value.size_bytes = 8u * 1024u * 1024u;
    value.error = "OK";
    return value;
}();

bool allocatable(Client client) {
    return client >= Client::EditorHistory && client <= Client::AudioCache;
}
std::size_t slot(Client client) {
    return static_cast<std::size_t>(client) - 1u;
}
}

bool init() {
    device.initialized = true;
    device.available = enabled;
    return enabled;
}
bool reprobe() {
    device.available = enabled;
    return enabled && allocator.active_count() == 0;
}
void test_set_available(bool value) {
    if (allocator.active_count() != 0) return;
    enabled = value;
    device.initialized = true;
    device.available = value;
}
void test_fail_write_after(int n) { fail_write_after = n; }

void clock_changed() {}
const DeviceInfo& info() { return device; }
bool busy() { return false; }

bool claim(
    Client client,
    std::uint32_t requested,
    std::uint32_t& base,
    std::uint32_t& allocated
) {
    if(!allocatable(client)) {
        base=allocated=0;
        return false;
    }
    return allocator.claim(slot(client),requested,base,allocated);
}

void release(Client client) {
    if(allocatable(client)) allocator.release(slot(client));
}

Client owner() {
    if(allocator.active_count()!=1) return Client::None;
    for(std::size_t i=0;i<kAllocatableClientCount;++i)
        if(allocator.active(i)) return static_cast<Client>(i+1u);
    return Client::None;
}

const char* client_name(Client client) {
    switch(client) {
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

std::size_t active_client_count() { return allocator.active_count(); }

AllocationInfo allocation(Client client) {
    AllocationInfo out{};
    if(!allocatable(client)) return out;
    const auto region=allocator.region(slot(client));
    out.active=region.used;
    out.base_address=region.base;
    out.allocated_bytes=region.size;
    return out;
}

std::uint32_t used_bytes() { return allocator.used_bytes(); }

bool read(std::uint32_t address,void* destination,std::size_t length) {
    if((!destination&&length)||address>memory.size()||
       length>memory.size()-address) return false;
    std::memcpy(destination,memory.data()+address,length);
    return true;
}

bool write(std::uint32_t address,const void* source,std::size_t length) {
    if((!source&&length)||address>memory.size()||
       length>memory.size()-address) return false;
    if (fail_write_after > 0 && --fail_write_after == 0) {
        std::memcpy(memory.data()+address,source,length/2);
        return false;
    }
    std::memcpy(memory.data()+address,source,length);
    return true;
}

bool fill(std::uint32_t address,std::uint8_t value,std::size_t length) {
    if(address>memory.size()||length>memory.size()-address) return false;
    std::memset(memory.data()+address,value,length);
    return true;
}

bool run_diagnostic(
    std::uint32_t,
    DiagnosticResult& result,
    ProgressCallback,
    void*
) {
    result={};
    return false;
}

} // namespace rmb::psram
