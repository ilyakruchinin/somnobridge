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
#include "journal.h"
#include "nvs_flash.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sched";

/* -------- tunables (build constants per spec §6 + update U9) ------ */
#define Q_LEN              16
#define REQ_GAP_NORMAL_US  (1000 * 1000)   /* 1/s ordinary spacing    */
#define WATCH_PERIOD_US    (60 * 1000 * 1000)
#define COLD_PERIOD_US     (5 * 1000 * 1000)
#define NEIGH_PERIOD_US    (3600LL * 1000 * 1000)  /* once/hour        */
#define ACTIVE_CAP_8H      700
#define LEDGER_BUCKETS     481
#define LEDGER_BUCKET_US   (60LL * 1000 * 1000)
#define ACQ_BUDGET_PER_SET 40              /* dense-burst req cap/set */
#define ACQ_DENSE_MINUTE   40              /* dense starts/minute     */
#define SVC_WINDOW         8               /* day-listing svc median  */

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
static bool     s_round_active;           /* watch round in progress   */
static bool     s_round_growth;           /* any relevant write        */
static bool     s_round_failed;           /* any listing error         */
static uint32_t s_backoff_ms;
static int      s_backoff_idx;
static uint16_t s_ledger[LEDGER_BUCKETS]; /* minute buckets, 8h ring   */
static int      s_ledger_head;
static int64_t  s_ledger_ckpt_us;

/* dense burst state (U9): armed only while drift_dense_window says a
 * flush is predicted; spacing is the profile's 0.5 s guarded by the
 * listing service-time median                                      */
static int64_t  s_burst_until_us;
static int64_t  s_burst_next_us;
static int      s_burst_reqs;             /* this session              */
static int      s_dense_minute;           /* dense starts this minute  */
static int64_t  s_dense_minute_at_us;
static bool     s_had_live;               /* live-session edge detect  */
static int64_t  s_svc_us[SVC_WINDOW];     /* day-listing service times */
static int      s_svc_n;

static const uint32_t s_backoff_ms_ladder[] =
    {2000, 4000, 8000, 16000, 30000, 60000};

static therapy_state_t s_prev_therapy;
static est_state_t     s_prev_est = EST_INVALID;   /* journaled once   */
static acc_tier_t      s_prev_tier = ACC_NONE;

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

/* Median of recent day-listing service times — the U9 guard that
 * widens burst spacing when the folder approaches the poll interval. */
