#include "usb_device.hpp"
#include "usb_msc.hpp"
#include "storage.hpp"
#include "tusb.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
bool mounted = false;
uint8_t sense_lun, sense_key, sense_asc, sense_ascq;
constexpr uint32_t fake_blocks = 8;
std::array<uint8_t, fake_blocks * 512> fake_media{};
bool fail_read = false;
bool fail_write = false;
bool safe_eject_notified = false;
bool disconnect_notified = false;

void expect_no_media(uint8_t lun = 0) {
    assert(sense_lun == lun);
    assert(sense_key == (lun ? SCSI_SENSE_ILLEGAL_REQUEST : SCSI_SENSE_NOT_READY));
    assert(sense_asc == (lun ? 0x25 : 0x3a));
    assert(sense_ascq == 0);
    assert(!rmb::usb_msc::active());
    assert(!rmb::usb_msc::media_present());
}
}

namespace rmb::storage {
static Owner fake_owner = Owner::Firmware;
Owner owner() { return fake_owner; }
std::uint32_t usb_block_count() {
    return fake_owner == Owner::UsbHost ? fake_blocks : 0;
}
bool usb_read_block(std::uint32_t lba, std::uint32_t offset, void* buffer, std::uint32_t size) {
    if (fake_owner != Owner::UsbHost || fail_read || !buffer ||
        lba >= fake_blocks || offset != 0 || size != 512) return false;
    std::memcpy(buffer, fake_media.data() + lba * 512, 512);
    return true;
}
bool usb_write_block(std::uint32_t lba, std::uint32_t offset, const void* buffer, std::uint32_t size) {
    if (fake_owner != Owner::UsbHost || fail_write || !buffer ||
        lba >= fake_blocks || offset != 0 || size != 512) return false;
    std::memcpy(fake_media.data() + lba * 512, buffer, 512);
    return true;
}
bool request_usb_safe_return(bool host_ejected) {
    if (!host_ejected) return false;
    safe_eject_notified = true;
    if (fake_owner == Owner::UsbHost) fake_owner = Owner::Transition;
    return fake_owner == Owner::Transition;
}
void notify_usb_disconnect() {
    disconnect_notified = true;
    if (fake_owner == Owner::UsbHost || fake_owner == Owner::Transition)
        fake_owner = Owner::Unavailable;
}
}

namespace {

std::string descriptor_string(uint8_t index) {
    const uint16_t* p = tud_descriptor_string_cb(index, 0x0409);
    assert(p && p[0] >> 8 == TUSB_DESC_STRING);
    const unsigned bytes = p[0] & 0xff;
    assert(bytes >= 2 && bytes <= 66 && bytes % 2 == 0);
    std::string s;
    for (unsigned i = 1; i < bytes / 2; ++i) s += static_cast<char>(p[i]);
    return s;
}

void descriptors() {
    const auto* d = reinterpret_cast<const tusb_desc_device_t*>(tud_descriptor_device_cb());
    assert(d->bLength == 18 && d->bcdUSB == 0x0200);
    assert(d->bDeviceClass == TUSB_CLASS_MISC && d->bDeviceSubClass == MISC_SUBCLASS_COMMON);
    assert(d->bDeviceProtocol == MISC_PROTOCOL_IAD && d->bNumConfigurations == 1);
    assert(d->idVendor == 0xcafe && d->idProduct == 0x4003);
    assert(descriptor_string(d->iManufacturer) == "Cala Maclir");
    assert(descriptor_string(d->iProduct) == "Cala's Pokecom BASIC");
    assert(descriptor_string(d->iSerialNumber) == "0123456789ABCDEF");
    assert(tud_descriptor_string_cb(0, 0)[1] == 0x0409);
    assert(!tud_descriptor_string_cb(6, 0));
    assert(!tud_descriptor_string_cb(255, 0));
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    assert(c && !tud_descriptor_configuration_cb(1));
    const unsigned total = c[2] | (c[3] << 8);
    assert(total == TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN);
    assert(c[4] == 3);
    bool seen[256] = {};
    unsigned interfaces = 0, endpoints = 0, iad = 0;
    uint8_t current_interface = 255;
    for (unsigned at = 0; at < total;) {
        const uint8_t len = c[at], type = c[at + 1];
        assert(len >= 2 && at + len <= total);
        if (type == TUSB_DESC_INTERFACE_ASSOCIATION) {
            assert(c[at + 2] == 0 && c[at + 3] == 2);
            ++iad;
        } else if (type == TUSB_DESC_INTERFACE) {
            current_interface = c[at + 2];
            assert(current_interface == interfaces++);
            const uint8_t expected[] = {TUSB_CLASS_CDC, TUSB_CLASS_CDC_DATA, TUSB_CLASS_MSC};
            assert(c[at + 5] == expected[current_interface]);
            assert(!descriptor_string(c[at + 8]).empty());
        } else if (type == TUSB_DESC_ENDPOINT) {
            const uint8_t ep = c[at + 2];
            assert(!seen[ep]);
            seen[ep] = true;
            ++endpoints;
            const unsigned packet = c[at + 4] | (c[at + 5] << 8);
            if (current_interface == 0) assert(ep == 0x81 && packet == 8);
            else if (current_interface == 1) assert((ep == 0x02 || ep == 0x82) && packet == 64);
            else assert((ep == 0x03 || ep == 0x83) && packet == 64);
        }
        at += len;
    }
    assert(interfaces == 3 && endpoints == 5 && iad == 1);
}
}

