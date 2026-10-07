/*
 * SomnoBridge — volatile DATALOG catalog (spec §5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Folder array is fixed-size; file entries live in a PSRAM arena.
 * cat_feed_* is called by the card worker; queries copy under a short
 * critical section (FreeRTOS spinlock / plain on host).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "ezs_parse.h"

/* bounded copy that always NUL-terminates; GCC 14 promotes the
 * truncating forms (strncpy cap-1 / snprintf %s) to errors            */
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    const char *e = memchr(src, 0, cap - 1);
    size_t n = e ? (size_t)(e - src) : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}


#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
#define LOCK()    xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK()  xSemaphoreGive(s_lock)
static void *cat_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static int64_t now_stub(void) { return esp_timer_get_time(); }
#define CAT_LOG(...)  ESP_LOGI("cat", __VA_ARGS__)
#else
#ifdef CAT_HOST_THREADS
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()    pthread_mutex_lock(&s_lock)
#define UNLOCK()  pthread_mutex_unlock(&s_lock)
#else
static int s_lock_dummy;
#define LOCK()    ((void)s_lock_dummy)
#define UNLOCK()  ((void)s_lock_dummy)
#endif
static void *cat_alloc(size_t n) { return malloc(n); }
static int64_t now_stub(void) { return 0; }
#define CAT_LOG(...)  ((void)0)
#endif

#define CAT_MAX_FOLDERS  366    /* ~1yr of days, hard ceiling         */

typedef struct {
    char     day[CAT_DAY_STR];
    cat_folder_state_t state;
    int64_t  last_scan_us;
    bool     stale;
    int      err_count;
    cat_file_t *files;            /* PSRAM array, n_cap capacity       */
    int      n_files;
    int      n_cap;
    bool     hot;                 /* currently within base±1 window  */
    bool     active;              /* observed growth — stays monitored */
} cat_folder_t;

static cat_folder_t s_folders[CAT_MAX_FOLDERS];
static int          s_nfolders;
static int          s_psram_used;
static cat_state_t  s_state = CAT_SCANNING;
static char         s_base[CAT_DAY_STR];  /* "" when time unknown      */
static char         s_base_src[16] = "none"; /* "time"|"card_latest"  */
static bool         s_date_uncertain = true;

/* ---------- day helpers ---------- */

static int day_cmp(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);   /* newest first */
}

/* add/subtract days from YYYYMMDD (calendar arithmetic, §5.1) */
static void day_offset(const char in[CAT_DAY_STR], int off,
                       char out[CAT_DAY_STR])
{
    int y = (in[0]-'0')*1000 + (in[1]-'0')*100 + (in[2]-'0')*10 + in[3]-'0';
    int m = (in[4]-'0')*10 + (in[5]-'0');
    int d = (in[6]-'0')*10 + (in[7]-'0');
    /* civil-from-days / days-from-civil (Hinnant) */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    long days = era * 146097L + doe - 719468L + off;
    days += 719468L;   /* civil_from_days maps back to 0000-03-01 era */
    era = (int)((days >= 0 ? days : days - 146096) / 146097);
    doe = days - era * 146097L;
    yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    d = doy - (153*mp + 2)/5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    if (y < 0 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
        memset(out, 0, CAT_DAY_STR);
        return;
    }
    out[0] = '0' + (y / 1000) % 10;
    out[1] = '0' + (y / 100) % 10;
    out[2] = '0' + (y / 10) % 10;
    out[3] = '0' + y % 10;
    out[4] = '0' + (m / 10) % 10;
    out[5] = '0' + m % 10;
    out[6] = '0' + (d / 10) % 10;
    out[7] = '0' + d % 10;
    out[8] = '\0';
}

static cat_folder_t *find(const char day[CAT_DAY_STR])
{
    for (int i = 0; i < s_nfolders; i++) {
        if (!strcmp(s_folders[i].day, day)) {
            return &s_folders[i];
        }
    }
    return NULL;
}

