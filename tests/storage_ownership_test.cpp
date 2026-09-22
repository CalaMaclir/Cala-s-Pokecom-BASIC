#include "storage.hpp"
#include "blockdevice/sd.h"
#include "filesystem/fat.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
constexpr std::uint32_t kBlocks = 8;
std::array<std::uint8_t, kBlocks * 512> medium{};
bool card = true;
bool fail_mount = false;
bool fail_unmount = false;
bool fail_sync = false;
bool fail_read = false;
bool fail_write = false;
int mounts = 0;
int unmounts = 0;
std::atomic<bool> hold_read{false};
std::atomic<bool> read_entered{false};
std::atomic<bool> release_read{false};

int read_block(blockdevice_t*, void* out, std::uint64_t address, std::size_t size) {
    if (fail_read || address + size > medium.size()) return -1;
    if (hold_read.load()) {
        read_entered.store(true);
        while (!release_read.load()) std::this_thread::yield();
    }
    std::memcpy(out, medium.data() + address, size);
    return 0;
}
int write_block(blockdevice_t*, const void* in, std::uint64_t address, std::size_t size) {
    if (fail_write || address + size > medium.size()) return -1;
    std::memcpy(medium.data() + address, in, size);
    return 0;
}
int sync_block(blockdevice_t*) { return fail_sync ? -1 : 0; }
std::uint64_t media_size(blockdevice_t*) { return medium.size(); }
blockdevice_t device{read_block, write_block, sync_block, media_size};
filesystem_t filesystem;
spi_inst_t spi;
}

spi_inst_t* spi0 = &spi;
blockdevice_t* blockdevice_sd_create(
    spi_inst_t*, unsigned, unsigned, unsigned, unsigned, std::uint32_t, bool) {
    return &device;
}
filesystem_t* filesystem_fat_create() { return &filesystem; }
int fs_mount(const char*, filesystem_t*, blockdevice_t*) {
    ++mounts;
    return fail_mount ? -1 : 0;
}
int fs_unmount(const char*) {
    ++unmounts;
    return fail_unmount ? -1 : 0;
}
void gpio_init(unsigned) {}
void gpio_set_dir(unsigned, bool) {}
void gpio_pull_up(unsigned) {}
int gpio_get(unsigned) { return card ? 0 : 1; }
void sleep_ms(std::uint32_t) {}

int main() {
    using namespace rmb::storage;
    assert(owner() == Owner::Firmware);
    assert(init() && available());

    // A failed unmount keeps the mounted firmware state intact.
    fail_unmount = true;
    assert(!begin_usb_host_ownership());
    assert(owner() == Owner::Firmware && available());
    fail_unmount = false;

    assert(begin_usb_host_ownership());
    assert(owner() == Owner::UsbHost && !available());
    assert(usb_block_count() == kBlocks);
    std::array<std::uint8_t, 512> input{}, output{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = i ^ 0xa5;
    assert(usb_write_block(2, 0, input.data(), input.size()));
    assert(usb_read_block(2, 0, output.data(), output.size()));
    assert(input == output);
    assert(!usb_read_block(kBlocks, 0, output.data(), output.size()));
    assert(!usb_read_block(0, 1, output.data(), output.size()));
    assert(!try_lock());

    // Safe return is refused until the host has issued a safe eject.
    assert(!request_usb_safe_return(false));
    assert(owner() == Owner::UsbHost && !available());
    assert(request_usb_safe_return(true));
    assert(owner() == Owner::Transition && !available());
    poll();
    assert(owner() == Owner::Firmware && available());
    assert(take_usb_event() == UsbEvent::ReturnedToFirmware);

    // A remount failure never returns a stale mounted state.
    assert(begin_usb_host_ownership());
    fail_mount = true;
    assert(request_usb_safe_return(true));
    poll();
    assert(owner() == Owner::Unavailable && !available());
    assert(take_usb_event() == UsbEvent::IoError);
    fail_mount = false;
    assert(recover_firmware_ownership());
    assert(take_usb_event() == UsbEvent::ReturnedToFirmware);

    // Disconnect is deliberately not auto-remounted.
    assert(begin_usb_host_ownership());
    notify_usb_disconnect();
    poll();
    assert(owner() == Owner::Unavailable && !available());
    assert(take_usb_event() == UsbEvent::UnsafeDisconnect);
    assert(recover_firmware_ownership());
    take_usb_event();

    // Force return withdraws the medium without disconnecting USB CDC. Once
    // transition starts, new raw requests fail, and remount waits for the one
    // raw operation that was already executing.
    assert(begin_usb_host_ownership());
    hold_read.store(true);
    read_entered.store(false);
    release_read.store(false);
    std::thread in_flight([&] {
        assert(usb_read_block(0, 0, output.data(), output.size()));
    });
    while (!read_entered.load()) std::this_thread::yield();
    assert(request_usb_force_return());
    assert(owner() == Owner::Transition);
    assert(!usb_read_block(1, 0, output.data(), output.size()));
    poll();
    assert(owner() == Owner::Transition);
    release_read.store(true);
    in_flight.join();
    hold_read.store(false);
    poll();
    assert(owner() == Owner::Firmware && available());
    assert(take_usb_event() == UsbEvent::ReturnedToFirmware);

    // Removal and a raw write error both require explicit recovery.
    assert(begin_usb_host_ownership());
    card = false;
    poll();
    assert(owner() == Owner::Unavailable);
    assert(take_usb_event() == UsbEvent::CardRemoved);
    card = true;
    assert(recover_firmware_ownership());
    take_usb_event();
    assert(begin_usb_host_ownership());
    fail_write = true;
    assert(!usb_write_block(0, 0, input.data(), input.size()));
    assert(owner() == Owner::Unavailable);
    assert(take_usb_event() == UsbEvent::IoError);
    fail_write = false;
    assert(recover_firmware_ownership());
    take_usb_event();

    // Sync failure during handoff rolls back to the firmware mount.
    fail_sync = true;
    assert(!begin_usb_host_ownership());
    assert(owner() == Owner::Firmware && available());
    fail_sync = false;

    assert(mounts >= 6 && unmounts >= 6);
    std::puts("Storage ownership / raw block / recovery: PASS");
}
