/*
 * SomnoPal — EZShare HTTP adapter (spec §11)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <stdio.h>
#include <string.h>

#include "ezshare.h"

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HTTP_CONNECT_TIMEOUT_MS 2000
#define HTTP_TOTAL_MS           5000   /* listings; GET uses its own   */
#define BODY_CHUNK_CAP          4096

static uint32_t      s_card_ip;
static ezw_range_cap_t s_range = EZW_RANGE_UNKNOWN;

/* ---------------- plumbing ---------------- */

void ezs_client_set_host(uint32_t card_ip) { s_card_ip = card_ip; }
ezw_range_cap_t ezs_range_cap(void)        { return s_range; }
void ezs_range_cap_reset(void)             { s_range = EZW_RANGE_UNKNOWN; }
void ezs_range_cap_note(bool ok_206)
{
    s_range = ok_206 ? EZW_RANGE_VERIFIED : EZW_RANGE_UNSUPPORTED;
}

/* Build "http://a.b.c.d/dir?dir=A%3A%5C..." — caller passes canonical
 * inputs only; both path and query are produced here, never elsewhere. */
static int build_url(char *out, size_t cap, const char *endpoint,
                     const char *q1_name, const char *q1_val,
                     const char *q2_name, const char *q2_val)
{
    char e1[512], e2[512];
    ezp_urlencode(q1_val, e1, sizeof(e1));
    if (q2_val) {
        ezp_urlencode(q2_val, e2, sizeof(e2));
    }
    return snprintf(out, cap,
                    "http://%lu.%lu.%lu.%lu/%s?%s=%s%s%s%s",
                    (unsigned long)(s_card_ip >> 24),
                    (unsigned long)(s_card_ip >> 16) & 255,
                    (unsigned long)(s_card_ip >> 8) & 255,
                    (unsigned long)s_card_ip & 255,
                    endpoint, q1_name, e1,
                    q2_val ? "&" : "", q2_val ? q2_name : "",
                    q2_val ? e2 : "");
}

/* Map the canonical dir to the card's fdir spelling (spec §11.1):
 *   A:\            -> ".."
 *   A:\X\Y         -> "..\X\Y"
 */
static void canon_to_fdir(const char *canon, char *out, size_t cap)
{
    if (!strcmp(canon, "A:\\")) {
        snprintf(out, cap, "..");
    } else {
        snprintf(out, cap, "..%s", canon + 2);  /* keep \components   */
    }
}

static esp_http_client_handle_t open_get(const char *url, bool is_file)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = is_file ? 30000 : HTTP_TOTAL_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    /* Request freshness; card may ignore — we never rely on it (§11.2). */
    esp_http_client_set_header(c, "Cache-Control", "no-cache");
    esp_http_client_set_header(c, "Pragma", "no-cache");
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

/* ---------------- /dir ---------------- */

esp_err_t ezs_list_dir(const char *canon_dir,
                       ezs_entry_t *entries, int *n_out,
                       ezw_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));
    *n_out = 0;
    if (!entries || !canon_dir) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[600];
    build_url(url, sizeof(url), "dir", "dir", canon_dir, NULL, NULL);

    obs->req_start_us = esp_timer_get_time();
    esp_http_client_handle_t c = open_get(url, false);
    if (!c) {
        obs->resp_end_us = esp_timer_get_time();
        obs->transport = ESP_FAIL;
        return ESP_FAIL;
    }
    obs->http_status = esp_http_client_get_status_code(c);
    int64_t clen = esp_http_client_get_content_length(c);

    if (obs->http_status != 200) {
        obs->transport = ESP_OK;
        obs->resp_end_us = esp_timer_get_time();
        esp_http_client_cleanup(c);
        return ESP_ERR_HTTP_BASE + obs->http_status;
    }

    /* bounded body: <=192 KiB listing cap (spec §11.2) */
    const size_t cap = 192 * 1024;
    char *body = malloc(cap);
    if (!body) {
        esp_http_client_cleanup(c);
        obs->resp_end_us = esp_timer_get_time();
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    int r = 0;
    while ((r = esp_http_client_read(c, body + got,
                                     cap - 1 - got)) > 0) {
        got += r;
        if (got >= cap - 1) {
            break;                    /* over cap -> LIMIT below       */
        }
    }
    body[got] = '\0';
    obs->resp_end_us = esp_timer_get_time();
    obs->body_bytes = got;
    esp_http_client_cleanup(c);

    if (r < 0) {
        free(body);
        obs->transport = ESP_FAIL;
        return ESP_FAIL;
    }
    if (got >= cap - 1 || (clen > 0 && (int64_t)got != clen)) {
        free(body);
        obs->parse = EZS_ERR_LIMIT;
        return ESP_ERR_INVALID_SIZE;
    }
    obs->transport = ESP_OK;
    obs->parse = ezs_parse_dir(body, entries, n_out);
    free(body);
    return obs->parse == EZS_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ---------------- /client?command=version ---------------- */

esp_err_t ezs_client_version(char *buf, size_t cap, ezw_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));
    char url[160];
    snprintf(url, sizeof(url),
             "http://%lu.%lu.%lu.%lu/client?command=version",
             (unsigned long)(s_card_ip >> 24),
             (unsigned long)(s_card_ip >> 16) & 255,
             (unsigned long)(s_card_ip >> 8) & 255,
             (unsigned long)s_card_ip & 255);

    obs->req_start_us = esp_timer_get_time();
    esp_http_client_handle_t c = open_get(url, false);
    if (!c) {
        obs->resp_end_us = esp_timer_get_time();
        return ESP_FAIL;
    }
    obs->http_status = esp_http_client_get_status_code(c);
    int r = esp_http_client_read(c, buf, cap - 1);
    obs->resp_end_us = esp_timer_get_time();
    obs->transport = ESP_OK;
    if (r > 0) {
        buf[r] = '\0';
        obs->body_bytes = r;
    } else {
        buf[0] = '\0';
    }
    esp_http_client_cleanup(c);
    return obs->http_status == 200 ? ESP_OK : ESP_FAIL;
}

