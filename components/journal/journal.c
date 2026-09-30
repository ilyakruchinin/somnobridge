/*
 * SomnoPal — bounded evidence journal (spec §9)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <stdio.h>
#include <string.h>

#include "journal.h"

#include "esp_check.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "journal";

#define JRN_NS         "evidence"
#define JRN_VERSION    1
#define JRN_MAGIC      0x4A524E31u   /* "JRN1" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t seq;
    uint16_t len;
    uint16_t reserved2;
    uint32_t crc32;                 /* over payload only */
} jrn_hdr_t;                        /* 16 bytes */

static nvs_handle_t s_nvs;
static bool         s_open;
static jrn_health_t s_health = JRN_OFF;
static uint32_t     s_seq_next;
static uint32_t     s_seq_oldest;
static bool         s_truncated;
static bool         s_dirty;
static int64_t      s_batch_since_us;
static char         s_batch[JRN_BATCH_MAX];
static size_t       s_batch_len;
static uint8_t      s_next_slot;

static void slot_key(uint8_t slot, char out[4])
{
    out[0] = 'b';
    out[1] = '0' + (slot / 100) % 10;
    out[2] = '0' + (slot / 10) % 10;
    out[3] = '0' + slot % 10;
    /* NOTE: 4-char key; nvs key space includes NUL so "b255" is fine  */
}

/* Find which slot holds sequence `seq` (i.e. slot index used when seq
 * was written); slot rotation is seq % JRN_SLOTS chunks, but one write
 * may span several slots, so ordering uses seq, not slot index.        */
static void scan_slots(void)
{
    uint32_t best_lo = UINT32_MAX, best_hi = 0;
    bool any = false;
    for (int i = 0; i < JRN_SLOTS; i++) {
        char k[5];
        slot_key((uint8_t)i, k);
        k[4] = '\0';
        size_t need = 0;
        if (nvs_get_blob(s_nvs, k, NULL, &need) != ESP_OK || need < sizeof(jrn_hdr_t)) {
            continue;
        }
        uint8_t buf[sizeof(jrn_hdr_t)];
        if (nvs_get_blob(s_nvs, k, buf, &need) != ESP_OK) {
            continue;
        }
        jrn_hdr_t *h = (jrn_hdr_t *)buf;
        if (h->magic != JRN_MAGIC || h->version != JRN_VERSION ||
            h->len > JRN_PAYLOAD_MAX) {
            s_truncated = true;
            continue;
        }
        /* CRC is verified lazily on read to keep boot fast; structural
         * checks are enough to bound the sequence space.             */
        any = true;
        if (h->seq < best_lo) best_lo = h->seq;
        if (h->seq >= best_hi) best_hi = h->seq + 1;
    }
    if (any) {
        s_seq_oldest = best_lo;
        s_seq_next   = best_hi;
        /* resume rotation where the highest-seq chunk ended */
        s_next_slot  = 0;
        /* find the slot holding the highest seq to continue after it */
        uint32_t hi = 0;
        int hi_slot = -1;
        for (int i = 0; i < JRN_SLOTS; i++) {
            char k[5];
            slot_key((uint8_t)i, k);
            k[4] = '\0';
            size_t need = 0;
            if (nvs_get_blob(s_nvs, k, NULL, &need) != ESP_OK ||
                need < sizeof(jrn_hdr_t)) {
                continue;
            }
            jrn_hdr_t h;
            size_t want = sizeof(h);
            if (nvs_get_blob(s_nvs, k, &h, &want) == ESP_OK &&
                h.magic == JRN_MAGIC && h.seq >= hi) {
                hi = h.seq;
                hi_slot = i;
            }
        }
        if (hi_slot >= 0) {
            s_next_slot = (uint8_t)((hi_slot + 1) % JRN_SLOTS);
        }
    } else {
        s_seq_oldest = 0;
        s_seq_next   = 1;
        s_next_slot  = 0;
    }
}

extern void boot_mark(uint8_t);

