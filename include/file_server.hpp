#pragma once

#include <cstddef>

namespace rmb::network {

bool file_server_start();
void file_server_stop();
void file_server_poll();
bool file_server_running();
const char* file_server_token();
const char* file_server_last_error();

} // namespace rmb::network
