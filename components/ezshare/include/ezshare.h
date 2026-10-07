/*
 * SomnoBridge — EZShare HTTP adapter (spec §11)
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

/** Observation stamped by the scheduler after every request. */
typedef struct {
    int64_t  req_start_us;   /* esp_timer at request issue         */
    int64_t  resp_end_us;    /* esp_timer at body complete/fail    */
    int      http_status;    /* 200/206/4xx/5xx; 0 = transport err */
    esp_err_t transport;     /* esp_http_client result            */
    ezs_err_t parse;         /* parser result for listings         */
    uint32_t body_bytes;     /* bytes received                     */
    int64_t  content_length; /* exact announced length, -1 unknown  */
    int64_t  header_us;
    int64_t  read_us;
    int64_t  sink_us;
    uint32_t read_calls;
    uint8_t  download_form;
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
 * GET /download — streams the body through `sink`.
 * sink is called with response chunks; return false from sink to abort.
 *
 * Addressing: the card emits 8.3-style abbreviations in its ?file=
 * listing hrefs (e.g. 202610~1.EDF) but those tokens do not resolve —
 * `dl_name` is retained for diagnostics only. The request always uses
 * the long name: `download?file=<dir>\<name>` first, then
 * `download?fname=<name>&fdir=<dir>` as fallback (U8).
 *
 * Size gate: `listed_size` is the listing's display-rounded size and
 * `size_unit_shift` its unit (ezs_entry_t). The exact Content-Length
 * must fall inside the rounding envelope — exact <= listed <
 * exact+unit (ezs_size_within_listing). listed_size==0 means "accept
 * Content-Length as truth". Sets obs->body_bytes and
 * obs->content_length; any mismatch fails closed before the first
 * body byte.
 */
typedef bool (*ezs_sink_t)(const uint8_t *data, size_t len, void *ctx);
esp_err_t ezs_download(const char *canon_dir, const char *name,
                       const char *dl_name, uint64_t listed_size,
                       uint8_t size_unit_shift, uint8_t first_form,
                       ezs_sink_t sink, void *ctx, ezw_obs_t *obs);

#ifdef __cplusplus
}
#endif
