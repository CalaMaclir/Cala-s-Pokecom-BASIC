#include "usb_device.hpp"
#include "usb_msc.hpp"
#include "tusb.h"

// Do not call tusb_init/tud_task here: pico_stdio_usb performs both, including
// while BASIC, menus or serial transfers block the main loop. CDC callbacks
// and the stdio driver remain SDK-owned.
namespace rmb::usb_device {
bool usb_connected() { return tud_mounted(); }
}

extern "C" void tud_mount_cb() { rmb::usb_msc::reset_session(); }
extern "C" void tud_umount_cb() { rmb::usb_msc::disconnected(); }
