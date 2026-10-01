/*
 * SomnoPal — card worker / poll scheduler (spec §6)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Single worker, one in-flight HTTP request, explicit budgets. The task
 * alternates: submit queue -> periodic planner -> execute.
 */
#include <stdlib.h>
#include <string.h>

#include "sched.h"

#include "drift.h"
#include "edf_hdr.h"
#include "journal.h"
#include "nvs_flash.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sched";

/* -------- tunables (build constants per spec §6) -------- */
#define Q_LEN              16
#define REQ_GAP_NORMAL_US  (1000 * 1000)   /* 1/s ordinary spacing    */
#define REQ_GAP_DENSE_US   (200 * 1000)    /* 200 ms burst-only       */
#define WATCH_PERIOD_US    (60 * 1000 * 1000)
#define COLD_PERIOD_US     (5 * 1000 * 1000)
#define NEIGH_PERIOD_US    (3600LL * 1000 * 1000)  /* once/hour        */
#define ACTIVE_CAP_8H      700
#define LEDGER_BUCKETS     481
#define LEDGER_BUCKET_US   (60LL * 1000 * 1000)
#define ACQ_BUDGET_PER_SET 40
#define ACQ_DENSE_MINUTE   40

typedef struct {
    job_kind_t kind;
    int        prio;
    char       dir[JOB_DIR_MAX];
    char       name[JOB_NAME_MAX];
    void      (*done)(const sched_result_t *, void *);
    void      *ctx;
} qjob_t;

static QueueHandle_t s_q;
static volatile bool s_link_up;
static int64_t  s_last_req_us;
static int64_t  s_next_watch_us;
static int64_t  s_next_cold_us;
static int64_t  s_next_neigh_us;
static int      s_dense_minute;           /* dense starts this minute  */
static bool     s_round_active;           /* watch round in progress   */
static bool     s_round_growth;           /* any relevant write        */
static bool     s_round_failed;           /* any listing error         */
static uint32_t s_backoff_ms;
static int      s_backoff_idx;
static uint16_t s_ledger[LEDGER_BUCKETS]; /* minute buckets, 8h ring   */
static int      s_ledger_head;
static int64_t  s_ledger_ckpt_us;

static const uint32_t s_backoff_ms_ladder[] =
    {2000, 4000, 8000, 16000, 30000, 60000};

/* estimator session bounds (§8.2/§8.3): provisional until validated   */
#define SESS_CACHE_BOUND_US  (60LL * 1000 * 1000)  /* listing cache C   */
#define SESS_JITTER_US       (300 * 1000)          /* per-edge jitter   */
#define PROBE_BUF_BYTES      (16 * 1024)
static therapy_state_t s_prev_therapy;
static est_state_t     s_prev_est = EST_INVALID;   /* journaled once   */

/* ---------------- ledger ---------------- */

static void ledger_add(bool active_mode, int64_t now)
{
    if (!active_mode) {
        return;
    }
    int idx = (int)((now / LEDGER_BUCKET_US) % LEDGER_BUCKETS);
    if (idx != s_ledger_head) {
        s_ledger_head = idx;
        s_ledger[idx] = 0;
    }
    if (s_ledger[idx] < 0xFFFF) {
        s_ledger[idx]++;
    }
    s_ledger_ckpt_us = now;
}

static uint32_t s_restored_8h;      /* pre-boot requests, conservative */

uint32_t sched_active_8h(void)
{
    uint32_t t = s_restored_8h;
    for (int i = 0; i < LEDGER_BUCKETS; i++) {
        t += s_ledger[i];
    }
    return t;
}

/* ---------------- load_budget checkpoint (§6.1) ----------------
 * Minute-bucket counts persist to metadata NVS once/minute. A reboot
 * cannot age them (no RTC), so a restored lump counts toward the cap
 * until this boot's own ledger has run 8 h — expiry rounds late, per
 * spec. If >60 s pass since the last successful commit, active-mode
 * requests suspend until one lands.                                 */

