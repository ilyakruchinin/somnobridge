/*
 * SomnoBridge — block stream codec (update7 §10.5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Raw file bytes are encoded into a self-describing block stream before
 * being fragmented onto the Data characteristic. Each block carries its
 * own codec tag so the receiver decodes without side-band metadata and
 * any block that does not compress falls back to raw bytes.
 *
 * Wire layout (little-endian):
 *   block := u8 codec | u8 ver(=1) | u16 rsv | u32 raw_len | u32 enc_len
 *            | enc[enc_len]
 * codec: 0 = raw  |  1 = deflate(raw)  |  2 = deflate(delta16(raw))
 * delta16: LE uint16 samples replaced by (x[i] - x[i-1]) mod 2^16; an
 * odd trailing byte is passed through unchanged.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STC_BLOCK_HDR_LEN  12
#define STC_BLOCK_MAX      (64 * 1024)      /* raw bytes per block     */
#define STC_WIRE_VER       1

typedef enum {
    STC_RAW            = 0,
    STC_DEFLATE        = 1,
    STC_DELTA16_DEFLATE= 2,
} stc_codec_t;

/* emitted by the codec into the framing layer; returning false aborts */
typedef bool (*stc_emit_fn)(const uint8_t *d, size_t n, void *ctx);

/* Allocates staging buffers (PSRAM). When allocation fails the codec
 * degrades to a raw passthrough — the wire format stays identical.  */
int  stc_begin(void);
/* Feed raw bytes. May emit one or more complete blocks via `emit`.   */
bool stc_feed(const uint8_t *d, size_t n, stc_emit_fn emit, void *ctx);
/* Encode + emit the trailing partial block (and empty stream marker
 * when nothing was ever fed — a zero-length file still gets a block). */
bool stc_flush(stc_emit_fn emit, void *ctx);
void stc_end(void);

/* diagnostics: compressed bytes produced / µs spent in deflate        */
uint64_t stc_wire_bytes(void);
uint32_t stc_encode_ms(void);
uint32_t stc_raw_blocks(void);    /* count of blocks that fell back    */
uint32_t stc_def_blocks(void);    /* blocks using codec 1              */
uint32_t stc_d16_blocks(void);    /* blocks using codec 2              */

/* pure helpers (host-testable) */
void   stc_delta16(const uint8_t *in, uint8_t *out, size_t n);
void   stc_inv_delta16(const uint8_t *in, uint8_t *out, size_t n);
/* write one block envelope: header + payload. Returns 0 on overflow. */
size_t stc_pack_block(uint8_t *out, size_t cap, uint8_t codec,
                      const uint8_t *enc, uint32_t enc_len,
                      uint32_t raw_len);

#ifdef __cplusplus
}
#endif