esp_err_t jrn_init(void)
{
    boot_mark(0xB0);
    nvs_sec_cfg_t *sec = (nvs_sec_cfg_t *)settings_sec_cfg();
    if (!sec) {
        ESP_LOGE(TAG, "no NVS security config — journal off");
        s_health = JRN_OFF;
        return ESP_ERR_INVALID_STATE;
    }
    boot_mark(0xB1);
    esp_err_t err = nvs_flash_secure_init_partition("evidence", sec);
    boot_mark(0xB2);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* no auto-erase: report degraded and leave data alone        */
        ESP_LOGE(TAG, "evidence partition needs erase — refusing");
        s_health = JRN_OFF;
        return err;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "evidence partition init");
    err = nvs_open_from_partition("evidence", JRN_NS, NVS_READWRITE, &s_nvs);
    boot_mark(0xB3);
    ESP_RETURN_ON_ERROR(err, TAG, "evidence ns open");
    s_open = true;
    s_health = JRN_OK;
    scan_slots();
    boot_mark(0xB4);
    s_batch_len = 0;
    s_dirty = false;
    s_batch_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "journal: seq_next=%lu oldest=%lu slot=%u%s",
             (unsigned long)s_seq_next, (unsigned long)s_seq_oldest,
             s_next_slot, s_truncated ? " (gaps)" : "");
    boot_mark(0xB5);
    return ESP_OK;
}

esp_err_t jrn_put(const char *kind, const char *json_body)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (!kind || !json_body) return ESP_ERR_INVALID_ARG;
    int need = snprintf(NULL, 0, "{\"k\":\"%s\",\"t\":%lld,%s}\n",
                        kind, (long long)(esp_timer_get_time() / 1000),
                        json_body);
    if (need <= 0 || need > JRN_LINE_MAX) {
        s_truncated = true;
        s_health = JRN_TRUNCATED;
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_batch_len + (size_t)need > sizeof(s_batch)) {
        /* batch full: commit first; failure means truncation         */
        if (jrn_commit() != ESP_OK) {
            s_truncated = true;
            s_health = JRN_TRUNCATED;
            return ESP_ERR_NO_MEM;
        }
        if (s_batch_len + (size_t)need > sizeof(s_batch)) {
            s_truncated = true;
            s_health = JRN_TRUNCATED;
            return ESP_ERR_NO_MEM;
        }
    }
    int w = snprintf(s_batch + s_batch_len, sizeof(s_batch) - s_batch_len,
                     "{\"k\":\"%s\",\"t\":%lld,%s}\n",
                     kind, (long long)(esp_timer_get_time() / 1000),
                     json_body);
    if (w > 0) {
        s_batch_len += (size_t)w;
        s_dirty = true;
    }
    return ESP_OK;
}

static esp_err_t write_chunk(const uint8_t *data, size_t len)
{
    if (len > JRN_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;
    jrn_hdr_t h = {
        .magic = JRN_MAGIC, .version = JRN_VERSION,
        .seq = s_seq_next, .len = (uint16_t)len,
        .crc32 = esp_crc32_le(0, data, len),
    };
    static uint8_t buf[sizeof(jrn_hdr_t) + JRN_PAYLOAD_MAX];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);
    char k[5];
    slot_key(s_next_slot, k);
    k[4] = '\0';
    esp_err_t err = nvs_set_blob(s_nvs, k, buf, sizeof(h) + len);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        s_next_slot = (uint8_t)((s_next_slot + 1) % JRN_SLOTS);
        s_seq_next++;
        if (s_seq_next - s_seq_oldest > JRN_SLOTS) {
            s_seq_oldest = s_seq_next - JRN_SLOTS;
        }
    }
    return err;
}

esp_err_t jrn_commit(void)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (!s_dirty) { s_batch_since_us = esp_timer_get_time(); return ESP_OK; }
    size_t off = 0;
    esp_err_t first_err = ESP_OK;
    while (off < s_batch_len) {
        size_t n = s_batch_len - off;
        if (n > JRN_PAYLOAD_MAX) {
            /* split at a line boundary so every chunk is valid lines */
            size_t cut = JRN_PAYLOAD_MAX;
            while (cut > 0 && s_batch[off + cut - 1] != '\n') cut--;
            n = cut ? cut : JRN_PAYLOAD_MAX;
        }
        esp_err_t err = write_chunk((const uint8_t *)s_batch + off, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "chunk write failed: %s", esp_err_to_name(err));
            first_err = err;
            s_health = JRN_TRUNCATED;
            s_truncated = true;
            break;                 /* keep remainder for next attempt */
        }
        off += n;
    }
    if (off > 0) {
        memmove(s_batch, s_batch + off, s_batch_len - off);
        s_batch_len -= off;
    }
    s_dirty = s_batch_len > 0;
    s_batch_since_us = esp_timer_get_time();
    return first_err;
}

