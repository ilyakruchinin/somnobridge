/*
 * SomnoPal — anchors, mapping, session accumulation (spec §8.1/§8.3)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <stdio.h>
#include <string.h>

#include "drift.h"

#ifdef ESP_PLATFORM
#include "esp_crc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "settings.h"
#endif

/* ---------------- anchors ---------------- */

static drift_anchor_t  s_anchors[DRIFT_ANCHORS_MAX];
static int             s_nanchors;
static uint32_t        s_cur_epoch;
static bool            s_epoch_set;
static drift_pending_t s_pend[5];   /* §10.4: cache last five          */

void drift_reset(void)
{
    s_nanchors = 0;
    s_epoch_set = false;
    memset(s_pend, 0, sizeof(s_pend));
}

static drift_pending_t *pend_find(uint32_t rid)
{
    for (int i = 0; i < 5; i++) {
        if (s_pend[i].used && s_pend[i].request_id == rid) return &s_pend[i];
    }
    return NULL;
}

esp_err_t drift_time_begin(uint32_t request_id, uint64_t t1_utc_us,
                           uint32_t src_uncertainty_us,
                           uint32_t source_epoch, int64_t m2_us)
{
    if (!request_id) return ESP_ERR_INVALID_ARG;
    if (pend_find(request_id)) return ESP_ERR_INVALID_STATE;
    drift_pending_t *p = NULL;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < 5; i++) {
        if (!s_pend[i].used) { p = &s_pend[i]; break; }
        if (s_pend[i].deadline_us < oldest) { oldest = s_pend[i].deadline_us; p = &s_pend[i]; }
    }
    *p = (drift_pending_t){
        .used = true, .request_id = request_id,
        .t1_utc_us = t1_utc_us,
        .src_uncertainty_us = src_uncertainty_us,
        .source_epoch = source_epoch, .m2_us = m2_us,
        .deadline_us = m2_us + 5LL * 1000 * 1000,   /* >5 s = stale   */
    };
    return ESP_OK;
}

void drift_time_reply_tsp(uint32_t request_id, int64_t m3_us)
{
    drift_pending_t *p = pend_find(request_id);
    if (p) p->m3_us = m3_us;
}

anchor_result_t drift_time_commit(uint32_t request_id, uint64_t t4_utc_us,
                                  int64_t now_us, uint32_t *uncertainty_us)
{
    drift_pending_t *p = pend_find(request_id);
    if (!p) return ANCHOR_STALE;
    drift_pending_t e = *p;
    p->used = false;

    if (now_us > e.deadline_us) return ANCHOR_STALE;
    if (e.m3_us < e.m2_us)      return ANCHOR_BAD;
    if (e.src_uncertainty_us > DRIFT_SRC_UNC_MAX_US) return ANCHOR_UNCERTAIN;

    int64_t rtt = (int64_t)t4_utc_us - (int64_t)e.t1_utc_us
                - (e.m3_us - e.m2_us);
    if (rtt < 0 || rtt > DRIFT_RTT_MAX_US) return ANCHOR_RTT;
    if (t4_utc_us < e.t1_utc_us)           return ANCHOR_BAD;

    drift_anchor_t a = {
        .mono_us = (e.m2_us + e.m3_us) / 2,
        .utc_us  = (int64_t)((e.t1_utc_us + t4_utc_us) / 2),
        .uncertainty_us = e.src_uncertainty_us + (uint32_t)rtt / 2
                        + DRIFT_STAMP_ALLOW_US,
        .source_epoch = e.source_epoch,
    };

    anchor_result_t res = ANCHOR_OK;
    if (!s_epoch_set || e.source_epoch != s_cur_epoch) {
        /* new source epoch -> new interpolation segment; flush anchors
         * from a different epoch (never interpolate across, §8.1)     */
        s_nanchors = 0;
        s_cur_epoch = e.source_epoch;
        s_epoch_set = true;
        res = ANCHOR_EPOCH;
    } else if (s_nanchors > 0) {
        /* consistency: |new_map(mono_new) - utc_new| beyond combined
         * uncertainty + oscillator bound -> discontinuity segment     */
        for (int i = 0; i < s_nanchors; i++) {
            const drift_anchor_t *o = &s_anchors[i];
            double dt = (double)(a.mono_us - o->mono_us);
            double bound = (double)(o->uncertainty_us + a.uncertainty_us)
                         + (dt < 0 ? -dt : dt) * DRIFT_EXTRAP_PPM / 1e6;
            double gap = (double)(a.utc_us - o->utc_us) - dt;
            if (gap < 0) gap = -gap;
            if (gap > bound) {
                s_nanchors = 0;      /* do not interpolate through     */
                res = ANCHOR_DISCONTINUITY;
                break;
            }
        }
    }

    if (s_nanchors == DRIFT_ANCHORS_MAX) {
        memmove(s_anchors, s_anchors + 1,
                (DRIFT_ANCHORS_MAX - 1) * sizeof(drift_anchor_t));
        s_nanchors--;
    }
    s_anchors[s_nanchors++] = a;
    if (uncertainty_us) *uncertainty_us = a.uncertainty_us;
    return res;
}

