/*
 * SomnoBridge — block stream codec (update7 §10.5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Buffers raw bytes into 64 KiB blocks, picks the smallest encoding per
 * block {raw, deflate(raw), deflate(delta16)} and emits the framed block
 * stream. Deflate uses the ESP32-S3 ROM miniz (tdefl) — no new library.
 * Host builds stub deflate out so the block framing and the delta16
 * round-trip stay unit-testable.
 */
#include "stream_codec.h"

#include <string.h>

#if defined(STC_HOST_TEST)
#include "esp_err.h"        /* stub header: ESP_OK */
#else
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "stc";
#endif

#define STC_DEFLATE_PROBES  32     /* low 15 bits of tdefl flags       */
#define STC_ENC_CAP         (STC_BLOCK_MAX + 8 * 1024)

/* ---------------- deflate backend ---------------- */

#if !defined(STC_HOST_TEST)
/* tdefl's compressor state is ~300 KiB — the mem_to_mem helper mallocs
 * it internally, which always fails under our internal-RAM budget.
 * The low-level API does no heap allocation: keep the state in PSRAM
 * and stream output through a put_buf callback instead.             */
static tdefl_compressor *s_comp;

typedef struct {
    uint8_t *dst;
    size_t   cap, pos;
    bool     ovf;
} stc_sink_ctx_t;

static mz_bool stc_put_buf(const void *pBuf, int len, void *pUser)
{
    stc_sink_ctx_t *c = pUser;
    if (c->ovf || (size_t)len > c->cap - c->pos) {
        c->ovf = true;
        return false;
    }
    memcpy(c->dst + c->pos, pBuf, len);
    c->pos += len;
    return true;
}
#endif

static size_t stc_deflate(uint8_t *dst, size_t cap,
                          const uint8_t *src, size_t n)
{
#if defined(STC_HOST_TEST)
    (void)dst; (void)cap; (void)src; (void)n;
    return 0;               /* host tests exercise raw blocks only    */
#else
    if (!cap || !s_comp) {
        return 0;
    }
    /* raw deflate stream (no zlib header — receiver inflates wbits
     * =-15)                                                           */
    stc_sink_ctx_t c = { .dst = dst, .cap = cap, .pos = 0,
                         .ovf = false };
    if (tdefl_init(s_comp, stc_put_buf, &c,
                   TDEFL_GREEDY_PARSING_FLAG | STC_DEFLATE_PROBES)
            != TDEFL_STATUS_OKAY ||
        tdefl_compress_buffer(s_comp, src, n, TDEFL_FINISH)
            != TDEFL_STATUS_DONE || c.ovf) {
        return 0;
    }
    return c.pos;
#endif
}

/* ---------------- pure helpers ---------------- */

void stc_delta16(const uint8_t *in, uint8_t *out, size_t n)
{
    /* samples are little-endian; delta wraps mod 2^16 so the inverse
     * is a plain running sum                                          */
    size_t m = n & ~(size_t)1;
    uint16_t prev = 0;
    for (size_t i = 0; i < m; i += 2) {
        uint16_t v = (uint16_t)(in[i] | (in[i + 1] << 8));
        uint16_t d = (uint16_t)(v - prev);
        out[i]     = (uint8_t)(d & 0xff);
        out[i + 1] = (uint8_t)(d >> 8);
        prev = v;
    }
    if (n & 1) {
        out[n - 1] = in[n - 1];
    }
}

void stc_inv_delta16(const uint8_t *in, uint8_t *out, size_t n)
{
    size_t m = n & ~(size_t)1;
    uint16_t acc = 0;
    for (size_t i = 0; i < m; i += 2) {
        acc = (uint16_t)(acc + (in[i] | (in[i + 1] << 8)));
        out[i]     = (uint8_t)(acc & 0xff);
        out[i + 1] = (uint8_t)(acc >> 8);
    }
    if (n & 1) {
        out[n - 1] = in[n - 1];
    }
}