static cat_folder_t *add_folder(const char day[CAT_DAY_STR])
{
    cat_folder_t *f = find(day);
    if (f) {
        return f;
    }
    if (s_nfolders >= CAT_MAX_FOLDERS) {
        return NULL;
    }
    f = &s_folders[s_nfolders++];
    memset(f, 0, sizeof(*f));
    strncpy(f->day, day, CAT_DAY_STR - 1);
    f->state = CAT_FOLDER_UNSCANNED;
    return f;
}

static void free_files(cat_folder_t *f)
{
    if (f->files) {
        free(f->files);
        s_psram_used -= f->n_cap * (int)sizeof(cat_file_t);
        f->files = NULL;
        f->n_cap = 0;
        f->n_files = 0;
    }
}

static void refresh_state(void)
{
    int unresident = 0, known = 0;
    for (int i = 0; i < s_nfolders; i++) {
        if (s_folders[i].state != CAT_FOLDER_GONE) {
            known++;
            if (s_folders[i].state != CAT_FOLDER_RESIDENT) {
                unresident++;
            }
        }
    }
    if (unresident == 0 && known > 0) {
        s_state = CAT_READY;
    } else if (unresident > 0 && s_nfolders > 0) {
        /* INCOMPLETE only when the budget, not laziness, is the cause */
        s_state = (s_psram_used >= CAT_PS_BUDGET)
                      ? CAT_INCOMPLETE : CAT_SCANNING;
    } else {
        s_state = CAT_SCANNING;
    }
}

/* ---------- public ---------- */

void cat_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
#endif
    LOCK();
    for (int i = 0; i < s_nfolders; i++) free_files(&s_folders[i]);
    memset(s_folders, 0, sizeof(s_folders));
    s_nfolders = 0;
    s_psram_used = 0;
    s_state = CAT_SCANNING;
    s_base[0] = '\0';
    strcpy(s_base_src, "none");
    s_date_uncertain = true;
    UNLOCK();
}

void cat_set_hot_base(const char base[CAT_DAY_STR], const char *source)
{
    LOCK();
    if (base && base[0]) {
        strncpy(s_base, base, CAT_DAY_STR - 1);
        s_base[CAT_DAY_STR-1] = '\0';
        snprintf(s_base_src, sizeof(s_base_src), "%s",
                 source ? source : "time");
        s_date_uncertain = !strcmp(s_base_src, "card_latest");
    } else {
        s_base[0] = '\0';
        s_date_uncertain = true;
    }
    /* recompute hot flags */
    char minus[CAT_DAY_STR], plus[CAT_DAY_STR];
    if (s_base[0]) {
        day_offset(s_base, -1, minus);
        day_offset(s_base, +1, plus);
    }
    for (int i = 0; i < s_nfolders; i++) {
        s_folders[i].hot = s_base[0] &&
            (!strcmp(s_folders[i].day, s_base) ||
             !strcmp(s_folders[i].day, minus) ||
             !strcmp(s_folders[i].day, plus));
    }
    UNLOCK();
}