void jrn_tick(int64_t now_us)
{
    if (s_dirty && now_us - s_batch_since_us >= (int64_t)JRN_COMMIT_MS * 1000) {
        jrn_commit();
    }
}

esp_err_t jrn_erase_all(void)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_erase_all(s_nvs);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) return err;
    s_seq_next = 1;
    s_seq_oldest = 0;
    s_next_slot = 0;
    s_truncated = false;
    s_batch_len = 0;
    s_dirty = false;
    s_batch_since_us = esp_timer_get_time();
    jrn_put("event", "\"e\":\"evidence_cleared\"");
    return jrn_commit();
}

jrn_health_t jrn_health(void)    { return s_health; }
uint32_t   jrn_seq(void)         { return s_seq_next; }
uint32_t   jrn_oldest_seq(void)  { return s_seq_oldest; }
bool       jrn_truncated(void)   { return s_truncated; }

/* ---------------- replay (GET_EVIDENCE) ---------------- */

typedef struct { uint32_t seq; uint8_t slot; } jrn_idx_t;
static uint8_t s_slot_buf[sizeof(jrn_hdr_t) + JRN_PAYLOAD_MAX];

esp_err_t jrn_walk(jrn_walk_cb_t cb, void *ctx, uint32_t *gaps_out)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (!cb)     return ESP_ERR_INVALID_ARG;
    if (gaps_out) *gaps_out = 0;

    /* one pass: index valid slots by seq (CRC checked on emit).
     * nvs_get_blob reads whole blobs — pull each into the scratch.   */
    jrn_idx_t idx[JRN_SLOTS];
    int n_idx = 0;
    for (int i = 0; i < JRN_SLOTS; i++) {
        char k[5];
        slot_key((uint8_t)i, k);
        k[4] = '\0';
        size_t want = sizeof(s_slot_buf);
        if (nvs_get_blob(s_nvs, k, s_slot_buf, &want) != ESP_OK ||
            want < sizeof(jrn_hdr_t)) {
            continue;
        }
        jrn_hdr_t *h = (jrn_hdr_t *)s_slot_buf;
        if (h->magic != JRN_MAGIC || h->version != JRN_VERSION ||
            h->len > JRN_PAYLOAD_MAX || want != sizeof(*h) + h->len) {
            continue;                       /* damaged slot: a gap    */
        }
        idx[n_idx++] = (jrn_idx_t){ .seq = h->seq, .slot = (uint8_t)i };
    }
    /* insertion-sort by seq (≤256 entries, evidence reads are rare)  */
    for (int i = 1; i < n_idx; i++) {
        jrn_idx_t t = idx[i];
        int j = i - 1;
        while (j >= 0 && idx[j].seq > t.seq) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = t;
    }
    uint32_t expect = n_idx ? idx[0].seq : 0;
    for (int i = 0; i < n_idx; i++) {
        if (gaps_out && idx[i].seq != expect) {
            *gaps_out += idx[i].seq - expect;
        }
        expect = idx[i].seq + 1;
        char k[5];
        slot_key(idx[i].slot, k);
        k[4] = '\0';
        size_t want = sizeof(s_slot_buf);
        if (nvs_get_blob(s_nvs, k, s_slot_buf, &want) != ESP_OK ||
            want < sizeof(jrn_hdr_t)) {
            if (gaps_out) (*gaps_out)++;
            continue;
        }
        jrn_hdr_t *h = (jrn_hdr_t *)s_slot_buf;
        if (h->magic != JRN_MAGIC || h->version != JRN_VERSION ||
            want != sizeof(*h) + h->len ||
            esp_crc32_le(0, s_slot_buf + sizeof(*h), h->len) != h->crc32) {
            if (gaps_out) (*gaps_out)++;   /* torn/corrupt → counted  */
            continue;
        }
        if (!cb((const char *)s_slot_buf + sizeof(*h), h->len,
                h->seq, ctx)) {
            return ESP_OK;                 /* caller stopped early   */
        }
    }
    return ESP_OK;
}
