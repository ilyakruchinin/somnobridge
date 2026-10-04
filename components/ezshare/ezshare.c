/*
 * SomnoBridge — EZShare HTTP adapter (spec §11)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <stdio.h>
#include <string.h>

#include "ezshare.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HTTP_CONNECT_TIMEOUT_MS 2000
#define HTTP_TOTAL_MS           5000   /* listings; GET uses its own   */
#define BODY_CHUNK_CAP          4096

static const char *TAG = "ezshare";

static uint32_t      s_card_ip;

/* ---------------- plumbing ---------------- */

void ezs_client_set_host(uint32_t card_ip) { s_card_ip = card_ip; }

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
                    "http://%lu.%lu.%lu.%lu/%.16s?%.16s=%.500s%s%.16s%s",
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

static esp_http_client_handle_t open_get(const char *url, bool is_file,
                                         ezw_obs_t *obs)
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
    /* DIAG: temporary instrumentation for the boot-time init crash   */
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    heap_caps_check_integrity_all(false);
    ESP_LOGI("ezs", "init cfg=%p url=%p:%.32s stack_hwm=%u heap=%u",
             (void *)&cfg, (void *)url, url,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)esp_get_free_heap_size());
#endif
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI("ezs", "init ok c=%p", (void *)c);
#endif
    if (!c) {
        obs->transport = ESP_FAIL;
        return NULL;
    }
    /* Request freshness; card may ignore — we never rely on it (§11.2). */
    esp_http_client_set_header(c, "Cache-Control", "no-cache");
    esp_http_client_set_header(c, "Pragma", "no-cache");
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        obs->transport = ESP_FAIL;
        return NULL;
    }
    /* esp_http_client_open() only *sends* the request: response->
     * status_code stays 0 until the response headers are read, and
     * esp_http_client_get_status_code() merely returns that field. Not
     * reading them here makes every request look like a transport
     * failure (status 0), so the card is never usable — the real status
     * is what §11.2 decisions are built on.                          */
    int64_t hdr = esp_http_client_fetch_headers(c);
    obs->http_status = esp_http_client_get_status_code(c);
    if (hdr < 0 || obs->http_status <= 0) {
        obs->transport = hdr < 0 ? ESP_FAIL : ESP_OK;
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

    char url[1152];
    build_url(url, sizeof(url), "dir", "dir", canon_dir, NULL, NULL);

    obs->req_start_us = esp_timer_get_time();
    esp_http_client_handle_t c = open_get(url, false, obs);
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
    if (obs->parse != EZS_OK) {
        /* Real card firmware varies; the body is the only way to see
         * what grammar was actually returned (metadata only, no data). */
        char snip[201];
        for (size_t base = 0; base < got; base += sizeof(snip) - 1) {
            size_t n = got - base < sizeof(snip) - 1 ? got - base
                                                     : sizeof(snip) - 1;
            for (size_t i = 0; i < n; i++) {
                unsigned char ch = (unsigned char)body[base + i];
                snip[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
            }
            snip[n] = '\0';
            ESP_LOGW(TAG, "body[%lu]: %s", (unsigned long)base, snip);
        }
        ESP_LOGW(TAG, "parse err=%d (%lu B)", obs->parse,
                 (unsigned long)got);
    }
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
    esp_http_client_handle_t c = open_get(url, false, obs);
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
                       const char *dl_name, uint64_t listed_size,
                       uint8_t size_unit_shift,
                       ezs_sink_t sink, void *ctx, ezw_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));
    obs->content_length = -1;
    if (!sink || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ezp_validate_name(name) != EZP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[1152];
    if (dl_name && dl_name[0]) {
        /* listing-derived 8.3 addressing: download?file=<dir>\<dl> —
         * the card canonicalizes long display names to 8.3 and only
         * this form resolves for them (U8)                         */
        if (ezp_validate_name(dl_name) != EZP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        char fpath[336];
        snprintf(fpath, sizeof(fpath), "%.320s\\%s",
                 canon_dir + 2, dl_name);      /* strip "A:\"         */
        build_url(url, sizeof(url), "download", "file", fpath,
                  NULL, NULL);
    } else {
        char fdir[320];
        canon_to_fdir(canon_dir, fdir, sizeof(fdir));
        build_url(url, sizeof(url), "download",
                  "fname", name, "fdir", fdir);
    }

    obs->req_start_us = esp_timer_get_time();
    esp_http_client_handle_t c = open_get(url, true, obs);
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
    obs->content_length = clen;
    if (listed_size && clen >= 0 &&
        !ezs_size_within_listing(listed_size, size_unit_shift,
                                 (uint64_t)clen)) {
        /* announced length outside the ceil-unit envelope -> the
         * catalog tuple no longer describes this file (U8)          */
        obs->resp_end_us = esp_timer_get_time();
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t chunk[BODY_CHUNK_CAP];
    uint64_t sent = 0;
    uint64_t bound = clen >= 0 ? (uint64_t)clen : listed_size;
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
        if (bound && sent + (uint64_t)r > bound) {
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
    if (clen >= 0) {
        if (sent != (uint64_t)clen) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else if (listed_size &&
               !ezs_size_within_listing(listed_size, size_unit_shift,
                                        sent)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
