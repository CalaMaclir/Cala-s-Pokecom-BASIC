#pragma once

#include <cstddef>
#include <cstdint>

#include "program_store.hpp"

namespace rmb::storage {

enum class Owner : std::uint8_t { Firmware, UsbHost, Transition, Unavailable };
enum class UsbEvent : std::uint8_t { None, ReturnedToFirmware, UnsafeDisconnect, CardRemoved, IoError };

Owner owner();
const char* owner_name();
bool firmware_owns_card();
bool begin_usb_host_ownership();
// Called by the foreground only. Return processing waits for every raw MSC
// operation to finish before syncing and remounting the card.
void poll();
UsbEvent take_usb_event();
bool recover_firmware_ownership();
// IRQ-safe transition requests; neither calls FatFs nor the SD driver.
// A normal return is accepted only after the host's START STOP UNIT eject.
bool request_usb_safe_return(bool host_ejected);
bool request_usb_force_return();
void notify_usb_disconnect();

std::uint32_t usb_block_count();
constexpr std::uint16_t usb_block_size() { return 512; }
bool usb_read_block(std::uint32_t lba, std::uint32_t offset, void* buffer, std::uint32_t size);
bool usb_write_block(std::uint32_t lba, std::uint32_t offset, const void* buffer, std::uint32_t size);

bool init();
bool available();
bool card_present();
bool remount();
const char* last_error();

// The HTTP server performs all FAT access from the foreground, but a transfer
// spans many foreground polls. This lease prevents BASIC, screenshots and
// settings writes from entering FAT while that transaction is open.
bool try_lock();
void unlock();
bool busy();

bool load_program(const char* name, ProgramStore& program);
bool save_program(const char* name, ProgramStore& program);
bool program_exists(const char* name);
bool list_program_files();
// DIR view: all visible regular root files (not ProgramStore work/staging files).
bool list_root_files();
std::size_t collect_program_files(
    char* output,
    std::size_t max_files,
    std::size_t slot_size
);
std::size_t collect_transfer_files(
    char* output,
    std::size_t max_files,
    std::size_t slot_size
);

bool read_root_text(
    const char* filename,
    char* output,
    std::size_t capacity
);
bool write_root_text(const char* filename, const char* text);

bool screenshot_exists(const char* name);
bool save_screenshot(
    const char* name,
    int x1 = 0,
    int y1 = 0,
    int x2 = 319,
    int y2 = 319
);

} // namespace rmb::storage