size_t stc_pack_block(uint8_t *out, size_t cap, uint8_t codec,
                      const uint8_t *enc, uint32_t enc_len,
                      uint32_t raw_len)
{
    size_t need = STC_BLOCK_HDR_LEN + (enc ? enc_len : 0);
    if (cap < need) {
        return 0;
    }
    out[0] = codec;
    out[1] = STC_WIRE_VER;
    out[2] = out[3] = 0;
    out[4] = (uint8_t)(raw_len);
    out[5] = (uint8_t)(raw_len >> 8);
    out[6] = (uint8_t)(raw_len >> 16);
    out[7] = (uint8_t)(raw_len >> 24);
    out[8]  = (uint8_t)(enc_len);
    out[9]  = (uint8_t)(enc_len >> 8);
    out[10] = (uint8_t)(enc_len >> 16);
    out[11] = (uint8_t)(enc_len >> 24);
    if (enc && enc_len) {
        memcpy(out + STC_BLOCK_HDR_LEN, enc, enc_len);
    }
    return need;
}

/* ---------------- encoder state ---------------- */

static uint8_t *s_raw;      /* accumulation buffer, 64 KiB            */
static size_t   s_raw_len;
static uint8_t *s_xform;    /* delta16 working buffer, 64 KiB         */
static uint8_t *s_enc_a;    /* deflate(delta16) output                */
static uint8_t *s_enc_b;    /* deflate(raw) output                    */
static bool     s_zip;      /* deflate usable this stream             */

static uint64_t s_wire_bytes;
static uint32_t s_encode_ms;
static uint32_t s_n_raw, s_n_def, s_n_d16;
static uint32_t s_raw_streak;   /* consecutive raw wins — deflate
                                   backoff for incompressible data  */
static uint32_t s_blk_idx;

uint64_t stc_wire_bytes(void) { return s_wire_bytes; }
uint32_t stc_encode_ms(void)  { return s_encode_ms; }
uint32_t stc_raw_blocks(void) { return s_n_raw; }
uint32_t stc_def_blocks(void) { return s_n_def; }
uint32_t stc_d16_blocks(void) { return s_n_d16; }

static bool emit_block(uint8_t codec, const uint8_t *enc,
                       uint32_t enc_len, uint32_t raw_len,
                       stc_emit_fn emit, void *ctx)
{
    uint8_t hdr[STC_BLOCK_HDR_LEN];
    if (!stc_pack_block(hdr, sizeof(hdr), codec,
                        NULL, enc_len, raw_len)) {
        return false;
    }
    if (!emit(hdr, STC_BLOCK_HDR_LEN, ctx)) {
        return false;
    }
    if (enc_len && !emit(enc, enc_len, ctx)) {
        return false;
    }
    s_wire_bytes += STC_BLOCK_HDR_LEN + enc_len;
    return true;
}

static bool encode_full(const uint8_t *raw, size_t n,
                        stc_emit_fn emit, void *ctx)
{
    if (!n) {
        return emit_block(STC_RAW, NULL, 0, 0, emit, ctx);
    }
    size_t d16 = 0, def = 0;
#if !defined(STC_HOST_TEST)
    int64_t t0 = esp_timer_get_time();
#endif
    if (s_zip && (s_raw_streak < 8 || (s_blk_idx & 7) == 0)) {
        /* delta16 first — on this corpus it wins for nearly all bytes;
         * deflate(raw) is tried too so non-signal payloads still pick
         * the better of the two. After 8 straight raw wins, deflate is
         * retried only every 4th block — incompressible regions would
         * otherwise burn ~300 ms/block of CPU for nothing.            */
        stc_delta16(raw, s_xform, n);
        d16 = stc_deflate(s_enc_a, s_enc_a ? STC_ENC_CAP : 0,
                          s_xform, n);
        def = stc_deflate(s_enc_b, s_enc_b ? STC_ENC_CAP : 0,
                          raw, n);
    }
    s_blk_idx++;
#if !defined(STC_HOST_TEST)
    s_encode_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
#endif
    uint8_t      codec = STC_RAW;
    const uint8_t *enc = raw;
    uint32_t     elen = (uint32_t)n;
    if (d16 && d16 < n && (!def || d16 <= def)) {
        codec = STC_DELTA16_DEFLATE; enc = s_enc_a; elen = d16;
        s_n_d16++; s_raw_streak = 0;
    } else if (def && def < n) {
        codec = STC_DEFLATE; enc = s_enc_b; elen = def;
        s_n_def++; s_raw_streak = 0;
    } else {
        s_n_raw++; s_raw_streak++;
    }
#if !defined(STC_HOST_TEST)
    ESP_LOGI(TAG, "blk n=%u d16=%u def=%u -> codec=%u",
             (unsigned)n, (unsigned)d16, (unsigned)def, codec);
#endif
    return emit_block(codec, enc, elen, (uint32_t)n, emit, ctx);
}