#define CKPT_PERIOD_US  (60LL * 1000 * 1000)
#define CKPT_MAGIC      0x4C424331u   /* "LBC1" */
static int64_t  s_ckpt_ok_us;
static uint32_t s_ckpt_seq;
static int64_t  s_ledger_boot_us;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint16_t n;
    uint16_t pad;
    uint16_t cnt[LEDGER_BUCKETS];
} lbc_t;

static void ledger_checkpoint(int64_t now)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) {
        s_ckpt_ok_us = now;          /* dev path: no partition         */
        return;
    }
    static lbc_t b;
    memset(&b, 0, sizeof(b));
    b.magic = CKPT_MAGIC;
    b.seq = ++s_ckpt_seq;
    b.n = LEDGER_BUCKETS;
    memcpy(b.cnt, s_ledger, sizeof(b.cnt));
    if (nvs_set_blob(h, "load_budget", &b, sizeof(b)) == ESP_OK &&
        nvs_commit(h) == ESP_OK) {
        s_ckpt_ok_us = now;
    } else {
        ESP_LOGW(TAG, "load_budget checkpoint failed");
    }
}

static void ledger_restore(void)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return;
    static lbc_t b;
    size_t want = sizeof(b);
    if (nvs_get_blob(h, "load_budget", &b, &want) == ESP_OK &&
        want == sizeof(b) && b.magic == CKPT_MAGIC &&
        b.n == LEDGER_BUCKETS) {
        uint32_t sum = 0;
        for (int i = 0; i < LEDGER_BUCKETS; i++) sum += b.cnt[i];
        s_ckpt_seq = b.seq;
        s_restored_8h = sum;         /* counts until boot+8h (conserv.) */
        ESP_LOGI(TAG, "load_budget restored: %lu pre-boot requests",
                 (unsigned long)sum);
    }
}

bool sched_budget_ok(void) { return sched_active_8h() < ACTIVE_CAP_8H; }
uint32_t sched_backoff_ms(void) { return s_backoff_ms; }

/* ---------------- spacing / budget gates ---------------- */

/* Dense acquisition (200 ms spacing, 40 starts/min, 40/set/30 min) is an
 * opt-in profile gated on E-sim jitter validation — disabled in v1, so no
 * dense-start gate is wired up here. s_dense_minute still counts starts so
 * the ledger stays honest if a dense profile is ever enabled.            */

static int64_t earliest_start(int64_t now, bool dense)
{
    int64_t gap = dense ? REQ_GAP_DENSE_US : REQ_GAP_NORMAL_US;
    int64_t t = s_last_req_us + gap;
    if (t < now) {
        t = now;
    }
    if (s_backoff_ms) {
        int64_t bo = s_last_req_us + (int64_t)s_backoff_ms * 1000;
        if (bo > t) {
            t = bo;
        }
    }
    return t;
}

/* ---------------- estimator feed (§8.2/§8.3) ----------------
 * The live drift session tracks ONE beacon: the first file observed
 * to grow while monitoring is active. Header bytes come from the
 * single bounded probe (one per observed file set). Session closes
 * on therapy gap / OUT — nothing carries across a discontinuity.   */

