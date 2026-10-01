/*
 * SomnoPal — volatile DATALOG catalog (spec §5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Rebuildable PSRAM index of A:\DATALOG day folders + file tuples.
 * No persistence, no versions, no boot IDs — somnotrace owns sync state.
 * Pure C except esp_timer timestamps (shimmed for host tests).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#ifndef ESP_ERR_INVALID_SIZE
#define ESP_ERR_INVALID_SIZE 0x107
#endif
#endif

#include "ezs_parse.h"   /* ezs_entry_t */

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_NAME_MAX   EZS_NAME_MAX
#define CAT_STAMP_MAX  EZS_STAMP_MAX
#define CAT_DAY_STR    9           /* "YYYYMMDD" + NUL                */
#define CAT_PS_BUDGET  (768*1024)  /* PSRAM ceiling, spec §5.2        */

typedef struct {
    char     name[CAT_NAME_MAX];
    uint64_t size;
    uint8_t  size_unit_shift;     /* log2 display unit (0=B,10=K..)  */
    bool     has_timestamp;
    char     timestamp_raw[CAT_STAMP_MAX];
    char     dl_name[16];         /* card's 8.3 ?file= basename, ""  */
                                  /* when only fname= was listed     */
} cat_file_t;

/* Per-file change events from one complete feed (spec §7 update tuple:
 * a change in either of (size, stamp) is an update event — U3). The
 * estimator consumes these; the FSM consumes diff->changed.        */
typedef enum {
    CAT_EV_NEW = 0,    /* file appeared in the new listing            */
    CAT_EV_GREW,       /* size increased                              */
    CAT_EV_SHRUNK,     /* size decreased (discontinuity suspect)      */
    CAT_EV_STAMP,      /* timestamp changed (size same or differed)   */
    CAT_EV_GONE,       /* vanished from the resident set              */
} cat_ev_kind_t;

#define CAT_EV_CAP 12

typedef struct {
    cat_ev_kind_t kind;
    char     name[CAT_NAME_MAX];
    uint64_t old_size, new_size;
    bool     old_stamp_ok, new_stamp_ok;
    char     old_stamp[CAT_STAMP_MAX];
    char     new_stamp[CAT_STAMP_MAX];
} cat_event_t;

typedef struct {
    bool        changed;          /* any update event / membership   */
    int         n_events;
    bool        overflowed;       /* more events than CAT_EV_CAP     */
    int64_t     prev_scan_us;     /* folder's last_scan before this  */
                                  /* feed; 0 = first-ever listing    */
    cat_event_t ev[CAT_EV_CAP];
} cat_diff_t;

typedef enum {
    CAT_FOLDER_UNSCANNED = 0,  /* known to exist, not resident          */
    CAT_FOLDER_RESIDENT,       /* complete entry set in memory          */
    CAT_FOLDER_EVICTED,        /* was resident, evicted by budget       */
    CAT_FOLDER_GONE,           /* parent listing says absent            */
} cat_folder_state_t;

typedef enum {
    CAT_SCANNING = 0,
    CAT_READY,
    CAT_INCOMPLETE,
    CAT_ERROR,
} cat_state_t;

typedef struct {
    char     day[CAT_DAY_STR];
    cat_folder_state_t state;
    int64_t  last_scan_us;       /* monotonic; 0 = never              */
    bool     folder_stale;       /* last refresh failed               */
    int      n_files;
    /* entries only when RESIDENT */
} cat_day_view_t;

/* ---------------- lifecycle ---------------- */

void cat_init(void);

/** Base noon-day + hot window (base-1..base+1) as "YYYYMMDD" strings.
 * base may be NULL when time is unknown (card_latest fallback). */
void cat_set_hot_base(const char base[CAT_DAY_STR], const char *source);

/* ---------------- parent/child feed ---------------- */

/** Feed a parsed DATALOG parent listing (dir names). Creates folder
 * records for valid days, marks absent ones GONE. Returns error code. */
esp_err_t cat_feed_parent(const ezs_entry_t *entries, int n,
                          int64_t now_us);

/** Atomically replace a day's file set on successful listing.
 * `diff` (may be NULL) reports the complete-listing change set:
 * `changed` is the therapy FSM's "relevant write" signal (§7); the
 * per-file events carry old/new (size, timestamp_raw) tuples for the
 * timestamp-first estimator (U3/U4). Events are emitted only across a
 * RESIDENT->RESIDENT transition — a first-ever listing is a baseline,
 * not evidence of writes. `prev_scan_us` is the previous feed's scan
 * time (the estimator's "previous unchanged observation" bound).   */
esp_err_t cat_feed_day(const char day[CAT_DAY_STR],
                       const ezs_entry_t *entries, int n,
                       int64_t now_us, cat_diff_t *diff);

/** Mark a day's last refresh failed (keeps old entries, sets stale). */
void cat_mark_folder_stale(const char day[CAT_DAY_STR]);

/* ---------------- queries ---------------- */

cat_state_t cat_state(void);
bool        cat_catalog_complete(void);
bool        cat_hot_ready(void);          /* all existing hot resident */
int         cat_folder_count(void);

/** Sorted (newest-first) day names that exist (not GONE). */
int cat_list_days(char (*days)[CAT_DAY_STR], int cap);
/** Newest existing day over the whole table; false when none. Unlike a
 * capped list, the maximum is computed before any truncation. */
bool cat_newest_day(char out[CAT_DAY_STR]);

/** Copy a complete day's entries into caller buffer (NULL -> count). */
int cat_day_entries(const char day[CAT_DAY_STR],
                    cat_file_t *out, int cap);
cat_folder_state_t cat_day_state(const char day[CAT_DAY_STR]);
int64_t cat_day_last_scan(const char day[CAT_DAY_STR]);

/** Look up one file's live tuple (name match is exact). */
bool cat_find_file(const char day[CAT_DAY_STR], const char *name,
                   cat_file_t *out);

/* Which folders are "hot" right now (existing, in base±1 window). */
int cat_hot_days(char (*days)[CAT_DAY_STR], int cap);
/* Days needing a scan (unscanned/stale), newest first. */
int cat_scan_queue(char (*days)[CAT_DAY_STR], int cap);

int  cat_psram_used(void);   /* bytes of entry payload in PSRAM       */
int  cat_incomplete_count(void); /* known-but-unresident folders      */

#ifdef __cplusplus
}
#endif