esp_err_t cat_feed_parent(const ezs_entry_t *entries, int n,
                          int64_t now_us)
{
    LOCK();
    /* mark every non-GONE folder as candidate-GONE; revive those seen */
    bool seen[CAT_MAX_FOLDERS] = {0};
    int known = 0;
    for (int i = 0; i < n; i++) {
        if (!entries[i].is_dir || !ezs_is_valid_day(entries[i].name)) {
            continue;
        }
        cat_folder_t *f = find(entries[i].name);
        if (!f) {
            f = add_folder(entries[i].name);
            if (!f) {
                UNLOCK();
                return ESP_ERR_NO_MEM;
            }
        } else {
            int idx = (int)(f - s_folders);
            seen[idx] = true;
            if (f->state == CAT_FOLDER_GONE) {
                f->state = CAT_FOLDER_UNSCANNED;
            }
        }
        known++;
    }
    /* folders absent from a good parent listing -> GONE */
    for (int i = 0; i < s_nfolders; i++) {
        if (!seen[i] && s_folders[i].state != CAT_FOLDER_GONE &&
            s_folders[i].state != CAT_FOLDER_UNSCANNED) {
            /* keep UNSCANNED ones too — parent may have raced */
        }
        if (!seen[i] && find(s_folders[i].day) &&
            s_folders[i].state == CAT_FOLDER_RESIDENT &&
            s_folders[i].last_scan_us > 0) {
            /* only demote folders that were ever confirmed present    */
        }
        if (!seen[i] && s_folders[i].state != CAT_FOLDER_UNSCANNED &&
            s_folders[i].last_scan_us > 0) {
            CAT_LOG("parent: %s not seen -> GONE (was state=%d)",
                    s_folders[i].day, (int)s_folders[i].state);
            s_folders[i].state = CAT_FOLDER_GONE;
            free_files(&s_folders[i]);
        }
    }
    /* seed hot fallback when base unknown: newest day becomes base    */
    if (!s_base[0] && s_nfolders > 0) {
        char newest[CAT_DAY_STR] = "";
        for (int i = 0; i < s_nfolders; i++) {
            if (s_folders[i].state != CAT_FOLDER_GONE &&
                strcmp(s_folders[i].day, newest) > 0) {
                strcpy(newest, s_folders[i].day);
            }
        }
        if (newest[0]) {
            UNLOCK();
            cat_set_hot_base(newest, "card_latest");
            LOCK();
        }
    }
    (void)known; (void)now_us;
    refresh_state();
    UNLOCK();
    return ESP_OK;
}

/* Emit one change event into the diff (bounded; overflow is flagged
 * rather than silently dropped — the estimator must not see a partial
 * truth, §11.2). */
