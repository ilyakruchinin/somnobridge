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
#include "phase.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sched";

/* -------- tunables (spec §6, update2 §3/§5) ----------------------- */
#define Q_LEN              16
#define REQ_GAP_NORMAL_US  (1000 * 1000)   /* 1/s ordinary spacing    */
#define COVER_PERIOD_US    (60LL * 1000 * 1000)  /* coverage round    */
#define COLD_PERIOD_US     (5 * 1000 * 1000)
#define NEIGH_PERIOD_US    (3600LL * 1000 * 1000)  /* once/hour        */
#define ACTIVE_CAP_8H      SCHED_ACTIVE_CAP_8H
#define LEDGER_BUCKETS     481
#define LEDGER_BUCKET_US   (60LL * 1000 * 1000)
#define TRACK_FRESH_US     (75LL * 1000 * 1000)  /* tracked folder must
     * have scanned within this at round close — beyond the longest
     * scheduled inter-window gap (~55 s in MAINTAIN) with margin     */
#define SVC_WINDOW         8               /* day-listing svc median  */

typedef struct {
    job_kind_t kind;
    int        prio;
    uint32_t   min_gap_us;
    int64_t    deadline_us;
    bool       required;
    bool       phase_job;
    char       dir[JOB_DIR_MAX];
    char       name[JOB_NAME_MAX];
    void      (*done)(const sched_result_t *, void *);
    void      *ctx;
} qjob_t;

static QueueHandle_t s_q;
static volatile bool s_link_up;
static int64_t  s_last_req_us;
static int64_t  s_next_cover_us;
static int64_t  s_next_cold_us;
static int64_t  s_next_neigh_us;
static int64_t  s_next_plan_us;
static int64_t  s_ph_due_us;              /* next phase pull due      */
static bool     s_round_active;           /* coverage round in progr. */
static bool     s_round_growth;           /* any relevant write        */
static bool     s_round_failed;           /* coverage listing error    */
static uint32_t s_backoff_ms;
static int      s_backoff_idx;
static uint16_t s_ledger[LEDGER_BUCKETS]; /* minute buckets, 8h ring   */
static int      s_ledger_head;
static int64_t  s_ledger_ckpt_us;

/* phase-tracking poll schedule (update2 §3): one tracker bound to the
 * tracked day folder — the live estimator session's day while a
 * session is open, the newest known day otherwise                  */
static phase_t  s_ph;
static char     s_beacon_seen[CAT_NAME_MAX];
static int      s_ph_logged     = -1;   /* last journaled phase state */
static uint32_t s_ph_miss_logged;       /* last journaled miss count  */
static int64_t  s_svc_us[SVC_WINDOW];   /* day-listing service times  */
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

int sched_phase_json(char *buf, size_t cap)
{
    int64_t now = esp_timer_get_time();
    int64_t mark_in = s_ph.mark_us > now ? (s_ph.mark_us - now) / 1000 : 0;
    int64_t br_ms = (s_ph.last_br_hi_us > s_ph.last_br_lo_us)
                  ? (s_ph.last_br_hi_us - s_ph.last_br_lo_us) / 1000 : 0;
    int64_t common_ms = (s_ph.fit_valid &&
                         s_ph.phase_hi_us > s_ph.phase_lo_us)
                      ? (s_ph.phase_hi_us - s_ph.phase_lo_us) / 1000 : 0;
    int64_t pred_ms = (s_ph.fit_valid &&
                       s_ph.pred_hi_us > s_ph.pred_lo_us)
                    ? (s_ph.pred_hi_us - s_ph.pred_lo_us) / 1000 : 0;
    char common_txt[24];
    if (s_ph.fit_valid) {
        snprintf(common_txt, sizeof(common_txt), "%lld",
                 (long long)common_ms);
    } else {
        strcpy(common_txt, "null");
    }
    int n = snprintf(buf, cap,
        "\"phase\":{\"st\":\"%s\",\"day\":\"%.8s\",\"armed\":%s,"
        "\"pulls\":[%d,%d],\"bracket_ms\":%lld,"
        "\"common_width_ms\":%s,\"prediction_width_ms\":%lld,"
        "\"period_ms\":%lld,\"period_interval_ms\":[%lld,%lld],"
        "\"cycle\":%lu,\"cycle_ambiguous\":%s,"
        "\"iunc_ms\":%lld,\"mark_in_ms\":%lld,\"brackets\":%lu,"
        "\"misses\":%lu,\"reacq_reason\":\"%s\",\"model\":\"u3_interval_v1\"}",
        phase_state_name(s_ph.st), s_ph.day,
        s_ph.armed ? "true" : "false",
        s_ph.pull_sent, s_ph.pull_total,
        (long long)br_ms, common_txt,
        (long long)pred_ms,
        (long long)(s_ph.period_us / 1000),
        (long long)(s_ph.per_lo_us / 1000),
        (long long)(s_ph.per_hi_us / 1000),
        (unsigned long)s_ph.last_cycle,
        s_ph.cycle_ambig ? "true" : "false",
        (long long)(s_ph.iunc_us / 1000), (long long)mark_in,
        (unsigned long)s_ph.n_brackets,
        (unsigned long)s_ph.n_misses,
        phase_reacq_name(s_ph.reacq_reason));
    return n > 0 && n < (int)cap ? n : 0;
}

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

