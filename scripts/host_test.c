/*
 * SomnoPal — host contract tests for pure-logic modules (spec §14.1)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Runs on the build host with stub ESP headers; no hardware needed.
 * Suites: EZShare parser/path, catalog/date rules, therapy FSM,
 * BLE framing, drift anchors/fit. Synthetic fixtures only.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezs_parse.h"
#include "ezs_path.h"
#include "catalog.h"
#include "sched.h"
#include "ble_proto.h"
#include "drift.h"
#include "edf_hdr.h"
#include "journal.h"
#include "nvs_flash.h"
#include "settings.h"

int64_t host_now_us;   /* drives esp_timer_get_time() stub */

/* ---------------- fake NVS KV store ---------------- */

#define FKV_MAX 300
static struct { char key[16]; uint8_t *data; size_t len; } s_kv[FKV_MAX];
static int  s_kv_n;
static int  s_fail_writes;

const void *settings_sec_cfg(void)
{
    static int dummy;
    return &dummy;
}
nvs_handle_t settings_nvs_metadata(void) { return 1; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "ERR"; }

static int kv_find(const char *key)
{
    for (int i = 0; i < s_kv_n; i++)
        if (!strcmp(s_kv[i].key, key)) return i;
    return -1;
}

void nvs_stub_reset(void)
{
    for (int i = 0; i < s_kv_n; i++) free(s_kv[i].data);
    s_kv_n = 0;
    s_fail_writes = 0;
}
void nvs_stub_corrupt(const char *key)
{
    int i = kv_find(key);
    if (i >= 0 && s_kv[i].len) s_kv[i].data[s_kv[i].len - 1] ^= 0xFF;
}
void nvs_stub_fail_writes(int n) { s_fail_writes = n; }

