/*
 * SomnoPal — EZShare HTTP adapter (spec §11)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Only three GET endpoints exist. Structured inputs only — no caller can
 * inject a URL, host, or traversal. One request in flight at a time;
 * the scheduler owns serialization.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ezs_parse.h"
#include "ezs_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EZW_RANGE_UNKNOWN = 0,  /* never probed                        */
    EZW_RANGE_UNSUPPORTED,/* got 200/416 for a Range request      */
    EZW_RANGE_VERIFIED,   /* real 206 + valid Content-Range seen   */
} ezw_range_cap_t;

/** Observation stamped by the scheduler after every request. */
typedef struct {
    int64_t  req_start_us;   /* esp_timer at request issue         */
    int64_t  resp_end_us;    /* esp_timer at body complete/fail    */
    int      http_status;    /* 200/206/4xx/5xx; 0 = transport err */
    esp_err_t transport;     /* esp_http_client result            */
    ezs_err_t parse;         /* parser result for listings         */
    uint32_t body_bytes;     /* bytes received                     */
} ezw_obs_t;

/** Configure the card host once per association (host-order IPv4). */
void ezs_client_set_host(uint32_t card_ip);

/**
 * GET /dir?dir=<canonical> — fully received + parsed.
 * entries must have EZS_ENTRIES_MAX capacity.
 * Returns ESP_OK when HTTP+parse both succeeded; check obs for detail.
 */
esp_err_t ezs_list_dir(const char *canon_dir,
                       ezs_entry_t *entries, int *n_out,
                       ezw_obs_t *obs);

/** One-shot card version probe: GET /client?command=version. */
esp_err_t ezs_client_version(char *buf, size_t cap, ezw_obs_t *obs);

/**
 * GET /download?fname&dir — streams the body through `sink`.
 * sink is called with response chunks; return false from sink to abort.
 * expected_size==0 means "accept Content-Length as truth".
 * Sets obs->body_bytes; rejects a length mismatching the expected tuple.
 */
typedef bool (*ezs_sink_t)(const uint8_t *data, size_t len, void *ctx);
esp_err_t ezs_download(const char *canon_dir, const char *name,
                       uint64_t expected_size,
                       ezs_sink_t sink, void *ctx, ezw_obs_t *obs);

/**
 * Bounded early-header probe (spec §8.2): bytes [0, len) of `name`.
 * Range is attempted only when ezw_range_cap()==VERIFIED; otherwise a
 * capped full GET (still counts toward the single-probe allowance).
 * Returns bytes written to buf via out_n.
 */
/**
 * Bounded early-header probe (spec §8.2): bytes [0, listed_size) of
 * `name`, with `listed_size` <= len <= 16 KiB (caller's §8.2 duty).
 * Range is attempted only when ezw_range_cap()==VERIFIED; otherwise a
 * full GET whose complete body must fit in buf (len).
 */
esp_err_t ezs_probe_header(const char *canon_dir, const char *name,
                           uint64_t listed_size,
                           uint8_t *buf, size_t len, size_t *out_n,
                           ezw_obs_t *obs);

/** Current card Range capability (§8.2). */
ezw_range_cap_t ezs_range_cap(void);
void ezs_range_cap_reset(void);   /* on association change          */

/** Called internally when a Range request behaves: VERIFIED. */
void ezs_range_cap_note(bool ok_206);

#ifdef __cplusplus
}
#endif
