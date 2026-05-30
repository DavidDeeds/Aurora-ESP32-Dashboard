#pragma once

#include <stddef.h>
#include <stdint.h>

/** Decode Aurora opcode-50 style payload bytes response[1..5] (CRC at [6],[7]). */
void aurora_decode_inverter_status(const uint8_t response[8], char *global, size_t glen, char *inverter, size_t ilen,
                                   char *chn1, size_t c1len, char *chn2, size_t c2len, char *alarm, size_t alen);
