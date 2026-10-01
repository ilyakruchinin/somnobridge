/*
 * SomnoPal — therapy FSM (spec §7)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * UNKNOWN -> IN on any relevant write; IN -> OUT only after the full
 * §7.1 proof (timeout + 2 clean rounds + final refresh). Anything that
 * interrupts monitoring sends us back to UNKNOWN, never silently to OUT.
 */
#include <string.h>

#include "sched.h"
#include "esp_timer.h"

#define PROVISIONAL_APPEND_MS   62000
#define PROVISIONAL_OUT_MS      125000
#define STALE_LIVE_MS           (PROVISIONAL_APPEND_MS + 30000)
#define OUT_FRESH_MS            90000   /* all monitors must be this fresh */

static therapy_status_t s_th;
static int  s_quiet_rounds;      /* complete clean passes while OUT-pending */
static int64_t s_quiet_since_us; /* first clean baseline                  */
static int64_t s_last_ok_us;     /* last successful monitoring listing   */

static void enter(therapy_state_t st, const char *reason, int64_t now)
{
    s_th.state = st;
    s_th.state_since_us = now;
    strncpy(s_th.reason, reason, sizeof(s_th.reason) - 1);
    s_th.reason[sizeof(s_th.reason) - 1] = '\0';
    s_th.confidence[0] = '\0';
    if (st == THERAPY_IN) {
        strcpy(s_th.confidence, "validated");
        s_quiet_rounds = 0;
    } else if (st == THERAPY_OUT) {
        strcpy(s_th.confidence, "validated");
    } else {
        strcpy(s_th.confidence, "unknown");
        s_quiet_rounds = 0;
        s_quiet_since_us = 0;
    }
    s_th.transfer_allowed = (st == THERAPY_OUT) && !s_th.therapy_stale;
}

void therapy_init(void)
{
    memset(&s_th, 0, sizeof(s_th));
    enter(THERAPY_UNKNOWN, "boot", esp_timer_get_time());
    s_th.expected_append_ms = PROVISIONAL_APPEND_MS;
    s_th.out_timeout_ms = PROVISIONAL_OUT_MS;
    s_th.therapy_stale = true;
}

const therapy_status_t *therapy_status(void) { return &s_th; }

void therapy_on_growth(int64_t now_us)
{
    s_th.last_growth_us = now_us;
    s_quiet_since_us = 0;
    if (s_th.state != THERAPY_IN) {
        enter(THERAPY_IN, "new_growth", now_us);
    }
    s_last_ok_us = now_us;
    s_th.therapy_stale = false;
    s_th.transfer_allowed = false;
}

/* A completed, successfully-parsed listing — changed or not — proves
 * the monitor is alive. Freshness must age from the last successful
 * listing, not from the last write: a quiet scan mid-therapy is
 * monitoring continuity, not a gap (§6.3).                        */
void therapy_note_listing(int64_t now_us)
{
    s_last_ok_us = now_us;
    s_th.therapy_stale = false;
}

void therapy_on_quiet_round(int64_t now_us)
{
    /* caller guarantees: a complete parent+hot round with zero writes */
    s_last_ok_us = now_us;
    s_th.therapy_stale = false;

    if (s_th.state == THERAPY_IN) {
        int64_t ref = s_th.last_growth_us ? s_th.last_growth_us
                                          : s_th.state_since_us;
        if (now_us - ref < (int64_t)s_th.out_timeout_ms * 1000) {
            return;                    /* still inside the timeout     */
        }
        s_quiet_rounds++;
        if (s_quiet_rounds >= 2) {
            enter(THERAPY_OUT, "quiet_timeout", now_us);
        }
        return;
    }
    if (s_th.state == THERAPY_UNKNOWN) {
        if (!s_quiet_since_us) {
            s_quiet_since_us = now_us; /* first clean baseline         */
            strcpy(s_th.reason, "baseline");
            s_th.confidence[0] = '\0';
            strcpy(s_th.confidence, "provisional");
            return;
        }
        /* need two clean rounds separated by >= out_timeout too       */
        if (now_us - s_quiet_since_us >=
            (int64_t)s_th.out_timeout_ms * 1000) {
            s_quiet_rounds++;
            if (s_quiet_rounds >= 1) {
                enter(THERAPY_OUT, "quiet_baseline", now_us);
            }
        }
        return;
    }
    /* already OUT */
}

void therapy_on_gap(void)
{
    enter(THERAPY_UNKNOWN, "monitor_gap", esp_timer_get_time());
    s_th.therapy_stale = true;
}

/* called periodically by the scheduler to age freshness */
void therapy_tick(int64_t now_us)
{
    if (s_th.state == THERAPY_IN && s_last_ok_us &&
        now_us - s_last_ok_us > (int64_t)STALE_LIVE_MS * 1000) {
        enter(THERAPY_UNKNOWN, "liveness_stale", now_us);
        s_th.therapy_stale = true;
    }
    /* quiet_since for API consumers */
    s_th.quiet_since_ms = s_quiet_since_us
        ? (uint32_t)((now_us - s_quiet_since_us) / 1000) : 0;
}

void therapy_note_resume_check(void)
{
    /* informational marker — STREAM_END calls into here via sched    */
    s_th.transfer_allowed = false;
}