bool drift_map_utc(int64_t mono_us, int64_t *utc_lo, int64_t *utc_hi)
{
    if (!utc_lo || !utc_hi || s_nanchors == 0) return false;

    /* bracketing pair = consecutive anchors straddling mono_us, same
     * domain (single-domain array by construction)                    */
    const drift_anchor_t *a = NULL, *b = NULL;
    for (int i = 0; i + 1 < s_nanchors; i++) {
        if (s_anchors[i].mono_us <= mono_us &&
            mono_us <= s_anchors[i + 1].mono_us) {
            a = &s_anchors[i];
            b = &s_anchors[i + 1];
            break;
        }
    }
    if (a && b) {
        /* affine interpolation, uncertainty = linear blend of endpoint
         * uncertainties (conservative envelope, §8.1)                  */
        double f = (double)(mono_us - a->mono_us) /
                   (double)(b->mono_us - a->mono_us);
        double mid = (double)a->utc_us +
                     f * (double)(b->utc_us - a->utc_us);
        double unc = a->uncertainty_us +
                     f * (double)(b->uncertainty_us - a->uncertainty_us);
        *utc_lo = (int64_t)(mid - unc);
        *utc_hi = (int64_t)(mid + unc);
        return true;
    }
    /* extrapolate from nearest endpoint with ±DRIFT_EXTRAP_PPM bound   */
    const drift_anchor_t *e =
        (mono_us < s_anchors[0].mono_us) ? &s_anchors[0]
                                         : &s_anchors[s_nanchors - 1];
    double dm = (double)(mono_us - e->mono_us);
    double unc = e->uncertainty_us +
                 (dm < 0 ? -dm : dm) * DRIFT_EXTRAP_PPM / 1e6;
    *utc_lo = (int64_t)((double)e->utc_us + dm - unc);
    *utc_hi = (int64_t)((double)e->utc_us + dm + unc);
    return true;
}

int drift_anchor_count(void) { return s_nanchors; }

/* ---------------- the one live session (§8.2) ---------------- */
/* Owned here so the card worker (producer) and the BLE ops layer
 * (reader) share it without depending on each other. Single writer:
 * sched task. Readers copy under no lock — fields are published
 * word-wise and the record builder tolerates a torn n_obs (fit uses
 * a prefix snapshot).                                                    */

static drift_session_t s_live_sess;
static bool            s_live;
static bool            s_closed;    /* session ended, data retained   */
static char            s_beacon_day[9];   /* "YYYYMMDD" + NUL          */
static char            s_beacon_name[96]; /* EZS_NAME_MAX              */
static bool            s_probe_used;      /* one probe per file set    */

drift_session_t *drift_live(void)   { return s_live ? &s_live_sess : NULL; }
drift_session_t *drift_last(void)
{
    /* live session, or the just-closed one (observation_closed), or
     * NULL when nothing has ever been observed this boot             */
    if (s_live || s_closed) {
        return &s_live_sess;
    }
    return NULL;
}
bool             drift_live_is(void){ return s_live; }
bool             drift_last_closed(void) { return s_closed && !s_live; }

void drift_live_begin(uint32_t cache_us, uint32_t jitter_us,
                      const char *day, const char *beacon)
{
    drift_sess_begin(&s_live_sess, cache_us, jitter_us);
    s_closed = false;
    memset(s_beacon_day, 0, sizeof(s_beacon_day));
    memset(s_beacon_name, 0, sizeof(s_beacon_name));
    if (day)    strncpy(s_beacon_day, day, sizeof(s_beacon_day) - 1);
    if (beacon) strncpy(s_beacon_name, beacon,
                        sizeof(s_beacon_name) - 1);
    s_probe_used = false;
    s_live = true;
}

void drift_live_end(void)
{
    /* retain the closed session for GET_DRIFT (§8.5): the record is
     * still queryable after therapy ends until a new session opens   */
    if (s_live) {
        s_closed = true;
    }
    s_live = false;
    s_beacon_name[0] = '\0';
}

const char *drift_live_beacon(char *day_out, char *name_out)
{
    if (!s_live || !s_beacon_name[0]) return NULL;
    if (day_out)  memcpy(day_out, s_beacon_day, sizeof(s_beacon_day));
    if (name_out) strcpy(name_out, s_beacon_name);
    return s_beacon_name;
}

void drift_live_set_beacon(const char *day, const char *name)
{
    if (!s_live) return;
    memset(s_beacon_day, 0, sizeof(s_beacon_day));
    memset(s_beacon_name, 0, sizeof(s_beacon_name));
    if (day)  strncpy(s_beacon_day, day, sizeof(s_beacon_day) - 1);
    if (name) strncpy(s_beacon_name, name, sizeof(s_beacon_name) - 1);
}