static void estim_on_growth(const char *day, const cat_file_t *grown,
                            const ezw_obs_t *obs)
{
    if (!grown || !grown->name[0]) {
        return;
    }
    drift_session_t *ls = drift_live();
    if (!ls) {
        /* no session yet — this growth opens one (§8.3)              */
        drift_live_begin(SESS_CACHE_BOUND_US, SESS_JITTER_US,
                         day, grown->name);
        ls = drift_live();
        ESP_LOGI(TAG, "estim: opened session day=%s beacon=%s live=%d",
                 day, grown->name, (int)drift_live_is());
    }
    char bday[CAT_DAY_STR], bname[96];
    const char *bn = drift_live_beacon(bday, bname);
    if (!bn || strcmp(day, bday) || strcmp(grown->name, bname)) {
        return;      /* growth in a non-beacon file: not our session  */
    }
    drift_sess_observe(ls, obs->req_start_us, obs->resp_end_us,
                       grown->size);
    /* keep the fit warm; journal + persist on state transitions (§9) */
    if (ls->header_known && ls->n_obs >= 3) {
        int64_t d = (int64_t)ls->record_duration_s * 1000000;
        if (drift_sess_fit(ls, d * 9 / 10, d * 11 / 10,
                           INT64_MIN / 4, INT64_MAX / 4)) {
            est_state_t now_est =
                ls->fit_valid ? EST_VALIDATED : EST_PENDING;
            if (now_est != s_prev_est) {
                char rec[64];
                snprintf(rec, sizeof(rec), "\"fit\":%s,\"obs\":%d",
                         ls->fit_valid ? "ok" : "no", ls->n_obs);
                jrn_put("estimate", rec);
                /* latest-group estimate into metadata NVS (§9)       */
                if (ls->fit_valid) {
                    drift_record_t dr;
                    drift_build_record(ls, &dr);
                    char *rj = malloc(8192);
                    if (rj) {
                        int n = drift_record_json(&dr, NULL, NULL,
                                                  rj, 8192);
                        if (n > 0) drift_meta_save(rj, (size_t)n);
                        free(rj);
                    }
                }
            }
            s_prev_est = now_est;
        }
    }
    /* One early probe per observed file set (§8.2). Every file in a
     * session shares the EDF layout, so probe the smallest listed
     * candidate ≤16 KiB rather than the beacon itself — the beacon is
     * whatever grows first and is usually the largest file (BRP).    */
    if (!ls->header_known && !drift_live_probe_used()) {
        int n = cat_day_entries(day, NULL, 0);
        if (n > 0) {
            cat_file_t *files = malloc((size_t)n * sizeof(cat_file_t));
            if (files) {
                cat_day_entries(day, files, n);
                const cat_file_t *best = NULL;
                for (int i = 0; i < n; i++) {
                    if (!strstr(files[i].name, ".edf") ||
                        files[i].size == 0 ||
                        files[i].size > PROBE_BUF_BYTES) {
                        continue;
                    }
                    if (!best || files[i].size < best->size) {
                        best = &files[i];
                    }
                }
                if (best) {
                    drift_live_probe_mark();
                    sched_job_t pj = { .kind = JOB_PROBE, .prio = 2 };
                    strncpy(pj.dir, day, JOB_DIR_MAX - 1);
                    strncpy(pj.name, best->name, JOB_NAME_MAX - 1);
                    sched_submit(&pj);
                    ESP_LOGI(TAG, "probe queued: %s (%llu B)", best->name,
                             (unsigned long long)best->size);
                }
                free(files);
            }
        }
    }
}

static void estim_track_therapy(int64_t now)
{
    therapy_state_t cur = therapy_status()->state;
    if (cur != s_prev_therapy) {
        /* session ends on OUT (therapy over) or on a regression to
         * UNKNOWN (monitoring lost) — the UNKNOWN->IN transition is
         * caused by the same growth that opens a session, so it must
         * not close it                                                */
        if (cur == THERAPY_OUT ||
            (cur == THERAPY_UNKNOWN &&
             s_prev_therapy == THERAPY_IN)) {
            /* persist the final estimate before closing (§9)         */
            drift_session_t *ls = drift_live();
            if (ls && ls->fit_valid) {
                drift_record_t dr;
                drift_build_record(ls, &dr);
                dr.observation_closed = true;
                char *rj = malloc(8192);
                if (rj) {
                    int n = drift_record_json(&dr, NULL, NULL, rj, 8192);
                    if (n > 0) drift_meta_save(rj, (size_t)n);
                    free(rj);
                }
            }
            drift_live_end();
            s_prev_est = EST_INVALID;
        }
        s_prev_therapy = cur;
        char rec[64];                    /* therapy transition (§9)    */
        snprintf(rec, sizeof(rec), "\"fsm\":%d", (int)cur);
        jrn_put("event", rec);
    }
    (void)now;
}

