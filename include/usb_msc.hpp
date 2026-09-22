#pragma once

namespace rmb::usb_msc {
// Descriptors always contain MSC. These queries describe media, not the
// interface. Media becomes present only after storage ownership handoff.
bool active();
bool media_present();

bool set_media_present(bool present);

// Latched on accepted host unload/eject, cleared at a new USB session.
bool host_ejected();

// Called only by the shared USB mount/unmount callbacks.
void reset_session();
void disconnected();
}