extern "C" bool tud_mounted() { return mounted; }
extern "C" bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    sense_lun = lun; sense_key = key; sense_asc = asc; sense_ascq = ascq;
    return true;
}
void pico_get_unique_board_id_string(char* buffer, size_t length) {
    assert(length >= 17);
    std::memcpy(buffer, "0123456789ABCDEF", 17);
}

int main() {
    descriptors();
    const uint8_t* fixed_config = tud_descriptor_configuration_cb(0);
    std::array<uint8_t, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN> saved_config;
    std::memcpy(saved_config.data(), fixed_config, saved_config.size());
    assert(!rmb::usb_device::usb_connected());
    for (unsigned session = 0; session < 100; ++session) {
        rmb::storage::fake_owner = rmb::storage::Owner::Firmware;
        mounted = true;
        tud_mount_cb();
        assert(rmb::usb_device::usb_connected() && !rmb::usb_msc::host_ejected());
        for (bool present : {false, true, false}) {
            assert(rmb::usb_msc::set_media_present(present) == !present);
            assert(!tud_msc_test_unit_ready_cb(0));
            expect_no_media();
            assert(rmb::usb_device::usb_connected());
        }
        uint32_t blocks = 123;
        uint16_t size = 0;
        tud_msc_capacity_cb(0, &blocks, &size);
        assert(blocks == 0 && size == 512);
        expect_no_media();
        tud_msc_capacity_cb(0, nullptr, nullptr);
        assert(!tud_msc_is_writable_cb(0));
        assert(tud_msc_start_stop_cb(0, 0, false, false));
        assert(!rmb::usb_msc::host_ejected());
        assert(tud_msc_start_stop_cb(0, 0, false, true));
        assert(rmb::usb_msc::host_ejected());
        assert(!tud_msc_start_stop_cb(0, 0, true, true));
        expect_no_media();
        assert(rmb::usb_msc::host_ejected());
        assert(!tud_msc_start_stop_cb(1, 0, false, true));
        expect_no_media(1);
        assert(!tud_msc_start_stop_cb(0, 1, false, true));
        assert(sense_key == SCSI_SENSE_ILLEGAL_REQUEST && sense_asc == 0x24);
        assert(!tud_msc_prevent_allow_medium_removal_cb(0, 0, 0));
        expect_no_media();
        assert(fixed_config == tud_descriptor_configuration_cb(0));
        assert(std::memcmp(saved_config.data(), fixed_config, saved_config.size()) == 0);
        mounted = false;
        tud_umount_cb();
        assert(!rmb::usb_device::usb_connected() && rmb::usb_msc::host_ejected());
    }

    // Real-media path: exact 512-byte blocks are transferred and every
    // partial/out-of-range request is rejected before touching the medium.
    mounted = true;
    rmb::storage::fake_owner = rmb::storage::Owner::UsbHost;
    safe_eject_notified = disconnect_notified = false;
    assert(rmb::usb_msc::set_media_present(true));
    assert(rmb::usb_msc::active() && rmb::usb_msc::media_present());
    assert(tud_msc_test_unit_ready_cb(0) && tud_msc_is_writable_cb(0));
    uint32_t block_count = 0;
    uint16_t block_size = 0;
    tud_msc_capacity_cb(0, &block_count, &block_size);
    assert(block_count == fake_blocks && block_size == 512);
    block_count = 123;
    tud_msc_capacity_cb(1, &block_count, &block_size);
    assert(block_count == 0 && sense_key == SCSI_SENSE_ILLEGAL_REQUEST && sense_asc == 0x25);
    std::array<uint8_t, 512> write_block{};
    std::array<uint8_t, 512> read_block{};
    for (std::size_t i = 0; i < write_block.size(); ++i)
        write_block[i] = static_cast<uint8_t>(i ^ 0x5a);
    assert(tud_msc_write10_cb(0, 3, 0, write_block.data(), 512) == 512);
    assert(tud_msc_read10_cb(0, 3, 0, read_block.data(), 512) == 512);
    assert(read_block == write_block);

    // Withdrawing only the MSC medium rejects subsequent raw requests while
    // leaving the composite device and its CDC console mounted.
    assert(rmb::usb_msc::set_media_present(false));
    assert(mounted && rmb::usb_device::usb_connected());
    assert(rmb::storage::fake_owner == rmb::storage::Owner::UsbHost);
    assert(tud_msc_read10_cb(0, 0, 0, read_block.data(), 512) == -1);
    assert(tud_msc_write10_cb(0, 0, 0, write_block.data(), 512) == -1);
    assert(!disconnect_notified);
    assert(rmb::usb_msc::set_media_present(true));

    assert(tud_msc_read10_cb(0, fake_blocks, 0, read_block.data(), 512) == -1);
    assert(sense_key == SCSI_SENSE_ILLEGAL_REQUEST && sense_asc == 0x21);
    assert(tud_msc_write10_cb(0, 0, 1, write_block.data(), 512) == -1);
    assert(tud_msc_read10_cb(0, 0, 0, read_block.data(), 511) == -1);
    assert(tud_msc_read10_cb(0, 0, 0, nullptr, 512) == -1);
    assert(tud_msc_write10_cb(0, 0, 0, nullptr, 512) == -1);

    fail_read = true;
    assert(tud_msc_read10_cb(0, 0, 0, read_block.data(), 512) == -1);
    assert(sense_key == SCSI_SENSE_MEDIUM_ERROR && sense_asc == 0x11);
    assert(!rmb::usb_msc::media_present());
    fail_read = false;
    assert(rmb::usb_msc::set_media_present(true));
    fail_write = true;
    assert(tud_msc_write10_cb(0, 0, 0, write_block.data(), 512) == -1);
    assert(sense_key == SCSI_SENSE_MEDIUM_ERROR && sense_asc == 0x0c && sense_ascq == 0x02);
    fail_write = false;
    assert(rmb::usb_msc::set_media_present(true));
    assert(tud_msc_start_stop_cb(0, 0, false, true));
    assert(!safe_eject_notified && rmb::storage::fake_owner == rmb::storage::Owner::UsbHost);
    assert(rmb::usb_msc::host_ejected());
    assert(!rmb::usb_msc::media_present());

    // Safe eject is a latch. Explicit Return (or a later USB disconnect)
    // performs the ownership transition; CDC remains independently mounted.
    rmb::usb_msc::disconnected();
    assert(safe_eject_notified && rmb::storage::fake_owner == rmb::storage::Owner::Transition);
    assert(mounted);

    rmb::storage::fake_owner = rmb::storage::Owner::UsbHost;
    assert(rmb::usb_msc::set_media_present(true));
    tud_umount_cb();
    assert(disconnect_notified && rmb::storage::fake_owner == rmb::storage::Owner::Unavailable);
    assert(!rmb::usb_msc::media_present());
    // Guard every inquiry field: no hidden string terminator may overflow.
    std::array<uint8_t, 10> vendor;
    std::array<uint8_t, 18> product;
    std::array<uint8_t, 6> revision;
    vendor.fill(0xa5); product.fill(0xa5); revision.fill(0xa5);
    tud_msc_inquiry_cb(0, vendor.data() + 1, product.data() + 1, revision.data() + 1);
    assert(vendor.front() == 0xa5 && vendor.back() == 0xa5);
    assert(product.front() == 0xa5 && product.back() == 0xa5);
    assert(revision.front() == 0xa5 && revision.back() == 0xa5);
    assert(std::memcmp(vendor.data() + 1, "CALA    ", 8) == 0);
    assert(std::memcmp(product.data() + 1, "CPB SD CARD     ", 16) == 0);
    assert(std::memcmp(revision.data() + 1, "0.8 ", 4) == 0);

    // Requests are rejected before touching any buffer/address, including
    // absent, undersized, zero-length and overflowing address/length inputs.
    std::array<uint8_t, 514> buffer;
    buffer.fill(0xa5);
    for (uint8_t lun : {0, 1, 255}) {
        for (uint32_t lba : {0u, 1u, UINT32_MAX}) {
            for (uint32_t offset : {0u, 511u, 512u, UINT32_MAX}) {
                for (uint32_t bytes : {0u, 1u, 511u, 512u, 513u, UINT32_MAX}) {
                    for (uint8_t* p : {buffer.data() + 1, static_cast<uint8_t*>(nullptr)}) {
                        assert(tud_msc_read10_cb(lun, lba, offset, p, bytes) == -1);
                        expect_no_media(lun);
                        assert(tud_msc_write10_cb(lun, lba, offset, p, bytes) == -1);
                        expect_no_media(lun);
                    }
                }
            }
        }
    }
    for (uint8_t byte : buffer) assert(byte == 0xa5);
    uint8_t command[16] = {0xff};
    assert(tud_msc_scsi_cb(0, command, nullptr, 0) == -1);
    assert(sense_key == SCSI_SENSE_ILLEGAL_REQUEST && sense_asc == 0x20);
    assert(tud_msc_scsi_cb(1, nullptr, nullptr, UINT16_MAX) == -1);
    expect_no_media(1);
    std::puts("USB descriptors / MSC No Media / eject / bounds: PASS");
}
