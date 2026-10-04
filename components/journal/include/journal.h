/*
 * SomnoBridge — bounded evidence journal (spec §9)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Append-only JSON-lines evidence in a dedicated encrypted NVS partition:
 * 256 rotating blob slots ("b000".."b255"), each <=3072 bytes payload,
 * with version/seq/len/CRC32 framing. A RAM batch is committed when full
 * or every 60 s — power loss may lose the tail and that gap is reported.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JRN_SLOTS        256
#define JRN_PAYLOAD_MAX  3072
#define JRN_BATCH_MAX    8192
#define JRN_LINE_MAX     512   /* one JSON record, without newline      */
#define JRN_COMMIT_MS    60000

typedef enum {
    JRN_OK = 0,
    JRN_TRUNCATED,      /* some records dropped: queue/flash failure    */
    JRN_DEGRADED,       /* init/scan problems; writes still attempted   */
    JRN_OFF,            /* partition unusable                           */
} jrn_health_t;

/* Open the evidence partition, scan slots, continue the sequence. Never
 * auto-erases: unrecoverable partition means JRN_OFF, not data loss.    */
esp_err_t jrn_init(void);

/* Queue one compact JSON record (kind is a short tag like "poll"). The
 * record lands in the RAM batch; it is not durable until jrn_commit.   */
esp_err_t jrn_put(const char *kind, const char *json_body);

/* Flush the RAM batch to the next rotating slot(s). Safe to call often. */
esp_err_t jrn_commit(void);

/* Periodic service: commit when the batch aged past JRN_COMMIT_MS.      */
void      jrn_tick(int64_t now_us);

/* CLEAR_EVIDENCE action (§4.1): erase every slot + reset sequence.
 * Never touches config/metadata — only the evidence namespace.        */
esp_err_t jrn_erase_all(void);

jrn_health_t jrn_health(void);
uint32_t   jrn_seq(void);            /* next sequence to be written     */
uint32_t   jrn_oldest_seq(void);     /* oldest retained (0 = unknown)   */
bool       jrn_truncated(void);      /* a gap/overflow was observed     */

/* Replay retained payloads oldest->newest, CRC-verified per slot.
 * cb returning false aborts with ESP_FAIL. Skipped/overwritten slots report
 * via `gaps_out` (may be NULL).                                      */
typedef bool (*jrn_walk_cb_t)(const char *payload, uint16_t len,
                              uint32_t seq, void *ctx);
esp_err_t jrn_walk(jrn_walk_cb_t cb, void *ctx, uint32_t *gaps_out);

#ifdef __cplusplus
}
#endif
