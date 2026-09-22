#include "usb_msc.hpp"
#include "storage.hpp"
#include "tusb.h"

#include <atomic>
#include <cstring>

namespace {
// Written by the SDK USB IRQ task and readable from the main loop.
std::atomic<bool> ejected{false};
std::atomic<bool> present{false};
static_assert(std::atomic<bool>::is_always_lock_free, "USB state must not block in IRQ");

bool valid_lun(uint8_t lun) {
    if (lun == 0) return true;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x25, 0); // unsupported LUN
    return false;
}

bool no_media(uint8_t lun) {
    if (valid_lun(lun)) tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
    return false;
}
}

namespace rmb::usb_msc {
bool active() { return storage::owner() == storage::Owner::UsbHost; }
bool media_present() { return present.load(std::memory_order_acquire) && active(); }
bool set_media_present(bool value) {
    if (value && (storage::owner() != storage::Owner::UsbHost ||
                  storage::usb_block_count() == 0)) return false;
    present.store(value, std::memory_order_release);
    if (value) ejected.store(false, std::memory_order_relaxed);
    return true;
}
bool host_ejected() { return ejected.load(std::memory_order_relaxed); }
void reset_session() { ejected.store(false, std::memory_order_relaxed); }
void disconnected() {
    present.store(false, std::memory_order_release);
    if (host_ejected()) storage::request_usb_safe_return(true);
    else storage::notify_usb_disconnect();
}
}

extern "C" {
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4]) {
    // Fixed-width fields: no terminator, no write beyond 8 / 16 / 4 bytes.
    std::memcpy(vendor, "CALA    ", 8);
    std::memcpy(product, "CPB SD CARD     ", 16);
    std::memcpy(revision, "0.8 ", 4);
    valid_lun(lun);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!valid_lun(lun)) return false;
    if (rmb::usb_msc::media_present()) return true;
    return no_media(lun);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    const bool lun_ok = valid_lun(lun);
    const auto count = lun_ok && rmb::usb_msc::media_present()
        ? rmb::storage::usb_block_count() : 0;
    if (block_count) *block_count = count;
    if (block_size) *block_size = 512;
    // TinyUSB 2.3.1's pinned MSC driver rejects zero block_count before
    // constructing READ CAPACITY / READ FORMAT CAPACITIES responses.
    if (!count) no_media(lun);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    if (!valid_lun(lun)) return false;
    if (rmb::usb_msc::media_present()) return true;
    return no_media(lun);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    if (!valid_lun(lun)) return false;
    if (power_condition != 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0);
        return false;
    }
    if (load_eject && !start) {
        ejected.store(true, std::memory_order_relaxed);
        present.store(false, std::memory_order_release);
        return true;
    }
    // STOP without eject is harmless; START/LOAD cannot make media ready.
    return start ? no_media(lun) : true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t, uint8_t) {
    if (!valid_lun(lun)) return false;
    return rmb::usb_msc::media_present() ? true : no_media(lun);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t size) {
    if (!valid_lun(lun) || !rmb::usb_msc::media_present()) { no_media(lun); return -1; }
    const auto blocks = rmb::storage::usb_block_count();
    if (!buffer || lba >= blocks || offset != 0 || size != rmb::storage::usb_block_size()) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }
    if (rmb::storage::usb_read_block(lba, offset, buffer, size)) return static_cast<int32_t>(size);
    present.store(false, std::memory_order_release);
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0);
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t size) {
    if (!valid_lun(lun) || !rmb::usb_msc::media_present()) { no_media(lun); return -1; }
    const auto blocks = rmb::storage::usb_block_count();
    if (!buffer || lba >= blocks || offset != 0 || size != rmb::storage::usb_block_size()) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }
    if (rmb::storage::usb_write_block(lba, offset, buffer, size)) return static_cast<int32_t>(size);
    present.store(false, std::memory_order_release);
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0x02);
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t[16], void*, uint16_t) {
    // TinyUSB handles INQUIRY, REQUEST SENSE, TUR, capacity and START STOP.
    // Reject every remaining command, including vendor-specific commands.
    if (valid_lun(lun)) tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}
}