int stc_begin(void)
{
    s_n_raw = s_n_def = s_n_d16 = 0;
    s_raw_streak = s_blk_idx = 0;
    s_wire_bytes = s_encode_ms = 0;
    s_raw_len = 0;
    s_zip = false;
#if defined(STC_HOST_TEST)
    s_raw = s_xform = s_enc_a = s_enc_b = NULL;
    (void)s_raw; (void)s_xform;
    return 0;               /* raw passthrough in host builds         */
#else
    s_raw   = heap_caps_malloc(STC_BLOCK_MAX, MALLOC_CAP_SPIRAM);
    s_xform = heap_caps_malloc(STC_BLOCK_MAX, MALLOC_CAP_SPIRAM);
    s_enc_a = heap_caps_malloc(STC_ENC_CAP,   MALLOC_CAP_SPIRAM);
    s_enc_b = heap_caps_malloc(STC_ENC_CAP,   MALLOC_CAP_SPIRAM);
    s_comp  = heap_caps_malloc(sizeof(tdefl_compressor),
                               MALLOC_CAP_SPIRAM);
    s_zip = s_raw && s_xform && s_enc_a && s_enc_b && s_comp;
    ESP_LOGI(TAG, "begin: zip=%d psram_free=%u",
             (int)s_zip,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!s_zip) {
        /* raw passthrough needs no staging buffers */
        heap_caps_free(s_raw);   s_raw   = NULL;
        heap_caps_free(s_xform); s_xform = NULL;
        heap_caps_free(s_enc_a); s_enc_a = NULL;
        heap_caps_free(s_enc_b); s_enc_b = NULL;
        heap_caps_free(s_comp);  s_comp  = NULL;
        ESP_LOGW(TAG, "codec buffers unavailable — raw stream");
    }
    return 0;               /* raw passthrough is always available    */
#endif
}

bool stc_feed(const uint8_t *d, size_t n, stc_emit_fn emit, void *ctx)
{
    if (!s_zip) {
        /* no staging buffers: emit input verbatim as raw blocks      */
        while (n) {
            size_t take = n > STC_BLOCK_MAX ? STC_BLOCK_MAX : n;
            if (!emit_block(STC_RAW, d, (uint32_t)take,
                            (uint32_t)take, emit, ctx)) {
                return false;
            }
            d += take; n -= take;
        }
        return true;
    }
    while (n) {
        size_t room = STC_BLOCK_MAX - s_raw_len;
        size_t take = n < room ? n : room;
        memcpy(s_raw + s_raw_len, d, take);
        s_raw_len += take;
        d += take; n -= take;
        if (s_raw_len == STC_BLOCK_MAX) {
            if (!encode_full(s_raw, s_raw_len, emit, ctx)) {
                return false;
            }
            s_raw_len = 0;
        }
    }
    return true;
}

bool stc_flush(stc_emit_fn emit, void *ctx)
{
    if (!s_zip) {
        return true;        /* raw path emits eagerly                 */
    }
    if (!encode_full(s_raw, s_raw_len, emit, ctx)) {
        return false;
    }
    s_raw_len = 0;
    return true;
}

void stc_end(void)
{
#if !defined(STC_HOST_TEST)
    heap_caps_free(s_raw);   s_raw   = NULL;
    heap_caps_free(s_xform); s_xform = NULL;
    heap_caps_free(s_enc_a); s_enc_a = NULL;
    heap_caps_free(s_enc_b); s_enc_b = NULL;
    heap_caps_free(s_comp);  s_comp  = NULL;
#endif
    s_zip = false;
    s_raw_len = 0;
}