static void diff_emit(cat_diff_t *d, cat_ev_kind_t kind,
                      const cat_file_t *oldf, const cat_file_t *newf)
{
    if (d->n_events >= CAT_EV_CAP) {
        d->overflowed = true;
        return;
    }
    cat_event_t *e = &d->ev[d->n_events++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    const cat_file_t *named = newf ? newf : oldf;
    copy_bounded(e->name, CAT_NAME_MAX, named->name);
    if (oldf) {
        e->old_size = oldf->size;
        e->old_stamp_ok = oldf->has_timestamp;
        copy_bounded(e->old_stamp, CAT_STAMP_MAX, oldf->timestamp_raw);
    }
    if (newf) {
        e->new_size = newf->size;
        e->new_stamp_ok = newf->has_timestamp;
        copy_bounded(e->new_stamp, CAT_STAMP_MAX, newf->timestamp_raw);
    }
}

esp_err_t cat_feed_day(const char day[CAT_DAY_STR],
                       const ezs_entry_t *entries, int n,
                       int64_t now_us, cat_diff_t *diff)
{
    if (diff) {
        memset(diff, 0, sizeof(*diff));
    }
    /* build new entry set in scratch first (atomic replace, §5.2) */
    cat_file_t *nf = NULL;
    if (n > 0) {
        nf = cat_alloc((size_t)n * sizeof(cat_file_t));
        if (!nf) {
            CAT_LOG("feed %s: alloc %u B failed (psram_used=%d)",
                     day, (unsigned)(n * sizeof(cat_file_t)), cat_psram_used());
            return ESP_ERR_NO_MEM;
        }
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) {
            continue;
        }
        strncpy(nf[w].name, entries[i].name, CAT_NAME_MAX - 1);
        nf[w].name[CAT_NAME_MAX - 1] = '\0';
        nf[w].size = entries[i].size;
        nf[w].size_unit_shift = entries[i].size_unit_shift;
        nf[w].has_timestamp = entries[i].has_timestamp;
        strncpy(nf[w].timestamp_raw, entries[i].timestamp_raw,
                CAT_STAMP_MAX - 1);
        nf[w].timestamp_raw[CAT_STAMP_MAX - 1] = '\0';
        strncpy(nf[w].dl_name, entries[i].dl_name,
                sizeof(nf[w].dl_name) - 1);
        nf[w].dl_name[sizeof(nf[w].dl_name) - 1] = '\0';
        w++;
    }

    LOCK();
    cat_folder_t *f = find(day);
    if (!f) {
        f = add_folder(day);
    }
    if (!f) {
        UNLOCK();
        free(nf);
        return ESP_ERR_NO_MEM;
    }
    if (diff) {
        diff->prev_scan_us = f->last_scan_us;
    }
    /* diff the resident set against the new listing; every tuple
     * difference is an update event (size, stamp, membership — U3)  */
    bool changed = (f->state != CAT_FOLDER_RESIDENT) ||
                   (w != f->n_files);
    if (f->state == CAT_FOLDER_RESIDENT) {
        for (int i = 0; i < w; i++) {
            const cat_file_t *old = NULL;
            for (int j = 0; j < f->n_files; j++) {
                if (!strcmp(f->files[j].name, nf[i].name)) {
                    old = &f->files[j];
                    break;
                }
            }
            if (!old) {
                if (diff) diff_emit(diff, CAT_EV_NEW, NULL, &nf[i]);
                changed = true;
                continue;
            }
            bool stamp_diff =
                nf[i].has_timestamp != old->has_timestamp ||
                (nf[i].has_timestamp &&
                 strcmp(nf[i].timestamp_raw, old->timestamp_raw));
            if (nf[i].size != old->size || stamp_diff) {
                changed = true;
                if (!diff) {
                    continue;
                }
                cat_ev_kind_t k = stamp_diff ? CAT_EV_STAMP
                    : (nf[i].size > old->size ? CAT_EV_GREW
                                              : CAT_EV_SHRUNK);
                diff_emit(diff, k, old, &nf[i]);
            }
        }
        for (int j = 0; j < f->n_files; j++) {
            bool gone = true;
            for (int i = 0; i < w; i++) {
                if (!strcmp(f->files[j].name, nf[i].name)) {
                    gone = false;
                    break;
                }
            }
            if (gone) {
                if (diff) diff_emit(diff, CAT_EV_GONE, &f->files[j], NULL);
                changed = true;
            }
        }
    }
    if (changed) {
        f->active = true;         /* a folder that ever wrote stays hot */
        if (diff) {
            diff->changed = true;
        }
    }
    /* budget enforcement: evict oldest cold folders if over ceiling */
    free_files(f);
    f->files = nf;
    f->n_cap = n;
    s_psram_used += n * (int)sizeof(cat_file_t);
    f->n_files = w;
    f->state = CAT_FOLDER_RESIDENT;
    f->stale = false;
    f->err_count = 0;
    f->last_scan_us = now_us ? now_us : now_stub();

    while (s_psram_used > CAT_PS_BUDGET) {
        /* evict oldest non-hot non-active RESIDENT folder */
        cat_folder_t *victim = NULL;
        for (int i = 0; i < s_nfolders; i++) {
            cat_folder_t *c = &s_folders[i];
            if (c->state == CAT_FOLDER_RESIDENT && !c->hot &&
                !c->active &&
                (!victim || c->last_scan_us < victim->last_scan_us)) {
                victim = c;
            }
        }
        if (!victim) {
            break;                    /* over-budget on hot data      */
        }
        free_files(victim);
        victim->state = CAT_FOLDER_EVICTED;
    }
    refresh_state();
    UNLOCK();
    return ESP_OK;
}

void cat_mark_folder_stale(const char day[CAT_DAY_STR])
{
    LOCK();
    cat_folder_t *f = find(day);
    if (f) {
        f->stale = true;
        f->err_count++;
    }
    UNLOCK();
}

cat_state_t cat_state(void)
{
    LOCK();
    cat_state_t state = s_state;
    UNLOCK();
    return state;
}

bool cat_catalog_complete(void) { return cat_state() == CAT_READY; }

int cat_folder_count(void)
{
    LOCK();
    int count = s_nfolders;
    UNLOCK();
    return count;
}

int cat_psram_used(void)
{
    LOCK();
    int used = s_psram_used;
    UNLOCK();
    return used;
}

bool cat_hot_ready(void)
{
    bool ok = true;
    LOCK();
    for (int i = 0; i < s_nfolders; i++) {
        if (s_folders[i].hot && s_folders[i].state != CAT_FOLDER_GONE &&
            s_folders[i].state != CAT_FOLDER_RESIDENT) {
            ok = false;
            break;
        }
    }
    UNLOCK();
    return ok;
}

