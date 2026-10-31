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
 * Raw GET of an arbitrary card path (diagnostics / admin pages).
 * `path` must start with '/'. Body is read into `buf` (NUL-terminated,
 * truncated at cap-1); the HTTP status is returned via `status_out`.
 * Returns ESP_OK when the transport succeeded (any HTTP status).
 */
esp_err_t ezs_get_raw(const char *path, char *buf, size_t cap,
                      int *status_out, ezw_obs_t *obs);

/** Same as ezs_get_raw plus an optional `Range: bytes=<from>-` request
 *  header (range_from <= 0 disables it) — lets a bounded diagnostic
 *  buffer page through files larger than the buffer.                 */
esp_err_t ezs_get_raw_range(const char *path, long range_from,
                            char *buf, size_t cap,
                            int *status_out, ezw_obs_t *obs);

/**
 * Change the card's own Wi-Fi password through its admin form
 * (captured live from the real card UI — config.js):
 *   1. GET /publicdir/sendurl?client=1&password=<admin> — marks this
 *      client IP as admin; <clientLogin>1</clientLogin> = ok,
 *      -2 = another admin session is online.
 *   2. GET /config?password=<admin>&back_href=&vtype=1 — the browser's
 *      login-form navigation; arms the write session and returns the
 *      config page (hidden oldAuth/oldNick/oldChannel inputs echo the
 *      current values, which are sent back unchanged).
 *   3. GET /config?auth=..&ssid=..&pass=..&channel=.. — stages the
 *      config; replies "config_success" but applies NOTHING by itself.
 *   4. GET /config?reset=1 — commits; the card reboots its AP and the
 *      connection dropping mid-read is expected.
 * Requires the bridge to be joined already (s_card_ip set).
 */
typedef enum {
    EZS_ADMIN_OK = 0,       /* submitted — AP will restart            */
    EZS_ADMIN_ARG,          /* bad arguments / not joined             */
    EZS_ADMIN_TRANSPORT,    /* card unreachable / read failure        */
    EZS_ADMIN_AUTH,         /* admin password rejected                */
    EZS_ADMIN_BUSY,         /* another admin session is online        */
    EZS_ADMIN_REJECTED      /* config submit failed                   */
} ezs_admin_err_t;
ezs_admin_err_t ezs_admin_change_wifi_pass(const char *admin_pass,
                                           const char *ssid,
                                           const char *new_wifi_pass);

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
