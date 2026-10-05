#include "network.hpp"
namespace rmb::network {
bool radio_ready() { return true; }
bool operation_active() { return false; }
OperationState operation_state() { return OperationState::Failed; }
void operation_poll() {}
void operation_cancel() {}
bool scan_start() { return false; }
int scan_result(AccessPoint*, int) { return -1; }
int scan_observed_count() { return 0; }
bool connect_start(const char*, const char*) { return false; }
bool ntp_start(int, const char*) { return false; }
bool ntp_result(NetworkDateTime&) { return false; }
}