int cat_incomplete_count(void)
{
    int c = 0;
    LOCK();
    for (int i = 0; i < s_nfolders; i++) {
        if (s_folders[i].state == CAT_FOLDER_UNSCANNED ||
            s_folders[i].state == CAT_FOLDER_EVICTED) {
            c++;
        }
    }
    UNLOCK();
    return c;
}

int cat_list_days(char (*days)[CAT_DAY_STR], int cap)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < s_nfolders && n < cap; i++) {
        if (s_folders[i].state != CAT_FOLDER_GONE) {
            copy_bounded(days[n], CAT_DAY_STR, s_folders[i].day);
            n++;
        }
    }
    UNLOCK();
    qsort(days, n, CAT_DAY_STR, day_cmp);
    return n;
}

bool cat_newest_day(char out[CAT_DAY_STR])
{
    bool found = false;
    memset(out, 0, CAT_DAY_STR);
    LOCK();
    for (int i = 0; i < s_nfolders; i++) {
        if (s_folders[i].state != CAT_FOLDER_GONE &&
            (!found || strcmp(s_folders[i].day, out) > 0)) {
            copy_bounded(out, CAT_DAY_STR, s_folders[i].day);
            found = true;
        }
    }
    UNLOCK();
    return found;
}

int cat_hot_days(char (*days)[CAT_DAY_STR], int cap)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < s_nfolders && n < cap; i++) {
        if (s_folders[i].hot && s_folders[i].state != CAT_FOLDER_GONE) {
            copy_bounded(days[n], CAT_DAY_STR, s_folders[i].day);
            n++;
        }
    }
    UNLOCK();
    qsort(days, n, CAT_DAY_STR, day_cmp);
    return n;
}

int cat_scan_queue(char (*days)[CAT_DAY_STR], int cap)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < s_nfolders && n < cap; i++) {
        if (s_folders[i].state == CAT_FOLDER_UNSCANNED ||
            s_folders[i].stale) {
            copy_bounded(days[n], CAT_DAY_STR, s_folders[i].day);
            n++;
        }
    }
    UNLOCK();
    qsort(days, n, CAT_DAY_STR, day_cmp);
    return n;
}

int cat_day_entries(const char day[CAT_DAY_STR],
                    cat_file_t *out, int cap)
{
    int n = 0;
    LOCK();
    cat_folder_t *f = find(day);
    if (f && f->state == CAT_FOLDER_RESIDENT) {
        n = f->n_files;
        if (out) {
            int cp = n < cap ? n : cap;
            if (cp > 0) {
                memcpy(out, f->files, cp * sizeof(cat_file_t));
            }
        }
    } else if (f) {
        n = -(int)f->state - 1;   /* negative encodes unavailability  */
    } else {
        n = -99;                  /* unknown day                      */
    }
    UNLOCK();
    return n;
}

cat_folder_state_t cat_day_state(const char day[CAT_DAY_STR])
{
    cat_folder_state_t st = CAT_FOLDER_GONE;
    LOCK();
    cat_folder_t *f = find(day);
    if (f) {
        st = f->state;
    }
    UNLOCK();
    return st;
}

int64_t cat_day_last_scan(const char day[CAT_DAY_STR])
{
    int64_t t = 0;
    LOCK();
    cat_folder_t *f = find(day);
    if (f) {
        t = f->last_scan_us;
    }
    UNLOCK();
    return t;
}

bool cat_find_file(const char day[CAT_DAY_STR], const char *name,
                   cat_file_t *out)
{
    bool found = false;
    LOCK();
    cat_folder_t *f = find(day);
    if (f && f->state == CAT_FOLDER_RESIDENT) {
        for (int i = 0; i < f->n_files; i++) {
            if (!strcmp(f->files[i].name, name)) {
                if (out) {
                    *out = f->files[i];
                }
                found = true;
                break;
            }
        }
    }
    UNLOCK();
    return found;
}