esp_err_t nvs_flash_secure_init_partition(const char *p, void *c)
{
    (void)p; (void)c; return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *p, const char *ns,
                                  int m, nvs_handle_t *out)
{
    (void)p; (void)ns; (void)m; *out = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out,
                       size_t *len)
{
    (void)h;
    int i = kv_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = s_kv[i].len; return ESP_OK; }
    if (*len < s_kv[i].len) { *len = s_kv[i].len;
        return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, s_kv[i].data, s_kv[i].len);
    *len = s_kv[i].len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in,
                       size_t len)
{
    (void)h;
    if (s_fail_writes > 0) { s_fail_writes--; return ESP_FAIL; }
    int i = kv_find(key);
    if (i < 0) {
        if (s_kv_n >= FKV_MAX) return ESP_ERR_NO_MEM;
        i = s_kv_n++;
        memset(s_kv[i].key, 0, sizeof(s_kv[i].key));
        strncpy(s_kv[i].key, key, sizeof(s_kv[i].key) - 1);
        s_kv[i].data = NULL;
    }
    free(s_kv[i].data);
    s_kv[i].data = malloc(len);
    memcpy(s_kv[i].data, in, len);
    s_kv[i].len = len;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    uint32_t v; size_t l = sizeof(v);
    esp_err_t e = nvs_get_blob(h, key, &v, &l);
    if (e == ESP_OK && l == sizeof(v)) { *out = v; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{
    return nvs_set_blob(h, key, &v, sizeof(v));
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    int i = kv_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    free(s_kv[i].data);
    s_kv[i] = s_kv[--s_kv_n];
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h;
    for (int i = 0; i < s_kv_n; i++) free(s_kv[i].data);
    s_kv_n = 0;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

static int fails;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fails++; \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ---------------- EZShare parser ---------------- */

/* grammar per the live card (see ezs_parse.c):
 *   <ts>  <size|<DIR>>  <a href="...">name</a>
 * timestamp fields are space padded, <DIR> arrives entity-escaped and
 * the link text is padded — all captured from a real EZShare card.   */
static const char *k_listing =
    "<html><body><h1>Directory Index of A:\\DATALOG</h1><pre>\r\n"
    "   2026- 8-14    4: 8:32         &lt;DIR&gt;   "
    "<a href=\"dir?dir=A:%5CDATALOG%5C20240101\"> 20240101</a>\r\n"
    "   2026- 8-14    4: 8:32         &lt;DIR&gt;   "
    "<a href=\"dir?dir=A:\"> ..</a>\r\n"
    "   2024- 1- 2    3: 4: 5             1234   "
    "<a href=\"download?fname=STR.edf&fdir=..\">STR.edf</a>\r\n"
    "   2026- 9-30   13:57:44           1KB   "
    "<a href=\"http://192.168.4.1/download?file=DATALOG%5C20240101%5C202609~1.EDF\">"
    " STR.edf</a>\r\n"
    "   2026- 9-30   14: 9:52          72KB   "
    "<a href=\"http://192.168.4.1/download?file=DATALOG%5C20240101%5C202609~5.EDF\">"
    " 20240101_235752_BRP.edf</a>\r\n"
    "Total Entries: 4.Total Size: 73KB.\r\n"
    "</pre></body></html>";

static void test_parser(void)
{
    ezs_entry_t e[8];
    int n = 0;
    CHECK(ezs_parse_dir(k_listing, e, &n) == EZS_OK, "valid listing");
    CHECK(n == 4, "four entries (nav row skipped)");
    if (n == 4) {
        /* row 0 = dir; row 1 = documented ?fname= form (raw bytes);
         * rows 2-3 = the real card: absolute href, ?file=, KB-rounded
         * size and the alias carried in the link text */
        CHECK(e[0].is_dir && !strcmp(e[0].name, "20240101"),
              "dir row + trimmed name");
        CHECK(!strcmp(e[1].name, "STR.edf") && e[1].size == 1234,
              "fname= form keeps raw bytes");
        CHECK(!strcmp(e[2].name, "STR.edf") && e[2].size == 1024,
              "?file= form: KB size + alias from link text");
        CHECK(!strcmp(e[3].name, "20240101_235752_BRP.edf") &&
              e[3].size == 72 * 1024, "72KB row");
        CHECK(e[2].has_timestamp, "file timestamp kept");
        /* size units: raw bytes -> shift 0; 1KB/72KB -> shift 10     */
        CHECK(e[1].size_unit_shift == 0, "raw byte size unit");
        CHECK(e[2].size_unit_shift == 10 &&
              e[3].size_unit_shift == 10, "KiB size unit");
        /* download aliases: ?file= 8.3 basename retained; the fname=
         * form has none and falls back at download time (U8)         */
        CHECK(!strcmp(e[2].dl_name, "202609~1.EDF") &&
              !strcmp(e[3].dl_name, "202609~5.EDF"),
              "?file= basename captured");
        CHECK(e[1].dl_name[0] == '\0', "fname= row has no dl_name");
    }
    /* ceil-unit envelope (U8): exact <= listed < exact + unit        */
    CHECK(ezs_size_within_listing(2048, 10, 1992), "1992 inside 2KB");
    CHECK(ezs_size_within_listing(2048, 10, 2048), "exact at bound");
    CHECK(!ezs_size_within_listing(2048, 10, 1024), "1024 outside 2KB");
    CHECK(!ezs_size_within_listing(2048, 10, 2049), "over listed");
    CHECK(ezs_size_within_listing(1992, 0, 1992), "raw bytes exact");
    CHECK(!ezs_size_within_listing(1992, 0, 1993), "raw bytes strict");
    char file[256], token[24];
    CHECK(ezs_find_download_file(k_listing, "A:\\DATALOG\\20240101",
              "20240101_235752_BRP.edf", file, sizeof(file), token,
              sizeof(token)), "resolve listing-derived download file");
    CHECK(!strcmp(file, "DATALOG\\20240101\\202609~5.EDF") &&
          !strcmp(token, "72KB"), "short-name path and size token retained");
    CHECK(!ezs_find_download_file(k_listing, "A:\\DATALOG\\20240102",
              "20240101_235752_BRP.edf", file, sizeof(file), token,
              sizeof(token)), "reject download path in another directory");
    CHECK(!ezs_find_download_file(k_listing, "A:\\DATALOG\\20240101",
              "missing.edf", file, sizeof(file), token, sizeof(token)),
              "reject missing download alias");
    const char *traversal =
        "<pre>2024-01-01 00:00:00 1KB "
        "<a href=\"download?file=DATALOG%5C20240101%5C..%5CSTR.edf\">X.edf</a>\n</pre>";
    CHECK(!ezs_find_download_file(traversal, "A:\\DATALOG\\20240101",
              "X.edf", file, sizeof(file), token, sizeof(token)),
              "reject listing-derived traversal");

    const char *login = "<html><form action=\"/login\"></form></html>";
    CHECK(ezs_parse_dir(login, e, &n) == EZS_ERR_NOT_LISTING,
          "login page rejected");

    static char trunc[200];
    size_t cut = 90;                          /* inside a row         */
    memcpy(trunc, k_listing, cut);
    trunc[cut] = '\0';
    CHECK(ezs_parse_dir(trunc, e, &n) != EZS_OK, "truncated rejected");

    const char *evil =
        "<pre> 2024-01-01 00:00:00 10 "
        "<a href=\"http://evil/x\">x.bin</a></pre>";
    CHECK(ezs_parse_dir(evil, e, &n) != EZS_OK, "external link rejected");
}

/* ---------------- path canonicalizer ---------------- */

static void test_paths(void)
{
    char out[280];
    CHECK(ezp_canon_dir("A:\\DATALOG\\20240101", out, sizeof(out)) == EZP_OK,
          "canonical ok");
    CHECK(!strcmp(out, "A:\\DATALOG\\20240101"), "unchanged");
    CHECK(ezp_canon_dir("a:/datalog", out, sizeof(out)) == EZP_OK,
          "slashes normalized");
    /* §11.1: drive uppercases, component case is preserved —
     * ezp_is_datalog() compares case-insensitively                  */
    CHECK(!strcmp(out, "A:\\datalog"), "drive uppercase + backslashes");
    CHECK(ezp_is_datalog(out), "ci datalog match");
    CHECK(ezp_canon_dir("A:\\..\\x", out, sizeof(out)) != EZP_OK,
          "traversal rejected");
    CHECK(ezp_canon_dir("B:\\DATALOG", out, sizeof(out)) != EZP_OK,
          "other drive rejected");
    CHECK(ezp_canon_dir("A:\\a%2fb", out, sizeof(out)) != EZP_OK,
          "encoded input rejected");
    CHECK(ezp_validate_name("20240101_223107_PLD.edf") == EZP_OK,
          "file name ok");
    CHECK(ezp_validate_name("../x") != EZP_OK, "name traversal rejected");
    CHECK(ezp_is_datalog("A:\\DATALOG\\20240101"), "datalog recognized");
    CHECK(ezp_is_datalog("A:\\datalog"), "datalog case-insensitive");
    CHECK(!ezp_is_datalog("A:\\OTHER"), "other dir not datalog");
    char day[CAT_DAY_STR];
    CHECK(ezp_datalog_day("A:\\DATALOG\\20240101", day) &&
          !strcmp(day, "20240101"), "day extracted");
    CHECK(!ezp_datalog_day("A:\\DATALOG", day), "plain datalog has no day");
}

/* ---------------- catalog ---------------- */

static ezs_entry_t mk(const char *name, uint64_t size, const char *ts,
                      bool is_dir)
{
    ezs_entry_t e;
    memset(&e, 0, sizeof e);
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.size = size;
    e.is_dir = is_dir;
    if (ts) {
        e.has_timestamp = true;
        strncpy(e.timestamp_raw, ts, sizeof(e.timestamp_raw) - 1);
    }
    return e;
}

static void test_catalog(void)
{
    cat_init();
    ezs_entry_t p[3] = {
        mk("20240101", 0, "2024-01-01 12:00:00", true),
        mk("20240102", 0, "2024-01-02 12:00:00", true),
        mk("20240103", 0, "2024-01-03 12:00:00", true),
    };
    CHECK(cat_feed_parent(p, 3, 1000) == ESP_OK, "parent feed");
    CHECK(cat_folder_count() == 3, "three folders");

    cat_set_hot_base("20240103", "time");
    CHECK(!cat_catalog_complete(), "incomplete before day scans");

    ezs_entry_t f[2] = {
        mk("20240103_220000_BRP.edf", 4096, "2024-01-03 22:01:00", false),
        mk("20240103_220000_PLD.edf", 8192, "2024-01-03 22:01:00", false),
    };
    CHECK(cat_feed_day("20240103", f, 2, 2000, NULL) == ESP_OK, "day feed");
    CHECK(cat_feed_day("20240102", f, 1, 3000, NULL) == ESP_OK, "older feed");
    CHECK(cat_feed_day("20240101", f, 1, 4000, NULL) == ESP_OK, "oldest feed");
    CHECK(cat_catalog_complete(), "complete after all scanned");

    cat_file_t got;
    CHECK(cat_find_file("20240103", "20240103_220000_PLD.edf", &got) &&
          got.size == 8192, "file lookup");

    /* change detection: identical re-feed -> false; size bump -> true.
     * Events carry old/new (size,stamp) tuples per file (U3).        */
    cat_diff_t d;
    cat_feed_day("20240103", f, 2, 5000, &d);
    CHECK(!d.changed && !d.n_events, "unchanged listing -> no change");
    CHECK(d.prev_scan_us == 2000, "prev scan bound reported");

    f[1].size = 9000;
    cat_feed_day("20240103", f, 2, 6000, &d);
    CHECK(d.changed && d.n_events == 1, "grown file -> one event");
    CHECK(d.n_events && d.ev[0].kind == CAT_EV_GREW &&
          d.ev[0].old_size == 8192 && d.ev[0].new_size == 9000 &&
          !strcmp(d.ev[0].name, "20240103_220000_PLD.edf"),
          "grew event carries both sizes");

    f[1].size = 8192;
    cat_feed_day("20240103", f, 2, 7000, &d);
    CHECK(d.changed && d.ev[0].kind == CAT_EV_SHRUNK,
          "shrunk file -> change (rewrite-suspect)");

    /* timestamp-only flush: same size, new stamp -> STAMP event (U3) */
    strcpy(f[1].timestamp_raw, "2024-01-03 22:02:02");
    cat_feed_day("20240103", f, 2, 8000, &d);
    CHECK(d.changed && d.n_events == 1 &&
          d.ev[0].kind == CAT_EV_STAMP &&
          !strcmp(d.ev[0].old_stamp, "2024-01-03 22:01:00") &&
          !strcmp(d.ev[0].new_stamp, "2024-01-03 22:02:02"),
          "stamp-only change -> STAMP event");

    /* membership: file removed -> GONE; re-added -> NEW               */
    cat_feed_day("20240103", f, 1, 9000, &d);
    CHECK(d.changed && d.n_events == 1 && d.ev[0].kind == CAT_EV_GONE,
          "removed file -> GONE");
    cat_feed_day("20240103", f, 2, 10000, &d);
    CHECK(d.n_events == 1 && d.ev[0].kind == CAT_EV_NEW &&
          d.ev[0].new_stamp_ok, "re-added file -> NEW");

    cat_file_t out[4];
    CHECK(cat_day_entries("20240103", out, 4) == 2, "day entries");

    char days[8][CAT_DAY_STR];
    int nd = cat_list_days(days, 8);
    CHECK(nd == 3 && !strcmp(days[0], "20240103"), "newest first");

    char hot[4][CAT_DAY_STR];
    int nh = cat_hot_days(hot, 4);
    printf("  hot: n=%d base=20240103 -> %s %s %s\n", nh,
           nh > 0 ? hot[0] : "-", nh > 1 ? hot[1] : "-",
           nh > 2 ? hot[2] : "-");
    CHECK(nh >= 2, "hot set nonempty");
}

/* ---------------- therapy FSM ---------------- */

static void test_therapy(void)
{
    host_now_us = 0;
    therapy_init();
    CHECK(therapy_status()->state == THERAPY_UNKNOWN, "boot unknown");
    CHECK(!therapy_status()->transfer_allowed, "no transfer at boot");

    therapy_on_growth(1000000);
    CHECK(therapy_status()->state == THERAPY_IN, "growth -> IN");
    CHECK(!therapy_status()->transfer_allowed, "IN blocks transfers");

    therapy_on_quiet_round(60000000);
    CHECK(therapy_status()->state == THERAPY_IN, "still IN at 59 s");

    therapy_on_quiet_round(130000000);
    CHECK(therapy_status()->state == THERAPY_IN, "first quiet round counts");
    therapy_on_quiet_round(131000000);
    CHECK(therapy_status()->state == THERAPY_OUT, "two rounds -> OUT");
    CHECK(therapy_status()->transfer_allowed, "OUT allows transfers");

    therapy_on_gap();
    CHECK(therapy_status()->state == THERAPY_UNKNOWN, "gap -> UNKNOWN");
    CHECK(!therapy_status()->transfer_allowed, "UNKNOWN blocks");
}

/* ---------------- BLE framing ---------------- */

static void test_framing(void)
{
    uint8_t frame[64];
    const char *msg = "hello";
    uint16_t n = blep_build_frame(frame, sizeof(frame), BLEP_JSON_REQUEST,
                                  0xA5, 0, (const uint8_t *)msg, 5,
                                  true, true);
    CHECK(n == sizeof(blep_hdr_t) + 5, "frame length");
    blep_hdr_t h;
    memcpy(&h, frame, sizeof(h));
    CHECK(h.version == 1 && h.kind == BLEP_JSON_REQUEST, "header fields");
    CHECK((h.flags & (BLEP_FLAG_START | BLEP_FLAG_END)) ==
          (BLEP_FLAG_START | BLEP_FLAG_END), "start|end flags");

    blep_reasm_t r;
    blep_reasm_init(&r);
    uint32_t rid = 0;
    uint8_t f1[64], f2[64];
    blep_build_frame(f1, sizeof(f1), BLEP_JSON_REQUEST, 7, 0,
                     (const uint8_t *)"{\"op\":\"GET_X", 12, true, false);
    blep_build_frame(f2, sizeof(f2), BLEP_JSON_REQUEST, 7, 12,
                     (const uint8_t *)"\"}", 2, false, true);
    CHECK(!blep_reasm_feed(&r, f1, sizeof(blep_hdr_t) + 12, 0, &rid),
          "first fragment not done");
    CHECK(blep_reasm_feed(&r, f2, sizeof(blep_hdr_t) + 2, 0, &rid),
          "second completes");
    CHECK(rid == 7 && r.len == 14, "reassembled length/id");
    CHECK(!memcmp(r.buf, "{\"op\":\"GET_X\"}", 14), "payload intact");

    blep_reasm_init(&r);
    blep_build_frame(f1, sizeof(f1), BLEP_JSON_REQUEST, 8, 0,
                     (const uint8_t *)"ab", 2, true, false);
    blep_reasm_feed(&r, f1, sizeof(blep_hdr_t) + 2, 0, &rid);
    blep_build_frame(f2, sizeof(f2), BLEP_JSON_REQUEST, 8, 5,
                     (const uint8_t *)"zz", 2, false, true);
    CHECK(!blep_reasm_feed(&r, f2, sizeof(blep_hdr_t) + 2, 0, &rid),
          "hole rejected");

    /* Multi-fragment stream at a real ATT budget: the emitted chunk
     * size must match the offset the caller advances by, or bytes go
     * missing between fragments (the whole payload is the budget the
     * builder takes — header included).                              */
    {
        uint8_t src[500];
        for (int i = 0; i < 500; i++) src[i] = (uint8_t)(i * 7 + 3);
        uint16_t budget = 244;                 /* ATT MTU 247 - 3     */
        uint16_t room = budget - sizeof(blep_hdr_t);
        uint8_t f[260];
        size_t off = 0;
        blep_reasm_init(&r);
        bool done = false;
        while (off < sizeof(src)) {
            size_t chunk = sizeof(src) - off;
            if (chunk > room) chunk = room;
            uint16_t fl = blep_build_frame(f, budget, BLEP_JSON_REQUEST, 9,
                                           off, src + off, (uint16_t)chunk,
                                           off == 0,
                                           off + chunk >= sizeof(src));
            CHECK(fl == sizeof(blep_hdr_t) + chunk, "chunk fits budget");
            done = blep_reasm_feed(&r, f, fl, 0, &rid);
            off += chunk;
        }
        CHECK(done, "stream completes");
        CHECK(r.len == sizeof(src) && !memcmp(r.buf, src, sizeof(src)),
              "multi-fragment payload byte-exact");
    }
}

/* ---------------- drift anchors + stamp estimator ---------------- */

/* dev_s (unix seconds) -> "YYYY-MM-DD HH:MM:SS" for fixture rows. */
static void fmt_stamp(int64_t dev_s, char *out)
{
    long days = dev_s / 86400;
    long rem  = dev_s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    long z   = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int  y   = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    int  d   = (int)(doy - (153 * mp + 2) / 5 + 1);
    int  m   = (int)mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    if (y < 0) y = 0;
    if (y > 9999) y = 9999;            /* bound %04d for -Wformat-trunc */
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        m = 1; d = 1;                  /* unreachable for real inputs   */
    }
    snprintf(out, 24, "%04d-%02d-%02d %02ld:%02ld:%02ld",
             y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

static void test_drift(void)
{
    drift_reset();
    int64_t lo, hi;
    CHECK(!drift_map_utc(0, &lo, &hi), "no anchors -> unmapped");

    host_now_us = 10000;
    CHECK(drift_time_begin(1, 1000000, 10000, 42, 100) == ESP_OK,
          "exchange begin");
    drift_time_reply_tsp(1, 120);
    uint32_t unc = 0;
    anchor_result_t ar = drift_time_commit(1, 1050000, host_now_us, &unc);
    CHECK(ar == ANCHOR_EPOCH || ar == ANCHOR_OK, "first anchor accepted");
    CHECK(drift_anchor_count() == 1, "anchor stored");

    drift_time_begin(2, 2000000, 10000, 42, 200);
    drift_time_reply_tsp(2, 220);
    ar = drift_time_commit(2, 2400000, host_now_us, &unc); /* 400ms rtt */
    CHECK(ar == ANCHOR_RTT, "big rtt rejected");

    drift_time_begin(3, 3000000, 10000, 43, 300);
    drift_time_reply_tsp(3, 320);
    ar = drift_time_commit(3, 3050000, host_now_us, &unc);
    CHECK(ar == ANCHOR_EPOCH, "epoch change -> new segment");

    /* §10.4 caches a pending exchange for ≤5 s: reusing its id after
     * that must start a fresh exchange, not fail INVALID_STATE.      */
    CHECK(drift_time_begin(4, 4000000, 10000, 43, 1000) == ESP_OK,
          "begin 4");
    CHECK(drift_time_begin(4, 4000000, 10000, 43, 1500) ==
          ESP_ERR_INVALID_STATE, "same id inside window rejected");
    CHECK(drift_time_begin(4, 4000000, 10000, 43,
                           1000 + 6LL * 1000 * 1000) == ESP_OK,
          "expired pending entry does not block id reuse");

    /* ---- stamp parsing (U4): card rows are space-padded ---- */
    int64_t ms, ms2;
    CHECK(drift_stamp_parse("2026- 9-30   14: 9:52", &ms), "stamp parse");
    CHECK(drift_stamp_parse("2026-09-30 14:09:54", &ms2) &&
          ms2 - ms == 2, "stamp delta consistent");
    CHECK(!drift_stamp_parse("garbage", &ms), "bad stamp rejected");
    CHECK(!drift_stamp_parse("2026-13-30 14:09:54", &ms),
          "month out of range");
    CHECK(!drift_stamp_parse("2026-09-30 14:09:54x", &ms),
          "trailing junk rejected");

    /* ---- beacon classification (U3): sampled vs annotation ---- */
    CHECK(drift_beacon_class("20240103_220000_BRP.edf") == DRIFT_BC_BRP,
          "BRP beacon");
    CHECK(drift_beacon_class("20240103_220000_PLD.edf") == DRIFT_BC_PLD,
          "PLD beacon");
    CHECK(drift_beacon_class("20240103_220000_SA2.edf") == DRIFT_BC_SA2,
          "SA2 beacon");
    CHECK(drift_beacon_class("20240103_220000_CSL.edf") == DRIFT_BC_ANNOT,
          "CSL is annotation, not a beacon");
    CHECK(drift_beacon_class("20240103_220000_EVE.edf") == DRIFT_BC_ANNOT,
          "EVE is annotation");
    CHECK(drift_beacon_class("STR.edf") == DRIFT_BC_NONE, "none");

    /* ---- interval-intersection estimator (U4) ----
     * Anchors map mono -> utc = mono + U. The device clock is Ddev
     * seconds ahead, so a write at true instant w carries stamp
     * floor((U + w)/2s)*2 + Ddev. Scan bounds come from the catalog
     * diff (prev unchanged scan -> upper bound via changed response). */
    const int64_t U    = 1700000000LL * 1000000LL;   /* anchor utc base */
    const int64_t Ddev = 100;                        /* dev +100 s      */
    drift_test_anchor(0, U, 2000, 7);
    drift_test_anchor(900LL * 1000 * 1000,
                      U + 900LL * 1000 * 1000, 2000, 7);

    char ts[32];
    fmt_stamp((U / 1000000) + 200 + Ddev, ts);   /* stamp M0 visible at
                                                    * the baseline    */
    ezs_entry_t fs[1];
    fs[0] = mk("20240103_220000_BRP.edf", 4096, ts, false);

    drift_session_t s;
    drift_sess_begin(&s, 60000000, 300000);      /* C=60 s, J=0.3 s    */
    memcpy(s.day, "20240103", sizeof(s.day));

    cat_init();
    cat_diff_t df;

    /* baseline feed — folder not yet resident -> baseline, no events */
    cat_feed_day("20240103", fs, 1, 200000, &df);
    CHECK(df.changed && df.n_events == 0, "first feed is a baseline");
    drift_feed_scan(&s, "20240103", fs, 1, &df, 200000, 210000);
    CHECK(s.n_ev == 0 && s.beacon[0], "baseline picks beacon, no ev");

    /* unchanged scan sets the previous-observation bound             */
    cat_feed_day("20240103", fs, 1, 250000, &df);
    CHECK(!df.changed, "quiet listing");
    drift_feed_scan(&s, "20240103", fs, 1, &df, 250000000, 250300000);
    CHECK(s.n_ev == 0, "quiet -> no events");

    /* flush observed: stamp +60 s (write inside (250.3 s, 320.8 s])  */
    int64_t w = U + 300LL * 1000 * 1000 + Ddev * 1000000LL;
    fmt_stamp(((w / 1000000) / 2) * 2, ts);      /* FAT 2 s floor      */
    strcpy(fs[0].timestamp_raw, ts);
    fs[0].size = 6144;
    cat_feed_day("20240103", fs, 1, 320000, &df);
    CHECK(df.n_events == 1 && (df.ev[0].kind == CAT_EV_STAMP ||
          df.ev[0].kind == CAT_EV_GREW), "change produces an event");
    int added = drift_feed_scan(&s, "20240103", fs, 1, &df,
                                320300000LL, 320500000LL);
    CHECK(added == 1, "transition event appended");
    drift_sess_eval(&s, 321000000);
    CHECK(s.est_bound_valid, "one window gives a bound");
    CHECK(s.tier == ACC_APPROXIMATE, "single window -> approximate");
    /* offset must contain -Ddev (true-device, device is ahead)       */
    CHECK(s.off_lo_us / 1000 <= -Ddev * 1000 &&
          s.off_hi_us / 1000 >= -Ddev * 1000, "bound contains offset");
    /* U4.3: the stamp is [M, M+2) — lo subtracts M+2, hi subtracts
     * M. Asymmetry is the quantization term; verify both edges.
     * Expected edges computed from the live profile + session args. */
    {
        const drift_profile_t *p = drift_profile();
        int64_t M = ((w / 1000000) / 2) * 2 * 1000000;   /* stamp µs   */
        int64_t wlo = 250000000 - (int64_t)s.cache_bound_us
                    - (int64_t)p->latency_hi_us - (int64_t)s.jitter_us;
        int64_t whi = 320500000 - (int64_t)p->latency_lo_us
                    + (int64_t)s.jitter_us;
        int64_t elo = (U + wlo) - (M + 2000000);        /* win_lo−M+2 */
        int64_t ehi = (U + whi) - M;                    /* win_hi−M   */
        CHECK(s.off_lo_us <= elo + 50000 &&
              s.off_lo_us >= elo - 50000,
              "lo edge carries the +2 s stamp quant");
        CHECK(s.off_hi_us >= ehi - 50000 &&
              s.off_hi_us <= ehi + 50000,
              "hi edge uses M (not M+2)");
    }

    /* second independent window keeps narrowing the bound            */
    cat_feed_day("20240103", fs, 1, 380000, &df);
    drift_feed_scan(&s, "20240103", fs, 1, &df, 380000000, 380300000);
    w = U + 360LL * 1000 * 1000 + Ddev * 1000000LL;
    fmt_stamp(((w / 1000000) / 2) * 2, ts);
    strcpy(fs[0].timestamp_raw, ts);
    fs[0].size = 8192;
    cat_feed_day("20240103", fs, 1, 440000, &df);
    added = drift_feed_scan(&s, "20240103", fs, 1, &df,
                            440300000LL, 440500000LL);
    CHECK(added == 1 && s.n_ev == 2, "second transition");
    drift_sess_eval(&s, 441000000);
    CHECK(s.n_windows == 2, "two independent windows");
    CHECK(s.est_bound_valid &&
          s.off_lo_us / 1000 <= -Ddev * 1000 &&
          s.off_hi_us / 1000 >= -Ddev * 1000,
          "intersection still contains the true offset");
    int64_t half1 = s.off_hi_us - s.off_lo_us;
    CHECK(half1 > 0, "nonempty interval");

    /* backwards stamp -> segment split, never interpolated through   */
    fmt_stamp((U / 1000000) + 10, ts);           /* wildly old stamp   */
    strcpy(fs[0].timestamp_raw, ts);
    cat_feed_day("20240103", fs, 1, 500000, &df);
    added = drift_feed_scan(&s, "20240103", fs, 1, &df,
                            500300000LL, 500500000LL);
    drift_sess_eval(&s, 501000000);
    CHECK(s.flag_bits & DF_BACKWARD, "backwards stamp flagged");
    CHECK(s.n_splits >= 1 && s.n_segments >= 2, "segment split");

    /* empty intersection -> contradiction -> new segment. Cadence-
     * plausible stamps (small ddev vs dmono) that nevertheless cannot
     * coexist inside one offset bound — needs tight windows, so run
     * it under a characterized profile.                              */
    {
        drift_profile_t saved = *drift_profile();
        drift_profile_t tight = saved;
        tight.latency_lo_us = 0;
        tight.latency_hi_us = 500000;      /* 0.5 s                  */
        tight.stamp_channel_validated = true;
        drift_profile_set(&tight);

        drift_sess_begin(&s, 500000, 100000);   /* C=0.5 s, J=0.1 s  */
        memcpy(s.day, "20240103", sizeof(s.day));

        cat_init();
        fmt_stamp(U / 1000000 + 300, ts);
        strcpy(fs[0].timestamp_raw, ts);
        cat_feed_day("20240103", fs, 1, 200000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 240000000, 240100000);

        /* quiet scan bound just ahead of event A                     */
        cat_feed_day("20240103", fs, 1, 250000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 300000000, 300050000);

        /* event A: stamp U_s+400 -> offset ~-100 s, tight bound      */
        fmt_stamp(U / 1000000 + 400, ts);
        strcpy(fs[0].timestamp_raw, ts);
        cat_feed_day("20240103", fs, 1, 320000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 301000000, 301050000);

        cat_feed_day("20240103", fs, 1, 380000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 360000000, 360050000);

        /* event B: stamp +55 s over ~60 s mono — plausible cadence
         * but its offset interval sits ~5 s above A's bound          */
        fmt_stamp(U / 1000000 + 455, ts);
        strcpy(fs[0].timestamp_raw, ts);
        cat_feed_day("20240103", fs, 1, 440000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 361000000, 361050000);

        drift_sess_eval(&s, 362000000);
        CHECK(s.flag_bits & DF_CONTRADICTION, "empty intersection flagged");
        CHECK(s.n_splits >= 1, "contradiction split the segment");
        CHECK(!(s.flag_bits & DF_STEP), "cadence stayed plausible");

        drift_profile_set(&saved);
    }

    /* beacon pick tracks the active session's file: two same-class
     * files in a day folder (stop/restart) -> the latest wins        */
    {
        ezs_entry_t two[2];
        char t1[32], t2[32];
        fmt_stamp(U / 1000000 + 100, t1);
        fmt_stamp(U / 1000000 + 200, t2);
        two[0] = mk("20240103_220000_BRP.edf", 4096, t1, false);
        two[1] = mk("20240103_230000_BRP.edf", 4096, t2, false);
        drift_session_t s2;
        drift_sess_begin(&s2, 60000000, 300000);
        memcpy(s2.day, "20240103", sizeof(s2.day));
        cat_init();
        cat_diff_t d2;
        cat_feed_day("20240103", two, 2, 200000, &d2);
        CHECK(drift_feed_scan(&s2, "20240103", two, 2, &d2,
                              200000, 210000) == 0, "baseline quiet");
        CHECK(!strcmp(s2.beacon, "20240103_230000_BRP.edf"),
              "picks the latest same-class file");
    }
}

/* ---------------- tiers, dense windows, SOT (U5/U6/U9) --------------
 * A *validated* profile shrinks the windows to the certified tier;
 * two independent windows inside the ±2 s gate -> ACCURATE -> the SOT
 * promotes and persists; NVS round-trip + corruption covered.       */
static void test_sot(void)
{
    drift_profile_t base = *drift_profile();   /* restore at the end   */
    drift_profile_t prof = base;
    prof.stamp_channel_validated = true;
    prof.cache_bound_us    = 500000;           /* 0.5 s                */
    prof.latency_lo_us     = 0;
    prof.latency_hi_us     = 500000;
    prof.jitter_us         = 100000;
    prof.accurate_half_us  = 2000000;
    prof.dev_rate_ppm      = 50;
    prof.dense_enabled     = true;
    prof.dense_spacing_us  = 500000;
    drift_profile_set(&prof);

    drift_reset();
    nvs_stub_reset();
    cat_init();
    drift_sot_invalidate();

    const int64_t U = 1700000000LL * 1000000LL;
    const int64_t D = 100;                     /* device +100 s ahead  */
    drift_test_anchor(0, U, 2000, 9);
    drift_test_anchor(900LL * 1000 * 1000,
                      U + 900LL * 1000 * 1000, 2000, 9);

    char ts[32];
    fmt_stamp(U / 1000000 + 200 + D, ts);
    ezs_entry_t fs[1] = { mk("20240103_220000_BRP.edf", 4096, ts, false) };
    cat_diff_t df;
    drift_session_t *ls;
    int64_t w;

    /* baseline: resident listing, no session yet                     */
    cat_feed_day("20240103", fs, 1, 200000000, &df);
    CHECK(df.n_events == 0, "baseline feed quiet");

    /* unchanged scan at 219.74 s — then a dense-window flush         */
    cat_feed_day("20240103", fs, 1, 219740000, &df);
    /* mid-session join: the first changed listing opens the session —
     * sched gates on diff.n_events; reproduce that here             */
    w = U + 220LL * 1000 * 1000 + D * 1000000LL;
    fmt_stamp((w / 1000000 / 2) * 2, ts);
    strcpy(fs[0].timestamp_raw, ts);
    fs[0].size = 6144;
    cat_feed_day("20240103", fs, 1, 220240000, &df);
    CHECK(df.n_events == 1, "flush observed as catalog event");
    drift_live_begin("20240103");
    ls = drift_live();
    CHECK(ls != NULL, "live session open");
    /* first feed carries the event; the prev bound comes from the
     * catalog's prev_scan_us (219.74 s baseline scan) — U4.          */
    CHECK(drift_feed_scan(ls, "20240103", fs, 1, &df,
                          220240000, 220480000) == 1, "joined mid-run");
    drift_sess_eval(ls, 220500000);
    CHECK(ls->est_bound_valid, "bound from mid-session join");
    CHECK(ls->tier != ACC_ACCURATE, "one window can't be accurate");
    CHECK(ls->off_lo_us <= -D * 1000000 && ls->off_hi_us >= -D * 1000000,
          "window contains the true -100 s offset");

    /* second flush, same spacing -> two independent windows          */
    cat_feed_day("20240103", fs, 1, 279740000, &df);
    drift_feed_scan(ls, "20240103", fs, 1, &df, 279740000, 279980000);
    w = U + 280LL * 1000 * 1000 + D * 1000000LL;
    fmt_stamp((w / 1000000 / 2) * 2, ts);
    strcpy(fs[0].timestamp_raw, ts);
    fs[0].size = 8192;
    cat_feed_day("20240103", fs, 1, 280240000, &df);
    drift_feed_scan(ls, "20240103", fs, 1, &df, 280240000, 280480000);
    drift_sess_eval(ls, 280500000);
    CHECK(ls->n_windows == 2, "two independent windows");
    CHECK(ls->tier == ACC_ACCURATE, "accurate tier met");
    CHECK(ls->off_hi_us - ls->off_lo_us <= 4000000,
          "half-width <= 2 s gate");
    CHECK(ls->off_lo_us <= -D * 1000000 &&
          ls->off_hi_us >= -D * 1000000, "SOT interval correct");

    /* SOT promoted + persisted                                        */
    drift_sot_t sot;
    CHECK(drift_sot_view(&sot) && sot.valid, "SOT promoted");
    CHECK(sot.origin == DRIFT_SOT_SESSION, "SOT from live session");
    CHECK(sot.accuracy_half_ms <= 2000, "SOT inside gate");

    /* supersede ordering: a later accurate fit supersedes; a fit with
     * an older computed_at never displaces the newer SOT (U6)        */
    int64_t comp1 = sot.computed_at_ms;
    drift_sess_eval(ls, 282000000);
    CHECK(drift_sot_view(&sot) && sot.computed_at_ms > comp1,
          "newer accurate supersedes");
    comp1 = sot.computed_at_ms;
    drift_sess_eval(ls, 281500000);          /* older fit instant     */
    CHECK(drift_sot_view(&sot) && sot.computed_at_ms == comp1,
          "older eval never displaces SOT");

    CHECK(drift_sot_dirty(), "SOT dirty before commit");
    CHECK(drift_sot_commit(true, 281000000) == ESP_OK, "SOT persisted");
    CHECK(!drift_sot_dirty(), "clean after commit");

    /* NVS round-trip: reload gives the same value, origin = NVS       */
    CHECK(drift_sot_load() == ESP_OK, "SOT reloads");
    CHECK(drift_sot_view(&sot) && sot.origin == DRIFT_SOT_NVS &&
          sot.clock_drift_ms <= -99000 && sot.clock_drift_ms >= -104000,
          "reloaded SOT matches");

    /* corrupt blob -> rejected, never a fabricated value              */
    nvs_stub_corrupt("drift_sot");
    drift_sot_invalidate();
    CHECK(drift_sot_load() != ESP_OK, "corrupt SOT rejected");
    CHECK(!drift_sot_view(&sot), "no SOT after corrupt+invalidate");

    /* freeze outside therapy: a closed session's eval cannot promote —
     * (the live-end eval itself may promote; the frozen SOT survives) */
    drift_live_end(290000000);
    CHECK(drift_live() == NULL, "session closed");
    drift_sot_invalidate();
    drift_sess_eval(drift_last(), 300000000);
    CHECK(!drift_sot_view(&sot), "closed session cannot promote SOT");

    /* GET_DRIFT envelope pieces: the closed session's record stays
     * published (observation_closed), carries the accurate tier and
     * bound; the SOT JSON carries origin + persisted_seq             */
    drift_record_t pub;
    CHECK(drift_pub_get(&pub) && pub.observation_closed,
          "closed record stays published");
    char jb[3072];
    CHECK(drift_estimate_json(&pub, jb, sizeof(jb)) > 0 &&
          strstr(jb, "\"accuracy_tier\":\"accurate\"") &&
          strstr(jb, "\"estimate_state\":\"validated\"") &&
          strstr(jb, "\"clock_drift_ms\":-") &&
          strstr(jb, "\"files\":[{"),
          "estimate JSON carries the accurate bound");
    /* SOT view is empty post-invalidate — simulate the stored value  */
    drift_sot_t sim = { .valid = true, .clock_drift_ms = -100000,
                        .lo_ms = -101900, .hi_ms = -98100,
                        .accuracy_half_ms = 1900,
                        .computed_at_ms = U / 1000 + 280000,
                        .reference_at_ms = U / 1000 + 280200,
                        .persisted_seq = 7, .origin = DRIFT_SOT_NVS };
    CHECK(drift_sot_json(&sim, jb, sizeof(jb)) > 0 &&
          strstr(jb, "\"origin\":\"nvs\"") &&
          strstr(jb, "\"persisted_seq\":7"),
          "SOT JSON carries origin+seq");
    /* empty combo: no session ever -> pub invalid -> estimate null   */
    drift_reset();
    CHECK(!drift_pub_get(NULL), "no session -> estimate null");

    drift_profile_set(&base);        /* restore the build profile     */
}

/* ---------------- EDF fixed header (§8.2) ---------------- */

static void test_edf(void)
{
    /* synthetic 2-signal EDF header — no patient data, spec fixtures  */
    static uint8_t h[256 * 3];
    memset(h, ' ', sizeof(h));
    memcpy(h, "0       ", 8);
    memcpy(h + 168, "01.02.24", 8);         /* 2024-02-01             */
    memcpy(h + 176, "23.45.56", 8);         /* 23:45:56 local         */
    memcpy(h + 184, "     768", 8);         /* 256*(1+2)              */
    memcpy(h + 236, "      -1", 8);         /* continuous             */
    memcpy(h + 244, "      60", 8);         /* 60 s records           */
    memcpy(h + 252, "   2", 4);
    /* samples-per-record array at 256 + 216*ns = 256+432 = 688       */
    memcpy(h + 688, "    3600", 8);
    memcpy(h + 696, "      60", 8);

    edf_hdr_t e;
    CHECK(edf_hdr_parse(h, sizeof(h), &e), "edf parse");
    CHECK(e.header_bytes == 768, "H=768");
    CHECK(e.record_bytes == (3600 + 60) * 2, "R=7320");
    CHECK(e.record_duration_s == 60, "D=60");
    CHECK(e.n_signals == 2, "ns=2");
    CHECK(e.n_records == -1, "continuous records");
    /* 2024-02-01 23:45:56 as days since 2000-01-01:
     * 24y*365 + 6 leaps + 31 (Jan) = 8797                            */
    CHECK(e.start_local_s == 8797 * 86400 + 23 * 3600 + 45 * 60 + 56,
          "start local seconds");

    edf_hdr_t bad;
    CHECK(!edf_hdr_parse(h, 200, &bad), "truncated rejected");
    h[252] = '9';                            /* ns=9 -> H mismatch    */
    CHECK(!edf_hdr_parse(h, sizeof(h), &bad), "H/ns mismatch rejected");
    h[252] = ' ';
    memcpy(h + 244, "     1.5", 8);          /* fractional duration   */
    CHECK(!edf_hdr_parse(h, sizeof(h), &bad), "non-integer D rejected");
}

/* ---------------- evidence journal (§9/§14.1) ---------------- */

static int s_walk_n;
static bool ev_count(const char *p, uint16_t len, uint32_t seq, void *c)
{
    (void)p; (void)c;
    if (seq == (uint32_t)s_walk_n + 1) s_walk_n = seq;
    (void)len;
    return true;
}

static void test_journal(void)
{
    nvs_stub_reset();
    host_now_us = 1000;
    CHECK(jrn_init() == ESP_OK, "journal init");
    CHECK(jrn_health() == JRN_OK, "journal healthy");

    for (int i = 0; i < 5; i++) {
        host_now_us += 1000;
        CHECK(jrn_put("poll", "\"i\":1") == ESP_OK, "put");
    }
    CHECK(jrn_commit() == ESP_OK, "commit");
    CHECK(jrn_seq() == 2, "seq advanced (one chunk)");

    /* replay walks slots in seq order                                */
    uint32_t gaps = 0;
    s_walk_n = 0;
    CHECK(jrn_walk(ev_count, NULL, &gaps) == ESP_OK, "walk ok");
    CHECK(s_walk_n == 1 && gaps == 0, "one contiguous record");

    /* corrupt the slot → CRC gap reported, not silently skipped      */
    nvs_stub_corrupt("b000");
    s_walk_n = 0; gaps = 0;
    CHECK(jrn_walk(ev_count, NULL, &gaps) == ESP_OK, "walk damaged");
    CHECK(gaps == 1, "corrupt slot counted as gap");

    /* write failure -> truncated flag                                */
    nvs_stub_fail_writes(1);
    CHECK(jrn_put("poll", "\"i\":2") == ESP_OK, "queue record");
    esp_err_t e = jrn_commit();
    CHECK(e != ESP_OK, "failed commit surfaces");
    CHECK(jrn_truncated(), "truncated flag set");
}

int main(void)
{
    test_parser();
    test_paths();
    test_catalog();
    test_therapy();
    test_framing();
    test_drift();
    test_sot();
    test_edf();
    test_journal();
    if (fails) {
        printf("\n%d FAILURES\n", fails);
        return 1;
    }
    printf("\nALL HOST TESTS PASSED\n");
    return 0;
}