/* ---------------- /download ---------------- */

esp_err_t ezs_download(const char *canon_dir, const char *name,
                       uint64_t expected_size,
                       ezs_sink_t sink, void *ctx, ezw_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));
    if (!sink || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ezp_validate_name(name) != EZP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    char fdir[320], url[900];
    canon_to_fdir(canon_dir, fdir, sizeof(fdir));
    build_url(url, sizeof(url), "download",
              "fname", name, "fdir", fdir);

    obs->req_start_us = esp_timer_get_time();
    esp_http_client_handle_t c = open_get(url, true);
    if (!c) {
        obs->resp_end_us = esp_timer_get_time();
        return ESP_FAIL;
    }
    obs->http_status = esp_http_client_get_status_code(c);
    if (obs->http_status != 200) {
        obs->resp_end_us = esp_timer_get_time();
        esp_http_client_cleanup(c);
        return ESP_ERR_HTTP_BASE + obs->http_status;
    }
    int64_t clen = esp_http_client_get_content_length(c);
    if (expected_size && clen >= 0 && (uint64_t)clen != expected_size) {
        /* known Content-Length mismatch -> fail before any body byte */
        obs->resp_end_us = esp_timer_get_time();
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t chunk[BODY_CHUNK_CAP];
    uint64_t sent = 0;
    bool ok = true;
    for (;;) {
        int r = esp_http_client_read(c, (char *)chunk, sizeof(chunk));
        if (r < 0) {
            ok = false;
            break;
        }
        if (r == 0) {
            if (!esp_http_client_is_complete_data_received(c)) {
                ok = false;           /* premature EOF               */
            }
            break;
        }
        if (expected_size && sent + (uint64_t)r > expected_size) {
            ok = false;               /* grew mid-transfer           */
            break;
        }
        if (!sink(chunk, r, ctx)) {
            ok = false;
            break;                    /* client aborted              */
        }
        sent += r;
    }
    obs->resp_end_us = esp_timer_get_time();
    obs->body_bytes = sent;
    esp_http_client_cleanup(c);

    if (!ok) {
        return ESP_FAIL;
    }
    if (expected_size && sent != expected_size) {
        return ESP_ERR_INVALID_SIZE;  /* shorter than listed         */
    }
    if (!expected_size && clen >= 0 && sent != (uint64_t)clen) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ---------------- bounded header probe (§8.2) ---------------- */

struct probe_ctx {
    uint8_t *buf; size_t cap, got;
};
static bool probe_sink(const uint8_t *d, size_t n, void *v)
{
    struct probe_ctx *p = v;
    size_t room = p->cap - p->got;
    size_t take = n < room ? n : room;
    memcpy(p->buf + p->got, d, take);
    p->got += take;
    return p->got < p->cap;           /* stop early once capped      */
}

esp_err_t ezs_probe_header(const char *canon_dir, const char *name,
                           uint64_t listed_size,
                           uint8_t *buf, size_t len, size_t *out_n,
                           ezw_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));
    *out_n = 0;
    if (len == 0 || len > 16 * 1024 || listed_size > len) {
        return ESP_ERR_INVALID_ARG;   /* §8.2: only ≤16 KiB files    */
    }

    char fdir[320], url[900];
    canon_to_fdir(canon_dir, fdir, sizeof(fdir));
    build_url(url, sizeof(url), "download",
              "fname", name, "fdir", fdir);

    obs->req_start_us = esp_timer_get_time();

    if (s_range == EZW_RANGE_VERIFIED) {
        /* Range path: bytes=0-(len-1), must get real 206 */
        esp_http_client_config_t cfg = {
            .url = url, .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TOTAL_MS,
            .buffer_size = 2048, .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) return ESP_FAIL;
        char range_hdr[40];
        snprintf(range_hdr, sizeof(range_hdr),
                 "bytes=0-%u", (unsigned)(len - 1));
        esp_http_client_set_header(c, "Range", range_hdr);
        if (esp_http_client_open(c, 0) != ESP_OK) {
            esp_http_client_cleanup(c);
            obs->resp_end_us = esp_timer_get_time();
            return ESP_FAIL;
        }
        obs->http_status = esp_http_client_get_status_code(c);
        if (obs->http_status != 206) {
            /* regression -> disable Range, abort before body (§8.2) */
            s_range = EZW_RANGE_UNSUPPORTED;
            esp_http_client_cleanup(c);
            obs->resp_end_us = esp_timer_get_time();
            return ESP_ERR_HTTP_BASE + obs->http_status;
        }
        /* verify Content-Range loosely; full check in H4 bring-up   */
        int r;
        size_t got = 0;
        while (got < len &&
               (r = esp_http_client_read(c, (char *)buf + got,
                                         len - got)) > 0) {
            got += r;
        }
        obs->resp_end_us = esp_timer_get_time();
        obs->body_bytes = got;
        esp_http_client_cleanup(c);
        *out_n = got;
        return ESP_OK;
    }

    /* UNKNOWN/UNSUPPORTED: one capped full GET on a small candidate.
     * listed_size (≤16 KiB) is the exact-match bound — if the file grew
     * mid-transfer the GET fails and the probe is spent (§8.2). */
    struct probe_ctx p = { .buf = buf, .cap = len, .got = 0 };
    esp_err_t err = ezs_download(canon_dir, name, listed_size,
                                 probe_sink, &p, obs);
    *out_n = p.got;
    return err;
}
