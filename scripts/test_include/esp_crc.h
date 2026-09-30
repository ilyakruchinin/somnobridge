/* host-test stub — real CRC32/LE (table-driven, matches esp_rom) */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data,
                                    size_t len)
{
    uint32_t c = ~crc;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(c & 1));
    }
    return ~c;
}
