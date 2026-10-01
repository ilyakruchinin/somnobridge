/*
 * SomnoPal — card worker + therapy FSM + poll scheduler (spec §6/§7)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * ONE task owns all EZShare HTTP. Observation results feed catalog,
 * therapy FSM, estimator and journal. No other code may touch the card.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "catalog.h"
#include "ezshare.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- therapy FSM ---------------- */

typedef enum {
    THERAPY_UNKNOWN = 0,
    THERAPY_IN,
    THERAPY_OUT,
} therapy_state_t;

typedef struct {
    therapy_state_t state;
    bool     therapy_stale;        /* freshness of monitoring itself  */
    int64_t  state_since_us;       /* monotonic                       */
    int64_t  last_growth_us;       /* monotonic; 0 = none seen        */
    uint32_t quiet_since_ms;       /* monotonic-age of quiet baseline */
    uint32_t expected_append_ms;   /* effective deadline, ~62 s       */
    uint32_t out_timeout_ms;       /* ~125 s                          */
    char     confidence[12];       /* unknown|provisional|validated   */
    char     reason[40];           /* machine-readable code           */
    bool     transfer_allowed;
} therapy_status_t;

const therapy_status_t *therapy_status(void);
void  therapy_init(void);
void  therapy_tick(int64_t now_us);
void  therapy_on_growth(int64_t now_us);   /* any relevant write      */
void  therapy_note_listing(int64_t now_us); /* successful listing, changed or not */
void  therapy_on_quiet_round(int64_t now_us); /* full no-change pass  */
void  therapy_on_gap(void);                /* monitoring interruption */
void  therapy_note_resume_check(void);     /* stream END gate         */

/* ---------------- card worker ---------------- */

typedef enum {
    JOB_PARENT,        /* list A:\DATALOG                          */
    JOB_DAY,           /* list one day folder                       */
    JOB_GET_FILE,      /* full download (BLE stream)                */
    JOB_LIST_LIVE,     /* live non-DATALOG listing                  */
    JOB_VERSION,       /* /client?command=version                   */
} job_kind_t;

#define JOB_DIR_MAX   260
#define JOB_NAME_MAX  EZS_NAME_MAX

/* Total card-request ceiling in the rolling 8 h ledger (spec §6.1,
 * update2 §5). Counts parent+day listings and version probes in every
 * therapy state — reads are cheap but never accounted as free.       */
#define SCHED_ACTIVE_CAP_8H 8000

struct sched_result;
typedef struct {
    job_kind_t kind;
    int        prio;             /* lower = sooner (0 = post-check) */
    uint32_t   min_gap_us;       /* min start spacing; 0 = default  */
    int64_t    deadline_us;      /* latest useful completion; 0 none */
    bool       required;         /* monitoring safety, not optional  */
    bool       phase_job;        /* owns one phase pull token        */
    char       dir[JOB_DIR_MAX]; /* canonical A:\ dir or day name   */
    char       name[JOB_NAME_MAX];
    /* completion callback — runs on card worker context           */
    void (*done)(const struct sched_result *r, void *ctx);
    void      *ctx;
} sched_job_t;

typedef struct sched_result {
    esp_err_t    err;
    ezw_obs_t    obs;
    int          n_entries;
    ezs_entry_t *entries;        /* non-NULL only for listings      */
    uint64_t     bytes_sent;     /* for GET_FILE                    */
    char         day[CAT_DAY_STR];
} sched_result_t;

esp_err_t sched_init(void);
esp_err_t sched_submit(const sched_job_t *job);   /* copy is taken    */
void      sched_run_forever(void);                /* task body        */

/* planning inputs refreshed by the app loop */
void sched_set_card_ip(uint32_t ip);
void sched_set_link(bool up);

/* load ledger (spec §6.1) */
uint32_t sched_active_8h(void);        /* card-request count, 8 h    */
bool     sched_budget_ok(void);
uint32_t sched_backoff_ms(void);       /* current HTTP backoff       */

/* Phase-tracker status for GET_STATUS (update2 §7): emits
 * {"st":"maintain","day":"...",...} — phase precision is reported
 * separately from the drift record's offset half-width, never as
 * clock-offset accuracy. Returns bytes written, 0 on truncation.   */
int      sched_phase_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
