#pragma once

namespace rmb::usb_device {
// Configured by the host, not merely cable-present and not CDC DTR/open.
// USB init and tud_task remain owned by Pico SDK pico_stdio_usb.
bool usb_connected();
}
