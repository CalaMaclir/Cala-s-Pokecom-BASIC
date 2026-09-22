#pragma once
#include <cstddef>
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
void pico_get_unique_board_id_string(char* buffer, size_t length);