/* Minimum start spacing for a job: a phase pull carries its grid's
 * own spacing; everything else uses the ordinary 1 s. Either way the
 * service-time guard applies — the listing median must stay under
 * 50% of the gap (update2 §3.3).                                  */
static int64_t earliest_start(int64_t now, const qjob_t *j)
{
    int64_t gap = j->min_gap_us ? (int64_t)j->min_gap_us
                                : REQ_GAP_NORMAL_US;
    int64_t med = svc_median_us();
    if (med && med * 2 > gap) {
        gap = med * 2;
    }
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

static bool diff_has_write(const cat_diff_t *diff)
{
    if (!diff) return false;
    for (int i = 0; i < diff->n_events; i++) {
        /* NEW/GREW/SHRUNK/STAMP are file writes; GONE alone is not
         * evidence that a new day's therapy session has started       */
        if (diff->ev[i].kind != CAT_EV_GONE) {
            return true;
        }
    }
    return false;
}

static int estim_feed_day(const char *day, const ezs_entry_t *ents,
                          int n, const cat_diff_t *diff,
                          const ezw_obs_t *obs, int64_t now)
{
    drift_session_t *ls = drift_live();
    if (ls && strcmp(ls->day, day)) {
        if (diff_has_write(diff)) {
            /* a different day folder is WRITING — the old session is
             * over (midnight rollover while monitoring stayed up,
             * U4). A quiet coverage pull of another folder is just
             * coverage: it must NOT end the live session — the day
             * is still the active one, only absent from this listing */
            drift_live_end(now);
            ls = NULL;
        } else {
            return -1;           /* foreign folder, nothing to feed   */
        }
    }
    if (!ls && diff_has_write(diff)) {
        /* file-level activity observed — open a session bound to this
         * day folder (mid-session join supported, U4). A same-day
         * reopen after a monitoring/OUT gap resumes the retained
         * evidence rather than discarding it (update2 §3.5)          */
        if (!drift_live_resume(day)) {
            drift_live_begin(day);
        }
        ls = drift_live();
        ESP_LOGI(TAG, "estim: session opened day=%s", day);
    }
    if (!ls || strcmp(ls->day, day)) {
        return -1;                       /* a different folder changed */
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
    return added;
}

/* The phase tracker's "transition" input for one completed tracked-day
 * listing: while a session is live it is the beacon's own periodic
 * update (TRANSITION events — creation/vanish are not flushes);
 * before any session it is any non-membership file write.           */
static bool is_periodic_change(const char *day, const cat_diff_t *diff)
{
    if (!diff) return false;
    drift_session_t *ls = drift_live();
    if (ls && ls->day[0] && !strcmp(ls->day, day) && ls->beacon[0]) {
        /* The catalog diff remains authoritative even after the
         * estimator's 96-event evidence array is full.              */
        for (int i = 0; i < diff->n_events; i++) {
            const cat_event_t *e = &diff->ev[i];
            if ((e->kind == CAT_EV_GREW || e->kind == CAT_EV_SHRUNK ||
                 e->kind == CAT_EV_STAMP) &&
                !strcmp(e->name, ls->beacon)) {
                return true;
            }
        }
        return false;
    }
    for (int i = 0; i < diff->n_events; i++) {
        if (diff->ev[i].kind == CAT_EV_GREW ||
            diff->ev[i].kind == CAT_EV_SHRUNK ||
            diff->ev[i].kind == CAT_EV_STAMP) {
            return true;
        }
    }
    return false;
}

static void estim_housekeeping(int64_t now)
{
    static int64_t s_eval_at_us;
    drift_session_t *ls = drift_live();
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
        /* the session ends ONLY on OUT (therapy over). An IN->UNKNOWN
         * regression is a monitoring gap, not a session end: the day
         * folder, beacon, and clock are unchanged, so the accumulated
         * windows remain valid evidence — closing would discard the
         * dense-bracketed bound on every transient listing failure
         * (U4/U6). Growth resuming re-feeds the same session; a new
         * day's growth is handled by the day check in estim_feed_day */
        if (cur == THERAPY_OUT) {
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
            if (diff.n_events > 0) {
                /* a RESIDENT->RESIDENT write, not a baseline relist —
                 * diff.changed alone also fires on a folder's first
                 * listing, which emits no events and is not growth   */
                s_round_growth = true;
                therapy_on_growth(now);   /* relevant write (§7.1) —
                    * stamp-only changes count identically (U3)       */
            }
            /* every successful listing feeds the estimator — an
             * unchanged response bounds the next transition (U4)     */
            estim_feed_day(j->dir, r->entries, r->n_entries,
                           &diff, &r->obs, now);
            /* …and the phase tracker — unchanged scans bound the
             * next transition from below (update2 §2/§3)             */
            if (s_ph.day[0] && !strcmp(j->dir, s_ph.day)) {
                phase_obs(&s_ph, r->obs.req_start_us,
                          r->obs.resp_end_us,
                          is_periodic_change(j->dir, &diff));
            }
            ESP_LOGI(TAG, "day %s: %d listed, ev=%d chg=%d s=%lld r=%lld",
                     j->dir, r->n_entries, diff.n_events,
                     (int)diff.changed,
                     (long long)r->obs.req_start_us,
                     (long long)r->obs.resp_end_us);
            obs_success(true);
            therapy_note_listing(now);   /* monitor alive (§6.3)       */
        } else {
            /* a tracked-folder pull failure is a phase observation
             * of nothing — the tracker's miss path stays
             * deterministic; round completeness is judged at cover
             * close via the tracked-folder freshness bound instead
             * of failing the whole round on one bad pull (update2)   */
            if (s_ph.day[0] && !strcmp(j->dir, s_ph.day)) {
                phase_fail(&s_ph, now);
            } else {
                s_round_failed = true;
            }
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

/* -------- tracked-day phase planner (update2 §3) --------
 * One folder is tracked: the live estimator session's day while a
 * session is open, else the newest known day. Every pull is a plain
 * JOB_DAY; only the submission timing differs per tracker state, and
 * every completion of the tracked folder feeds the tracker — whether
 * it was submitted by the tracker or not.                          */

static void update_track_target(void)
{
    drift_session_t *ls = drift_live();
    char dbuf[CAT_DAY_STR];
    const char *want = NULL;
    if (ls && ls->day[0]) {
        want = ls->day;
    } else if (cat_newest_day(dbuf)) {
        want = dbuf;
    }
    if (!want) {
        phase_track(&s_ph, NULL, false);
        return;
    }
    bool live = ls && ls->day[0];
    phase_track(&s_ph, want, live);
    /* a beacon swap must not reuse the old file's phase (§3.5)      */
    if (live && ls->beacon[0]) {
        if (s_beacon_seen[0] && strcmp(s_beacon_seen, ls->beacon)) {
            phase_reacquire(&s_ph);
            jrn_put("phase", "\"ev\":\"beacon_swap\"");
        }
        strncpy(s_beacon_seen, ls->beacon, sizeof(s_beacon_seen) - 1);
        s_beacon_seen[sizeof(s_beacon_seen) - 1] = '\0';
    } else if (!live) {
        s_beacon_seen[0] = '\0';
    }
}

static void plan_phase(int64_t now)
{
    update_track_target();
    const drift_profile_t *prof = drift_profile();
    phase_cfg_t cfg = {
        .watch_gap_us = 5 * 1000 * 1000,
        .rate_ppm     = prof->dev_rate_ppm,
        .enabled      = prof->phase_track_enabled,
        .refine       = prof->phase_refine_enabled,
    };
    phase_config(&s_ph, &cfg);

    s_ph_due_us = 0;
    if (!s_ph.day[0]) {
        /* no folder to watch yet — hunt for the first day at watch
         * cadence rather than waiting a full coverage round (§3.1)  */
        static int64_t s_next_disc_us;
        if (now >= s_next_disc_us) {
            s_next_disc_us = now + 5LL * 1000 * 1000;
            sched_job_t p = { .kind = JOB_PARENT, .prio = 2,
                              .required = true };
            sched_submit(&p);
        }
        return;
    }

    int64_t due, gap;
    if (!phase_due(&s_ph, now, svc_median_us(), &due, &gap)) {
        return;
    }
    s_ph_due_us = due;                 /* loop wake hint             */
    if (now < due) {
        return;
    }
    sched_job_t d = { .kind = JOB_DAY, .prio = 2,
                      .min_gap_us = (uint32_t)gap,
                      .deadline_us = due + (gap ? gap :
                                             REQ_GAP_NORMAL_US),
                      .phase_job = true };
    memcpy(d.dir, s_ph.day, CAT_DAY_STR);
    if (sched_submit(&d) != ESP_OK) {
        phase_fail(&s_ph, now);       /* owner notification on drop   */
    }
}

/* -------- coverage round (§6.2/§6.3, update2 §3.5) --------
 * Once a minute: the parent plus the hot days EXCEPT the tracked one
 * — the phase pulls cover the tracked folder several-fold. Round
 * completeness still gates OUT/transfers: a round is incomplete when
 * a coverage job failed OR the tracked folder went silent beyond the
 * longest scheduled inter-window gap (~55 s in MAINTAIN, so 75 s).  */

static void plan_cover(int64_t now)
{
    if (now < s_next_cover_us) {
        return;
    }
    s_next_cover_us = now + COVER_PERIOD_US;

    /* close the previous round: growth was already reported live via
     * therapy_on_growth; here we only report quiet / interrupted
     * rounds                                                        */
    if (s_round_active) {
        bool complete = !s_round_failed;
        if (complete && s_ph.day[0]) {
            int64_t last = cat_day_last_scan(s_ph.day);
            if (!last || now - last > TRACK_FRESH_US) {
                complete = false;   /* tracked folder went silent    */
            }
        }
        if (!complete) {
            therapy_on_gap();
        } else if (!s_round_growth) {
            therapy_on_quiet_round(now);
        }
        /* poll evidence: one bounded record per closed round (§9)   */
        char rec[96];
        snprintf(rec, sizeof(rec),
                 "\"round\":\"%s\",\"n8h\":%lu",
                 !complete ? "fail" :
                    (s_round_growth ? "growth" : "quiet"),
                 (unsigned long)sched_active_8h());
        jrn_put("poll", rec);
    }
    s_round_active = true;
    s_round_growth = false;
    s_round_failed = false;

    sched_job_t p = { .kind = JOB_PARENT, .prio = 3,
                      .deadline_us = now + COVER_PERIOD_US,
                      .required = true };
    sched_submit(&p);

    char hot[4][CAT_DAY_STR];
    int nh = cat_hot_days(hot, 4);
    for (int i = 0; i < nh; i++) {
        if (s_ph.day[0] && !strcmp(hot[i], s_ph.day)) {
            continue;                /* covered by the phase pulls    */
        }
        sched_job_t d = { .kind = JOB_DAY, .prio = 3,
                          .deadline_us = now + COVER_PERIOD_US,
                          .required = true };
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

static bool q_same_target(const qjob_t *a, const qjob_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case JOB_DAY:
        return !strncmp(a->dir, b->dir, CAT_DAY_STR);
    case JOB_GET_FILE:
    case JOB_LIST_LIVE:
        return !strncmp(a->dir, b->dir, JOB_DIR_MAX) &&
               !strncmp(a->name, b->name, JOB_NAME_MAX);
    default:
        return true;
    }
}

static void q_drop(qjob_t *j, esp_err_t err)
{
    if (j->phase_job) {
        phase_fail(&s_ph, esp_timer_get_time());
    }
    if (j->done) {
        sched_result_t r = { .err = err };
        j->done(&r, j->ctx);
    }
}

/* Merge an equivalent queued target rather than enqueueing duplicate
 * card work. If the queue is momentarily full, a merge still succeeds;
 * unrelated work fails honestly and its owner is notified.           */
esp_err_t sched_submit(const sched_job_t *job)
{
    if (!job) return ESP_ERR_INVALID_ARG;
    qjob_t q = {
        .kind = job->kind, .prio = job->prio, .ctx = job->ctx,
        .done = job->done, .min_gap_us = job->min_gap_us,
        .deadline_us = job->deadline_us,
        .required = job->required, .phase_job = job->phase_job,
    };
    strncpy(q.dir, job->dir, JOB_DIR_MAX - 1);
    strncpy(q.name, job->name, JOB_NAME_MAX - 1);

    qjob_t held[Q_LEN];
    int n = 0;
    bool merged = false;
    qjob_t old;
    while (xQueueReceive(s_q, &old, 0) == pdTRUE) {
        if (!merged && q_same_target(&old, &q)) {
            if (q.prio < old.prio) old.prio = q.prio;
            if (q.min_gap_us && (!old.min_gap_us ||
                                 q.min_gap_us < old.min_gap_us)) {
                old.min_gap_us = q.min_gap_us;
            }
            if (q.deadline_us && (!old.deadline_us ||
                                  q.deadline_us < old.deadline_us)) {
                old.deadline_us = q.deadline_us;
            }
            old.required |= q.required;
            old.phase_job |= q.phase_job;
            merged = true;
        }
        held[n++] = old;
    }
    esp_err_t err = ESP_OK;
    if (!merged) {
        if (n < Q_LEN) {
            held[n++] = q;
        } else {
            err = ESP_ERR_NO_MEM;
            q_drop(&q, err);
        }
    }
    for (int i = 0; i < n; i++) {
        if (xQueueSend(s_q, &held[i], 0) != pdTRUE) {
            q_drop(&held[i], ESP_ERR_NO_MEM);
            err = ESP_ERR_NO_MEM;
        }
    }
    return err;
}

/* Select by priority among jobs whose inter-request release has
 * arrived; an expired required deadline wins over optional precision.
 * Jobs are drained into a bounded snapshot so a low-priority job at the
 * FIFO head cannot block a phase pull queued behind it.               */
static int q_pick(qjob_t *jobs, int n, int64_t now, int64_t *next_when)
{
    int64_t med = svc_median_us();
    int64_t first_when = INT64_MAX;
    int best = -1;
    int64_t best_deadline = INT64_MAX;
    for (int i = 0; i < n; i++) {
        int64_t when = earliest_start(now, &jobs[i]);
        if (when < first_when) first_when = when;
        if (when > now) continue;
        bool overdue = jobs[i].deadline_us &&
                       now + (med ? med : 0) > jobs[i].deadline_us;
        if (overdue && (best < 0 ||
            (jobs[i].required && !jobs[best].required) ||
            (jobs[i].required == jobs[best].required &&
             jobs[i].deadline_us < best_deadline))) {
            best = i;
            best_deadline = jobs[i].deadline_us;
        }
    }
    if (best < 0) {
        for (int i = 0; i < n; i++) {
            if (earliest_start(now, &jobs[i]) > now) continue;
            if (best < 0 || jobs[i].prio < jobs[best].prio ||
                (jobs[i].prio == jobs[best].prio &&
                 jobs[i].deadline_us &&
                 (!jobs[best].deadline_us ||
                  jobs[i].deadline_us < jobs[best].deadline_us))) {
                best = i;
            }
        }
    }
    if (next_when) *next_when = first_when;
    return best;
}

static int q_drain(qjob_t *jobs)
{
    int n = 0;
    while (n < Q_LEN &&
           xQueueReceive(s_q, &jobs[n], 0) == pdTRUE) {
        n++;
    }
    return n;
}

static void q_requeue(qjob_t *jobs, int n, int skip)
{
    for (int i = 0; i < n; i++) {
        if (i == skip) continue;
        if (xQueueSend(s_q, &jobs[i], 0) != pdTRUE) {
            q_drop(&jobs[i], ESP_ERR_NO_MEM);
        }
    }
}

void sched_run_forever(void)
{
    s_next_plan_us = 0;
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

        /* journal phase-tracker transitions and misses (update2 §7) */
        if ((int)s_ph.st != s_ph_logged ||
            s_ph.n_misses != s_ph_miss_logged) {
            char rec[112];
            snprintf(rec, sizeof(rec),
                     "\"st\":\"%s\",\"day\":\"%.8s\",\"brk\":%lu,"
                     "\"miss\":%lu,\"iunc_ms\":%lld,\"per_ms\":%lld",
                     phase_state_name(s_ph.st), s_ph.day,
                     (unsigned long)s_ph.n_brackets,
                     (unsigned long)s_ph.n_misses,
                     (long long)(s_ph.iunc_us / 1000),
                     (long long)(s_ph.period_us / 1000));
            jrn_put("phase", rec);
            s_ph_logged = (int)s_ph.st;
            s_ph_miss_logged = s_ph.n_misses;
        }

        qjob_t jobs[Q_LEN];
        qjob_t first;
        bool have = xQueueReceive(s_q, &first, pdMS_TO_TICKS(50)) == pdTRUE;
        int njobs = 0;
        if (have) {
            jobs[njobs++] = first;
            njobs += q_drain(jobs + njobs);
        }

        if (!have && now >= s_next_plan_us) {
            /* planner cadence: 500 ms idle, tightened to the phase
             * tracker's next due while a window is armed            */
            s_next_plan_us = now + 500 * 1000;
            if (s_link_up) {
                plan_phase(now);
                plan_cover(now);
                plan_cold(now);
                plan_neigh(now);
                if (s_ph_due_us > now && s_ph_due_us < s_next_plan_us) {
                    s_next_plan_us = s_ph_due_us;
                }
            }
            continue;
        }
        if (!have) {
            continue;
        }

        int64_t next_when = now;
        int pick = q_pick(jobs, njobs, now, &next_when);
        if (pick < 0) {
            q_requeue(jobs, njobs, -1);
            int64_t wait = next_when > now ? next_when - now : 1000;
            if (wait > 500000) wait = 500000;
            vTaskDelay(pdMS_TO_TICKS(wait / 1000 + 1));
            continue;
        }
        qjob_t j = jobs[pick];
        q_requeue(jobs, njobs, pick);

        /* §6.1 + update2 §5: requests suspend when the budget is
         * exhausted OR the last checkpoint is older than 60 s. The
         * ledger counts card requests in every therapy state —
         * OUT-mode reads are cheap but never accounted as free.     */
        bool checkpoint_fresh =
            now - s_ckpt_ok_us < CKPT_PERIOD_US;
        if (!sched_budget_ok() || !checkpoint_fresh) {
            q_drop(&j, ESP_ERR_NO_MEM);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_last_req_us = esp_timer_get_time();
        if (j.phase_job) {
            phase_submitted(&s_ph);   /* pull token consumed on start */
        }
        sched_result_t r;
        exec_job(&j, &r);
        if (j.kind == JOB_PARENT || j.kind == JOB_DAY ||
            j.kind == JOB_VERSION) {
            ledger_add(true, esp_timer_get_time());
        }
        if (r.entries) {
            free(r.entries);
        }
    }
}