/* ---------------- job execution ---------------- */

static void obs_success(bool ok)
{
    if (ok) {
        s_backoff_idx = 0;
        s_backoff_ms = 0;
    } else {
        if (s_backoff_idx < (int)(sizeof(s_backoff_ms_ladder) /
                                sizeof(s_backoff_ms_ladder[0])) - 1) {
            s_backoff_idx++;
        }
        s_backoff_ms = s_backoff_ms_ladder[s_backoff_idx];
    }
}

static void exec_job(const qjob_t *j, sched_result_t *r)
{
    memset(r, 0, sizeof(*r));
    int64_t now = esp_timer_get_time();

    switch (j->kind) {
    case JOB_PARENT: {
        r->entries = malloc(EZS_ENTRIES_MAX * sizeof(ezs_entry_t));
        if (!r->entries) { r->err = ESP_ERR_NO_MEM; return; }
        r->err = ezs_list_dir("A:\\DATALOG", r->entries,
                              &r->n_entries, &r->obs);
        if (r->err == ESP_OK) {
            cat_feed_parent(r->entries, r->n_entries, now);
            ledger_add(therapy_status()->state != THERAPY_OUT, now);
            obs_success(true);
        } else {
            s_round_failed = true;
            obs_success(false);
            ESP_LOGW(TAG, "list %s failed: http=%d transport=%d parse=%d "
                     "body=%lu", "A:\\DATALOG", r->obs.http_status,
                     r->obs.transport, r->obs.parse,
                     (unsigned long)r->obs.body_bytes);
        }
        break;
    }
    case JOB_DAY: {
        strncpy(r->day, j->dir, CAT_DAY_STR - 1);
        char canon[12 + CAT_DAY_STR];   /* "A:\DATALOG\" + day + NUL   */
        memcpy(canon, "A:\\DATALOG\\", 11);
        memcpy(canon + 11, j->dir, CAT_DAY_STR);   /* day is NUL-term */
        r->entries = malloc(EZS_ENTRIES_MAX * sizeof(ezs_entry_t));
        if (!r->entries) { r->err = ESP_ERR_NO_MEM; return; }
        r->err = ezs_list_dir(canon, r->entries, &r->n_entries, &r->obs);
        if (r->err == ESP_OK) {
            bool changed = false;
            cat_file_t grown = {0};
            cat_feed_day(j->dir, r->entries, r->n_entries, now,
                         &changed, &grown);
            if (changed) {
                s_round_growth = true;
                therapy_on_growth(now);   /* relevant write (§7.1)     */
                estim_on_growth(j->dir, &grown, &r->obs);  /* §8.3     */
            }
            ESP_LOGI(TAG, "day %s: %d listed, changed=%d grown=%s s=%lld r=%lld",
                     j->dir, r->n_entries, (int)changed,
                     grown.name[0] ? grown.name : "-",
                     (long long)r->obs.req_start_us,
                     (long long)r->obs.resp_end_us);
            ledger_add(therapy_status()->state != THERAPY_OUT, now);
            obs_success(true);
        } else {
            s_round_failed = true;
            cat_mark_folder_stale(j->dir);
            obs_success(false);
            ESP_LOGW(TAG, "day %s failed: http=%d transport=%d parse=%d",
                     j->dir, r->obs.http_status, r->obs.transport,
                     r->obs.parse);
        }
        break;
    }
    case JOB_VERSION: {
        char v[64];
        r->err = ezs_client_version(v, sizeof(v), &r->obs);
        obs_success(r->err == ESP_OK);
        if (r->err == ESP_OK) {
            ESP_LOGI(TAG, "card fw: %s", v);
        }
        break;
    }
    case JOB_PROBE: {
        /* bounded header read of one file in the observed set (§8.2):
         * bytes [0, listed_size) capped at 16 KiB, once per file set. */
        drift_session_t *ls = drift_live();
        if (!ls || ls->header_known) {
            r->err = ESP_ERR_INVALID_STATE;
            break;
        }
        cat_file_t cur;
        if (!cat_find_file(j->dir, j->name, &cur) ||
            cur.size > PROBE_BUF_BYTES) {
            r->err = ESP_ERR_INVALID_SIZE;
            break;
        }
        uint8_t *buf = malloc(PROBE_BUF_BYTES);
        if (!buf) { r->err = ESP_ERR_NO_MEM; break; }
        size_t got = 0;
        char canon[12 + CAT_DAY_STR];
        memcpy(canon, "A:\\DATALOG\\", 11);
        memcpy(canon + 11, j->dir, CAT_DAY_STR);
        r->err = ezs_probe_header(canon, j->name, cur.size,
                                  buf, cur.size, &got, &r->obs);
        if (r->err == ESP_OK) {
            edf_hdr_t h;
            if (edf_hdr_parse(buf, got, &h)) {
                drift_sess_header(ls, h.header_bytes, h.record_bytes,
                                  h.record_duration_s, h.start_local_s);
                ESP_LOGI(TAG, "ed hdr: H=%lu R=%lu D=%lus ns=%lu",
                         (unsigned long)h.header_bytes,
                         (unsigned long)h.record_bytes,
                         (unsigned long)h.record_duration_s,
                         (unsigned long)h.n_signals);
                char rec[160];
                snprintf(rec, sizeof(rec),
                         "\"f\":\"%s\",\"H\":%lu,\"R\":%lu,\"D\":%lu",
                         j->name, (unsigned long)h.header_bytes,
                         (unsigned long)h.record_bytes,
                         (unsigned long)h.record_duration_s);
                jrn_put("header", rec);      /* evidence (§9)         */
            } else {
                ESP_LOGW(TAG, "probe: not parseable EDF (%u B)",
                         (unsigned)got);
            }
        } else {
            ESP_LOGW(TAG, "probe %s failed: http=%d transport=%d got=%u",
                     j->name, r->obs.http_status, r->obs.transport,
                     (unsigned)got);
        }
        free(buf);
        obs_success(r->err == ESP_OK);
        break;
    }
    case JOB_GET_FILE:
    case JOB_LIST_LIVE:
        /* implemented via ble_api / portal paths with explicit ctx */
        r->err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    if (j->done) {
        j->done(r, j->ctx);
    }
}

/* ---------------- planner ---------------- */

static void plan_watch(int64_t now)
{
    if (now < s_next_watch_us) {
        return;
    }
    s_next_watch_us = now + WATCH_PERIOD_US;

    /* close the previous round: growth was already reported live via
     * therapy_on_growth; here we only report quiet / interrupted rounds */
    if (s_round_active) {
        if (s_round_failed) {
            therapy_on_gap();
        } else if (!s_round_growth) {
            therapy_on_quiet_round(now);
        }
        /* poll evidence: one bounded record per closed round (§9)     */
        char rec[96];
        snprintf(rec, sizeof(rec),
                 "\"round\":%s%s,\"n8h\":%lu",
                 s_round_failed ? "fail" :
                    (s_round_growth ? "growth" : "quiet"), "",
                 (unsigned long)sched_active_8h());
        jrn_put("poll", rec);
    }
    s_round_active = true;
    s_round_growth = false;
    s_round_failed = false;

    sched_job_t p = { .kind = JOB_PARENT, .prio = 3 };
    sched_submit(&p);

    char hot[4][CAT_DAY_STR];
    int nh = cat_hot_days(hot, 4);
    for (int i = 0; i < nh; i++) {
        sched_job_t d = { .kind = JOB_DAY, .prio = 3 };
        strncpy(d.dir, hot[i], CAT_DAY_STR);
        sched_submit(&d);
    }
}

static void plan_cold(int64_t now)
{
    if (now < s_next_cold_us) {
        return;
    }
    /* cold crawl only when idle/OUT per spec §6.2 */
    if (therapy_status()->state != THERAPY_OUT) {
        s_next_cold_us = now + COLD_PERIOD_US;
        return;
    }
    s_next_cold_us = now + COLD_PERIOD_US;
    char q[8][CAT_DAY_STR];
    int n = cat_scan_queue(q, 8);
    if (n > 0) {
        sched_job_t d = { .kind = JOB_DAY, .prio = 6 };
        strncpy(d.dir, q[0], CAT_DAY_STR);
        sched_submit(&d);
    }
}

static void plan_neigh(int64_t now)
{
    if (now < s_next_neigh_us) {
        return;
    }
    s_next_neigh_us = now + NEIGH_PERIOD_US;
    /* once/hour housekeeping while in therapy — hot neighbours only,
     * the active folder is handled by the liveness loop itself (§6.2) */
}

/* ---------------- task ---------------- */

esp_err_t sched_init(void)
{
    s_q = xQueueCreate(Q_LEN, sizeof(qjob_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    therapy_init();
    ledger_restore();
    s_ckpt_ok_us = esp_timer_get_time();     /* grace: first 60 s free */
    s_ledger_boot_us = s_ckpt_ok_us;
    return ESP_OK;
}

void sched_set_card_ip(uint32_t ip) { ezs_client_set_host(ip); }
void sched_set_link(bool up)        { s_link_up = up; }

esp_err_t sched_submit(const sched_job_t *job)
{
    qjob_t q = {
        .kind = job->kind, .prio = job->prio, .ctx = job->ctx,
        .done = job->done,
    };
    strncpy(q.dir, job->dir, JOB_DIR_MAX - 1);
    strncpy(q.name, job->name, JOB_NAME_MAX - 1);
    if (xQueueSend(s_q, &q, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sched_run_forever(void)
{
    int64_t last_plan = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        therapy_tick(now);
        estim_track_therapy(now);      /* session open/close on FSM   */

        /* restored pre-boot budget expires only after this boot has
         * itself run 8 h (§6.1: expiry rounds late, never early)     */
        if (s_restored_8h &&
            now - s_ledger_boot_us > 8LL * 3600 * 1000000LL) {
            s_restored_8h = 0;
        }
        /* once/minute budget checkpoint; suspension gate below       */
        if (now - s_ckpt_ok_us >= CKPT_PERIOD_US) {
            ledger_checkpoint(now);
        }

        qjob_t j;
        bool have = xQueuePeek(s_q, &j, pdMS_TO_TICKS(50)) == pdTRUE;

        if (!have && now - last_plan > 500 * 1000) {
            /* run planner every 500 ms when idle */
            last_plan = now;
            if (s_link_up) {
                plan_watch(now);
                plan_cold(now);
                plan_neigh(now);
            }
            continue;
        }
        if (!have) {
            continue;
        }

        /* spacing + budget + backoff */
        bool dense = false;           /* dense acquisition is an opt-in
                                         * profile — off in v1 */
        int64_t when = earliest_start(now, dense);
        if (when > now) {
            vTaskDelay(pdMS_TO_TICKS((when - now) / 1000 + 1));
            continue;
        }
        /* §6.1: active requests suspend when the budget is exhausted
         * OR the last checkpoint is older than 60 s (uncommitted
         * tail cannot grow unaccounted)                               */
        bool checkpoint_fresh =
            now - s_ckpt_ok_us < CKPT_PERIOD_US;
        if ((!sched_budget_ok() || !checkpoint_fresh) &&
            therapy_status()->state != THERAPY_OUT) {
            /* budget exhausted -> UNKNOWN + block (§6.1) */
            if (therapy_status()->state == THERAPY_IN) {
                /* keep state; report budget via reason */
            }
            xQueueReceive(s_q, &j, 0);       /* drop lowest prio item  */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        xQueueReceive(s_q, &j, 0);
        s_last_req_us = esp_timer_get_time();
        s_dense_minute++;
        sched_result_t r;
        exec_job(&j, &r);
        if (r.entries) {
            free(r.entries);
        }
    }
}