static int64_t svc_median_us(void)
{
    if (!s_svc_n) return 0;
    int64_t v[SVC_WINDOW];
    int n = s_svc_n < SVC_WINDOW ? s_svc_n : SVC_WINDOW;
    memcpy(v, s_svc_us, (size_t)n * sizeof(v[0]));
    for (int i = 1; i < n; i++) {          /* insertion sort, n <= 8  */
        int64_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return v[n / 2];
}

static void svc_note(int64_t req_us, int64_t resp_us)
{
    int64_t d = resp_us - req_us;
    if (d <= 0) return;
    if (s_svc_n < SVC_WINDOW) {
        s_svc_us[s_svc_n++] = d;
    } else {
        memmove(s_svc_us, s_svc_us + 1, (SVC_WINDOW - 1) * sizeof(int64_t));
        s_svc_us[SVC_WINDOW - 1] = d;
    }
}

/* Effective dense spacing: the profile's 0.5 s, widened when the
 * service-time median approaches it (U9). */
static int64_t dense_gap_us(void)
{
    int64_t gap = (int64_t)drift_profile()->dense_spacing_us;
    int64_t med = svc_median_us();
    if (med && med * 4 > gap * 3) {
        gap = med * 2;                 /* keep service < 50% of gap    */
    }
    return gap;
}

static int64_t earliest_start(int64_t now, bool dense)
{
    int64_t gap = dense ? dense_gap_us() : REQ_GAP_NORMAL_US;
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

/* ---------------- estimator feed (update spec U3/U4/U6) ----------
 * Every successfully-parsed day listing feeds the drift session —
 * changed or not; an unchanged response is what bounds the next
 * transition window. The session opens on the first listing with
 * file-level events (a write actually observed) and closes on
 * therapy OUT / IN->UNKNOWN regression. The estimate re-evaluates
 * after each feed and on a 1 s cadence so late anchors refit.      */

static void estim_feed_day(const char *day, const ezs_entry_t *ents,
                           int n, const cat_diff_t *diff,
                           const ezw_obs_t *obs, int64_t now)
{
    drift_session_t *ls = drift_live();
    if (!ls && diff && diff->n_events > 0) {
        /* file-level activity observed — open a session bound to this
         * day folder (mid-session join supported, U4)                */
        drift_live_begin(day);
        ls = drift_live();
        ESP_LOGI(TAG, "estim: session opened day=%s", day);
    }
    if (!ls || strcmp(ls->day, day)) {
        return;                        /* a different folder changed  */
    }
    int added = drift_feed_scan(ls, day, ents, n, diff,
                                obs->req_start_us, obs->resp_end_us);
    if (added > 0 || (diff && diff->n_events > 0)) {
        char rec[128];
        snprintf(rec, sizeof(rec),
                 "\"day\":\"%.8s\",\"ev\":%d,\"cat\":%d%s",
                 day, added, diff ? diff->n_events : 0,
                 (diff && diff->overflowed) ? ",\"of\":1" : "");
        jrn_put("obs", rec);           /* stamp/size events (§9)      */
    }
    drift_sess_eval(ls, now);
}

static void estim_housekeeping(int64_t now)
{
    static int64_t s_eval_at_us;
    drift_session_t *ls = drift_live();
    if (ls && !s_had_live) {
        /* session boundary — burst accounting is per file set       */
        s_burst_reqs = 0;
        s_burst_next_us = 0;
        s_burst_until_us = 0;
        s_had_live = true;
    } else if (!ls) {
        s_had_live = false;
    }
    /* ~1 s refit cadence so anchors landing after a scan still fold
     * into the live estimate                                       */
    if (ls && now - s_eval_at_us >= 1000 * 1000) {
        s_eval_at_us = now;
        drift_sess_eval(ls, now);
    }
    /* journal + persist on (state,tier) transitions (§9, U6)        */
    drift_record_t r;
    if (drift_pub_get(&r) &&
        (r.state != s_prev_est || r.tier != s_prev_tier)) {
        char rec[96];
        snprintf(rec, sizeof(rec),
                 "\"state\":%d,\"tier\":%d,\"w\":%d,\"seg\":%d",
                 (int)r.state, (int)r.tier, r.n_windows, r.n_segments);
        jrn_put("estimate", rec);
        if (r.tier == ACC_ACCURATE && s_prev_tier != ACC_ACCURATE) {
            /* first accurate result in the session — persist now    */
            drift_sot_commit(true, now);
        }
        s_prev_est = r.state;
        s_prev_tier = r.tier;
    }
    /* throttled improvement writes (U6) — internal rate limit       */
    drift_sot_commit(false, now);
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
            /* freeze + persist the final estimate (U6 IN->OUT)      */
            drift_session_t *ls = drift_live();
            if (ls) {
                drift_sess_eval(ls, now);
                drift_record_t dr;
                if (drift_pub_get(&dr)) {
                    dr.observation_closed = true;
                    char *rj = malloc(4096);
                    if (rj) {
                        int n = drift_estimate_json(&dr, rj, 4096);
                        if (n > 0) drift_meta_save(rj, (size_t)n);
                        free(rj);
                    }
                }
            }
            drift_live_end(now);
            drift_sot_commit(true, now);
            s_prev_est = EST_INVALID;
            s_prev_tier = ACC_NONE;
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
            svc_note(r->obs.req_start_us, r->obs.resp_end_us);
            cat_diff_t diff;
            memset(&diff, 0, sizeof(diff));
            cat_feed_day(j->dir, r->entries, r->n_entries, now, &diff);
            if (diff.changed) {
                s_round_growth = true;
                therapy_on_growth(now);   /* relevant write (§7.1) —
                    * stamp-only changes count identically (U3)       */
            }
            /* every successful listing feeds the estimator — an
             * unchanged response bounds the next transition (U4)     */
            estim_feed_day(j->dir, r->entries, r->n_entries, &diff,
                           &r->obs, now);
            ESP_LOGI(TAG, "day %s: %d listed, ev=%d chg=%d s=%lld r=%lld",
                     j->dir, r->n_entries, diff.n_events,
                     (int)diff.changed,
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
            drift_card_fw_note(v);      /* identity check → SOT (U6)  */
        }
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

    /* card firmware/identity check (U9): hourly, lowest priority —
     * feeds drift_card_fw_note; a changed identity invalidates the
     * persisted SOT                                                   */
    sched_job_t v = { .kind = JOB_VERSION, .prio = 7 };
    sched_submit(&v);
}

/* ---------------- dense burst (U9) ----------------
 * Armed only when the estimator predicts a flush AND the
 * characterization profile has enabled dense polling. One burst
 * request at a time, 0.5 s nominal spacing (service-time guarded),
 * stops the moment the stamp moves (drift_dense_window recomputes the
 * prediction from the changed response, pushing the next window one
 * cadence out — the burst self-terminates).                           */

static void plan_dense(int64_t now)
{
    int64_t open_us, close_us;
    if (!drift_dense_window(now, &open_us, &close_us)) {
        s_burst_until_us = 0;
        return;
    }
    if (s_burst_reqs >= ACQ_BUDGET_PER_SET) {
        return;                        /* 40/set allowance spent      */
    }
    /* per-minute dense-start cap (§6.2)                              */
    if (now - s_dense_minute_at_us >= 60LL * 1000 * 1000) {
        s_dense_minute_at_us = now;
        s_dense_minute = 0;
    }
    if (s_dense_minute >= ACQ_DENSE_MINUTE) {
        return;
    }
    if (s_burst_next_us && now < s_burst_next_us) {
        return;
    }
    drift_session_t *ls = drift_live();
    if (!ls || !ls->day[0]) {
        return;
    }
    sched_job_t d = { .kind = JOB_DAY, .prio = 1 };
    strncpy(d.dir, ls->day, CAT_DAY_STR);
    if (sched_submit(&d) == ESP_OK) {
        s_burst_reqs++;
        s_dense_minute++;
        s_burst_next_us = now + dense_gap_us();
        s_burst_until_us = close_us;
    }
}

/* True while this queued job is a burst DAY scan of the live session
 * — the exec path uses it to pick the dense spacing.                 */
static bool job_is_dense(const qjob_t *j, int64_t now)
{
    if (j->kind != JOB_DAY || !drift_live() ||
        now > s_burst_until_us) {
        return false;
    }
    return !strcmp(j->dir, drift_live()->day);
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
        estim_housekeeping(now);       /* refit cadence + SOT persist */

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
                plan_dense(now);      /* U9 burst — profile-gated     */
                plan_cold(now);
                plan_neigh(now);
            }
            continue;
        }
        if (!have) {
            continue;
        }

        /* spacing + budget + backoff */
        bool dense = job_is_dense(&j, now);
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
        sched_result_t r;
        exec_job(&j, &r);
        if (r.entries) {
            free(r.entries);
        }
    }
}