bool drift_live_probe_used(void)   { return s_probe_used; }
void drift_live_probe_mark(void)   { s_probe_used = true; }

/* ---------------- session observations ---------------- */

void drift_sess_begin(drift_session_t *s, uint32_t cache_bound_us,
                      uint32_t jitter_us)
{
    memset(s, 0, sizeof(*s));
    s->cache_bound_us = cache_bound_us;
    s->jitter_us = jitter_us;
}

void drift_sess_header(drift_session_t *s, uint32_t header_bytes,
                       uint32_t record_bytes, uint32_t record_duration_s,
                       int64_t hdr_start_local_s)
{
    s->header_known = true;
    s->header_bytes = header_bytes;
    s->record_bytes = record_bytes;
    s->record_duration_s = record_duration_s;
    s->hdr_start_local_s = hdr_start_local_s;
}

bool drift_sess_observe(drift_session_t *s, int64_t issue_us,
                        int64_t resp_us, uint64_t size)
{
    if (!s->header_known || s->n_obs >= DRIFT_OBS_MAX) return false;
    if (size < s->header_bytes) return true;   /* header-only stage    */
    s->seen_nonheader = true;
    uint64_t recs = (size - s->header_bytes) / s->record_bytes;
    if (recs > s->complete_records) s->complete_records = (uint32_t)recs;
    s->obs[s->n_obs++] = (drift_obs_t){
        .issue_us = issue_us, .resp_us = resp_us, .size = size,
    };
    return true;
}

#ifdef ESP_PLATFORM
/* ---------------- metadata-partition record store (§9) ----------------
 * Up to 64 latest observation-group records, d000..d063 + "dcur"
 * cursor; each blob: hdr {magic,ver,seq,len,crc} + JSON ≤ 8 KiB.
 * Only ever mounted when the HMAC key exists — a missing partition is
 * reported, never auto-erased.                                        */

#define DRIFT_META_SLOTS 64
#define DRM_MAGIC 0x44524D31u   /* "DRM1" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  rsv[3];
    uint32_t seq;
    uint16_t len;
    uint16_t rsv2;
    uint32_t crc32;
} drm_hdr_t;

static const char *TAG_DRIFT = "drift";

static void meta_key(uint8_t slot, char out[5])
{
    out[0] = 'd';
    out[1] = '0' + (slot / 100) % 10;
    out[2] = '0' + (slot / 10) % 10;
    out[3] = '0' + slot % 10;
    out[4] = '\0';
}

esp_err_t drift_meta_save(const char *json, size_t len)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h || !json || !len || len > 8192) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t cur = 0;
    nvs_get_u32(h, "dcur", &cur);
    uint8_t slot = cur % DRIFT_META_SLOTS;
    uint32_t seq = cur + 1;

    static uint8_t buf[sizeof(drm_hdr_t) + 8192];
    drm_hdr_t *hd = (drm_hdr_t *)buf;
    *hd = (drm_hdr_t){ .magic = DRM_MAGIC, .version = 1,
                       .seq = seq, .len = (uint16_t)len,
                       .crc32 = esp_crc32_le(0, (const uint8_t *)json,
                                             len) };
    memcpy(buf + sizeof(*hd), json, len);
    char k[5];
    meta_key(slot, k);
    esp_err_t err = nvs_set_blob(h, k, buf, sizeof(*hd) + len);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "dcur", seq);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DRIFT, "meta save failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Load the newest retained record JSON; NULL when none/absent. */
size_t drift_meta_latest(char *buf, size_t cap, uint32_t *seq_out)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return 0;
    uint32_t cur = 0;
    if (nvs_get_u32(h, "dcur", &cur) != ESP_OK || !cur) return 0;
    char k[5];
    meta_key((cur - 1) % DRIFT_META_SLOTS, k);
    drm_hdr_t hd;
    /* nvs_get_blob reads whole blobs — bound-check against cap        */
    size_t need = 0;
    if (nvs_get_blob(h, k, NULL, &need) != ESP_OK ||
        need < sizeof(hd) || need > cap) {
        return 0;
    }
    if (nvs_get_blob(h, k, buf, &need) != ESP_OK) return 0;
    memcpy(&hd, buf, sizeof(hd));
    if (hd.magic != DRM_MAGIC || hd.len > need - sizeof(hd) ||
        esp_crc32_le(0, (const uint8_t *)buf + sizeof(hd),
                     hd.len) != hd.crc32) {
        return 0;
    }
    memmove(buf, buf + sizeof(hd), hd.len);
    buf[hd.len] = '\0';
    if (seq_out) *seq_out = hd.seq;
    return hd.len;
}
#endif /* ESP_PLATFORM */
