#include "pico/unique_id.h"
#include "tusb.h"

#include <cstddef>

namespace {
// TinyUSB cdc_msc example development VID/PID, NOT an allocated CPB identity.
// Different from the former SDK CDC-only PID to avoid cached driver layouts.
// Allocate a production identity before distributing a finished USB product.
constexpr uint16_t kVid = 0xcafe;
constexpr uint16_t kPid = 0x4003;
constexpr uint16_t kConfigLength = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN;
static_assert(CFG_TUD_CDC == 1 && CFG_TUD_MSC == 1, "CDC + MSC configuration required");
static_assert(CFG_TUD_MSC_EP_BUFSIZE == 512, "Stage 1 needs only a single sector buffer");

const tusb_desc_device_t device = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200,
    TUSB_CLASS_MISC, MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD,
    CFG_TUD_ENDPOINT0_SIZE, kVid, kPid, 0x0080,
    1, 2, 3, 1
};

// USB full-speed packets are 64 bytes; the MSC software workspace is 512.
// Keep the SDK CDC endpoints unchanged. MSC owns a separate endpoint pair.
const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, kConfigLength, 0, 250),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_MSC_DESCRIPTOR(2, 5, 0x03, 0x83, 64),
};
static_assert(sizeof(configuration) == kConfigLength, "Descriptor length mismatch");

// Descriptor storage must outlive its control transfer. No heap allocation.
uint16_t string_buffer[33];
}

extern "C" {
const uint8_t* tud_descriptor_device_cb() {
    return reinterpret_cast<const uint8_t*>(&device);
}

const uint8_t* tud_descriptor_configuration_cb(uint8_t index) {
    return index == 0 ? configuration : nullptr;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t) {
    if (index == 0) {
        string_buffer[0] = (TUSB_DESC_STRING << 8) | 4;
        string_buffer[1] = 0x0409;
        return string_buffer;
    }
    char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    const char* text;
    switch (index) {
    case 1: text = "Cala Maclir"; break;
    case 2: text = "Cala's Pokecom BASIC"; break;
    case 3:
        pico_get_unique_board_id_string(serial, sizeof(serial));
        text = serial;
        break;
    case 4: text = "CPB Console"; break;
    case 5: text = "CPB SD Card"; break;
    default: return nullptr;
    }
    size_t length = 0;
    while (length < 32 && text[length]) {
        string_buffer[length + 1] = static_cast<uint8_t>(text[length]);
        ++length;
    }
    string_buffer[0] = (TUSB_DESC_STRING << 8) | (2 * length + 2);
    return string_buffer;
}
}
