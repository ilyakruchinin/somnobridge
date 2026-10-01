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
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"

#include "ezs_parse.h"
#include "ezs_path.h"
#include "catalog.h"
#include "sched.h"
#include "phase.h"
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

/* ---------------- minimal JSON validator ----------------
 * Structural check only: values, escapes, commas, nesting. Enough to
 * catch emitter bugs (misplaced keys, unquoted names, stray commas)
 * that substring greps cannot.                                   */
typedef struct { const char *p; int depth; } jv_t;

static void jv_ws(jv_t *j)
{
    while (*j->p == ' ' || *j->p == '\t' ||
           *j->p == '\n' || *j->p == '\r') j->p++;
}

static int jv_str(jv_t *j)
{
    if (*j->p++ != '"') return 0;
    for (;;) {
        if (!*j->p || *(unsigned char *)j->p < 0x20) return 0;
        if (*j->p == '"') { j->p++; return 1; }
        if (*j->p == '\\') {
            j->p++;
            if (!strchr("\"\\/bfnrtu", *j->p)) return 0;
            if (*j->p == 'u') {
                for (int i = 0; i < 4; i++)
                    if (!isxdigit((unsigned char)j->p[i])) return 0;
                j->p += 4;
            }
            j->p++;
            continue;
        }
        j->p++;
    }
}

static int jv_num(jv_t *j)
{
    const char *s = j->p;
    if (*j->p == '-') j->p++;
    if (*j->p == '0') j->p++;
    else if (!isdigit((unsigned char)*j->p)) return 0;
    else while (isdigit((unsigned char)*j->p)) j->p++;
    if (*j->p == '.') {
        j->p++;
        if (!isdigit((unsigned char)*j->p)) return 0;
        while (isdigit((unsigned char)*j->p)) j->p++;
    }
    if (*j->p == 'e' || *j->p == 'E') {
        j->p++;
        if (*j->p == '+' || *j->p == '-') j->p++;
        if (!isdigit((unsigned char)*j->p)) return 0;
        while (isdigit((unsigned char)*j->p)) j->p++;
    }
    return j->p > s;
}

static int jv_val(jv_t *j)
{
    jv_ws(j);
    if (++j->depth > 64) return 0;
    int ok = 0;
    if (*j->p == '{') {
        j->p++;
        jv_ws(j);
        if (*j->p == '}') { j->p++; ok = 1; goto out; }
        for (;;) {
            jv_ws(j);
            if (!jv_str(j)) goto out;
            jv_ws(j);
            if (*j->p++ != ':') goto out;
            if (!jv_val(j)) goto out;
            jv_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; ok = 1; }
            goto out;
        }
    }
    if (*j->p == '[') {
        j->p++;
        jv_ws(j);
        if (*j->p == ']') { j->p++; ok = 1; goto out; }
        for (;;) {
            if (!jv_val(j)) goto out;
            jv_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; ok = 1; }
            goto out;
        }
    }
    if (*j->p == '"') ok = jv_str(j);
    else if (!strncmp(j->p, "true", 4)) { j->p += 4; ok = 1; }
    else if (!strncmp(j->p, "false", 5)) { j->p += 5; ok = 1; }
    else if (!strncmp(j->p, "null", 4)) { j->p += 4; ok = 1; }
    else ok = jv_num(j);
out:
    j->depth--;
    return ok;
}

static int json_valid(const char *s)
{
    jv_t j = { s, 0 };
    if (!jv_val(&j)) return 0;
    jv_ws(&j);
    return *j.p == '\0';
}

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

    /* live-session feed on the singleton: two 60 s stamp deltas fit
     * the published cadence (update2 keeps the estimator stamp-first;
     * the polling schedule lives in the phase tracker)              */
    {
        drift_reset();
        drift_test_anchor(0, U, 2000, 7);
        drift_test_anchor(900LL * 1000 * 1000,
                          U + 900LL * 1000 * 1000, 2000, 7);
        drift_live_begin("20240103");
        drift_session_t *ls = drift_live();
        cat_init();
        cat_diff_t d3;
        char ts3[32];
        fmt_stamp(U / 1000000 + 300, ts3);
        ezs_entry_t f3[1] = { mk("20240103_220000_BRP.edf", 4096, ts3,
                                 false) };
        cat_feed_day("20240103", f3, 1, 200000, &d3);
        drift_feed_scan(ls, "20240103", f3, 1, &d3, 200000000, 200100000);
        cat_feed_day("20240103", f3, 1, 250000, &d3);
        drift_feed_scan(ls, "20240103", f3, 1, &d3, 260000000, 260100000);
        fmt_stamp(U / 1000000 + 360, ts3);       /* flush 1           */
        strcpy(f3[0].timestamp_raw, ts3);
        f3[0].size = 6144;
        cat_feed_day("20240103", f3, 1, 320000, &d3);
        drift_feed_scan(ls, "20240103", f3, 1, &d3, 320000000, 320100000);
        cat_feed_day("20240103", f3, 1, 380000, &d3);
        drift_feed_scan(ls, "20240103", f3, 1, &d3, 380000000, 380100000);
        fmt_stamp(U / 1000000 + 420, ts3);       /* flush 2           */
        strcpy(f3[0].timestamp_raw, ts3);
        f3[0].size = 8192;
        cat_feed_day("20240103", f3, 1, 440000, &d3);
        drift_feed_scan(ls, "20240103", f3, 1, &d3, 440000000, 440100000);
        drift_sess_eval(ls, 441000000);
        CHECK(ls->n_ev == 2 && ls->n_deltas == 2 &&
              ls->flush_period_ms == 60000,
              "two events, 60 s cadence fitted");
        drift_live_end(450000000);
        /* resume semantics (update2 §3.5): a closed same-day session
         * reactivates with its evidence; a different day must not    */
        CHECK(drift_last_closed() && !drift_live(),
              "ended session is retained-closed");
        CHECK(!drift_live_resume("20240104"), "different day: no resume");
        CHECK(drift_live_resume("20240103"), "same day resumes");
        CHECK(drift_live() && drift_live()->n_ev == 2 &&
              drift_live()->n_deltas == 2,
              "evidence kept across the gap");
        drift_live_end(460000000);
        CHECK(drift_live_resume("20240103"), "re-closed: resumes again");
        CHECK(drift_live() && drift_live()->n_ev == 2,
              "evidence still intact");
        CHECK(!drift_live_resume("20240103"), "while live: no-op");
        drift_live_end(470000000);
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
    prof.phase_track_enabled    = true;
    prof.phase_refine_enabled   = false;
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

    /* unchanged scan at 219.74 s — then the next flush               */
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

    /* update3: recomputation alone does not displace persisted
     * history. The same slot changes only on a materially tighter or
     * non-overlapping bound, not merely because it is newer.          */
    int64_t comp1 = sot.computed_at_ms;
    drift_sess_eval(ls, 282000000);
    CHECK(drift_sot_view(&sot) && sot.computed_at_ms == comp1,
          "same-width re-eval does not supersede");
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
    nvs_stub_corrupt("drift_sot_q");
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
          json_valid(jb) &&
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
          json_valid(jb) &&
          strstr(jb, "\"origin\":\"nvs\"") &&
          strstr(jb, "\"persisted_seq\":7"),
          "SOT JSON carries origin+seq");
    /* empty combo: no session ever -> pub invalid -> estimate null   */
    drift_reset();
    CHECK(!drift_pub_get(NULL), "no session -> estimate null");

    drift_profile_set(&base);        /* restore the build profile     */
}

/* ---------------- update3: filename candidates + SOT tiers -------- */

static void feed_quiet(const char *day, ezs_entry_t *fs, int n,
                       cat_diff_t *df, int64_t req_us, int64_t resp_us,
                       drift_session_t *s)
{
    cat_feed_day(day, fs, n, req_us, df);
    drift_feed_scan(s, day, fs, n, df, req_us, resp_us);
}

static void test_update3(void)
{
    drift_profile_t base = *drift_profile();
    const int64_t F = 1704319200LL * 1000000LL;  /* 2024-01-03 22:00Z */
    char ts[32];
    cat_diff_t df;
    drift_sot_t sot;

    CHECK(drift_filename_local("20240103_220000_BRP.edf",
                               &(int64_t){0}), "filename parses");
    CHECK(!drift_filename_local("20240103_246000_BRP.edf",
                                &(int64_t){0}), "bad filename rejected");

    drift_reset();
    nvs_stub_reset();
    cat_init();
    drift_sot_invalidate();
    drift_profile_t p = base;
    p.cache_bound_us = 0;
    p.jitter_us = 0;
    p.latency_lo_us = 0;
    p.latency_hi_us = 0;
    p.dev_rate_ppm = 100;
    p.stamp_channel_validated = false;
    p.filename_encoding = DRIFT_ENC_TRUNCATED;
    p.filename_origin_bounded = true;
    p.fname_create_lo_us = 0;
    p.fname_create_hi_us = 500000;
    p.filename_periodic_bounded = true;
    p.fname_period_lo_us = 0;
    p.fname_period_hi_us = 500000;
    p.filename_create_validated = false;
    p.filename_periodic_validated = false;
    drift_profile_set(&p);
    drift_timezone_set("UTC0", true);
    drift_test_anchor(0, F, 1000, 21);
    drift_test_anchor(900LL * 1000 * 1000, F + 900LL * 1000 * 1000,
                      1000, 21);

    /* Creation candidate: one bounded file-appearance event supplies a
     * displayable filename-origin estimate, separate from the mtime
     * fallback.                                                        */
    {
        drift_session_t s;
        drift_sess_begin(&s, 0, 0);
        memcpy(s.day, "20240103", sizeof(s.day));
        cat_feed_day("20240103", NULL, 0, 10000000, &df);
        drift_feed_scan(&s, "20240103", NULL, 0, &df,
                        10000000, 10200000);
        fmt_stamp(F / 1000000, ts);
        ezs_entry_t fs[1] = {
            mk("20240103_220000_BRP.edf", 4096, ts, false)
        };
        cat_feed_day("20240103", fs, 1, 10500000, &df);
        CHECK(df.n_events == 1 && df.ev[0].kind == CAT_EV_NEW,
              "appearance is a creation event");
        CHECK(drift_feed_scan(&s, "20240103", fs, 1, &df,
                              10500000, 10700000) == 1,
              "creation evidence appended");
        drift_sess_eval(&s, 10800000);
        CHECK(s.n_create_ev == 1, "creation candidate observed");
        CHECK(s.preferred_source == DRIFT_SRC_FNAME_CREATE,
              "narrower creation source selected");
        drift_record_t rec;
        CHECK(drift_pub_get(&rec) && rec.candidates[1].available,
              "creation candidate published");
        CHECK(!rec.candidates[2].available &&
              strstr(rec.candidates[2].reason, "no_periodic_event"),
              "periodic candidate has explicit reason");
        char jb[4096];
        CHECK(drift_estimate_json(&rec, jb, sizeof(jb)) > 0 &&
              json_valid(jb) &&
              strstr(jb, "\"preferred_source\":\"filename_creation\"") &&
              strstr(jb, "filename_creation_interval_ms"),
              "candidate JSON emitted");
    }

    /* Periodic filename candidate: each transition must resolve to one
     * cycle by intersecting the filename/period model with its mtime
     * bin; an inconsistent stamp makes the candidate unavailable.      */
    {
        drift_session_t s;
        drift_sess_begin(&s, 0, 0);
        memcpy(s.day, "20240103", sizeof(s.day));
        fmt_stamp(F / 1000000, ts);
        ezs_entry_t fs[1] = {
            mk("20240103_220000_BRP.edf", 4096, ts, false)
        };
        cat_feed_day("20240103", fs, 1, 0, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df, 0, 200000);
        feed_quiet("20240103", fs, 1, &df, 59000000, 59200000, &s);
        fmt_stamp(F / 1000000 + 60, ts);
        strcpy(fs[0].timestamp_raw, ts);
        fs[0].size = 6144;
        cat_feed_day("20240103", fs, 1, 59800000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df,
                        59800000, 60000000);
        feed_quiet("20240103", fs, 1, &df, 119000000, 119200000, &s);
        fmt_stamp(F / 1000000 + 120, ts);
        strcpy(fs[0].timestamp_raw, ts);
        fs[0].size = 8192;
        cat_feed_day("20240103", fs, 1, 119800000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df,
                        119800000, 120000000);
        drift_sess_eval(&s, 120100000);
        CHECK(s.n_periodic_ev == 2 && s.periodic_cycle_valid,
              "periodic cycles resolved");
        CHECK(s.preferred_source == DRIFT_SRC_FNAME_PERIODIC,
              "periodic filename source selected");
        CHECK(s.off_lo_us <= 0 && s.off_hi_us >= 0,
              "periodic bound contains true offset");

        fmt_stamp(F / 1000000 + 125, ts);       /* no 60 s cycle match */
        strcpy(fs[0].timestamp_raw, ts);
        cat_feed_day("20240103", fs, 1, 125400000, &df);
        drift_feed_scan(&s, "20240103", fs, 1, &df,
                        125400000, 125600000);
        drift_sess_eval(&s, 125700000);
        CHECK(s.flag_bits & DF_CYCLE_AMBIG,
              "cycle ambiguity flagged");
        CHECK(!s.periodic_cycle_valid &&
              s.preferred_source == DRIFT_SRC_MTIME,
              "ambiguous periodic candidate cannot win");
    }

    /* Provisional SOT: three distinct mtime windows inside ±5 s persist
     * without touching the qualified slot. Failed writes stay dirty.   */
    {
        drift_profile_t q = p;
        q.filename_periodic_bounded = false;
        drift_profile_set(&q);
        drift_reset();
        nvs_stub_reset();
        cat_init();
        drift_sot_invalidate();
        drift_timezone_set("UTC0", true);
        drift_test_anchor(0, F, 1000, 22);
        drift_test_anchor(900LL * 1000 * 1000,
                          F + 900LL * 1000 * 1000, 1000, 22);
        drift_live_begin("20240103");
        drift_session_t *ls = drift_live();
        fmt_stamp(F / 1000000, ts);
        ezs_entry_t fs[1] = {
            mk("20240103_220000_BRP.edf", 4096, ts, false)
        };
        cat_feed_day("20240103", fs, 1, 0, &df);
        drift_feed_scan(ls, "20240103", fs, 1, &df, 0, 200000);
        for (int i = 1; i <= 3; i++) {
            int64_t rq = i * 60000000LL - 200000;
            feed_quiet("20240103", fs, 1, &df, rq, rq + 200000, ls);
            fmt_stamp(F / 1000000 + i * 60, ts);
            strcpy(fs[0].timestamp_raw, ts);
            fs[0].size = 4096 + i * 2048;
            cat_feed_day("20240103", fs, 1, rq + 800000, &df);
            drift_feed_scan(ls, "20240103", fs, 1, &df,
                            rq + 800000, rq + 1000000);
        }
        drift_sess_eval(ls, 181000000);
        CHECK(ls->n_windows >= 3 && ls->n_segments == 1,
              "three distinct provisional windows");
        CHECK(!drift_sot_view(&sot), "qualified SOT absent");
        CHECK(drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, &sot) &&
              sot.quality == DRIFT_Q_PROVISIONAL,
              "provisional SOT offered");
        CHECK(drift_sot_best(&sot) &&
              sot.quality == DRIFT_Q_PROVISIONAL,
              "best view returns provisional explicitly");

        int64_t alo, ahi;
        int64_t w0 = sot.hi_ms - sot.lo_ms;
        CHECK(drift_sot_apply(&sot, sot.ref_mono_us + 8LL * 3600 * 1000000,
                              &alo, &ahi), "SOT can be re-referenced");
        CHECK(ahi - alo >= w0 + 14000,
              "rate uncertainty expands persisted bound");

        nvs_stub_fail_writes(1);
        CHECK(drift_sot_commit(true, 182000000) != ESP_OK &&
              drift_sot_dirty(), "failed SOT write remains dirty");
        CHECK(drift_sot_commit(true, 183000000) == ESP_OK &&
              !drift_sot_dirty(), "SOT retry persists");
        CHECK(drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, &sot) &&
              sot.persisted_seq == 1, "persisted seq advances once");
        CHECK(drift_sot_load() == ESP_OK &&
              drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, &sot) &&
              sot.freshness_unknown,
              "restored record starts freshness-unknown");
        CHECK(!drift_sot_apply(&sot, 184000000, &alo, &ahi),
              "restored record not applicable before fresh anchor");

        /* Same session now qualifies: provisional remains separate.    */
        q.stamp_channel_validated = true;
        drift_profile_set(&q);
        drift_sess_eval(ls, 185000000);
        CHECK(drift_sot_view(&sot) &&
              sot.quality == DRIFT_Q_QUALIFIED,
              "qualified SOT is separate");
        CHECK(drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, &sot) &&
              sot.quality == DRIFT_Q_PROVISIONAL,
              "provisional slot retained");
        CHECK(drift_sot_commit(true, 186000000) == ESP_OK,
              "qualified record persists separately");
    }

    /* Legacy v1 migration: the old single accurate blob becomes a
     * freshness-unknown qualified record, then serializes as v2.       */
    {
        typedef struct __attribute__((packed)) {
            uint32_t magic;
            uint8_t version, rsv[3];
            uint32_t seq;
            int64_t drift_ms, lo_ms, hi_ms, half_ms, comp_ms, ref_ms;
            char fw[24];
            uint32_t crc32;
        } old_sot_t;
        old_sot_t old;
        memset(&old, 0, sizeof(old));
        old.magic = 0x534F5431u;
        old.version = 1;
        old.seq = 9;
        old.drift_ms = -100000;
        old.lo_ms = -101000;
        old.hi_ms = -99000;
        old.half_ms = 1000;
        old.comp_ms = F / 1000 + 1000;
        old.ref_ms = F / 1000 + 1000;
        strcpy(old.fw, "ezs-test");
        old.crc32 = esp_crc32_le(0, (const uint8_t *)&old,
                                 offsetof(old_sot_t, crc32));
        drift_sot_invalidate();
        CHECK(nvs_set_blob(settings_nvs_metadata(), "drift_sot",
                           &old, sizeof(old)) == ESP_OK, "seed legacy SOT");
        CHECK(drift_sot_load() == ESP_OK &&
              drift_sot_view(&sot) &&
              sot.quality == DRIFT_Q_QUALIFIED &&
              sot.freshness_unknown &&
              (sot.assumptions & DRIFT_A_LEGACY_MODEL),
              "v1 migrates to qualified slot");
        CHECK(drift_sot_commit(true, 187000000) == ESP_OK,
              "migrated SOT serializes as v2");
    }

    drift_timezone_set("UTC0", true);
    drift_profile_set(&base);
}

/* ---------------- phase tracker (update2 §3) --------------------
 * Drive the tracker over a synthetic 60 s flush at a fixed phase and
 * a 240 ms service time. A pull "executes" at its due instant; the
 * observation is a transition iff the next pending flush became
 * visible at or before the response end.                            */

static int64_t sim_svc_us  = 240000;
static int64_t sim_fl_next;             /* next flush instant        */
static int64_t sim_period  = 60000000;

static bool sim_pull(phase_t *p, int64_t now)
{
    int64_t due, gap;
    if (!phase_due(p, now, sim_svc_us, &due, &gap) || due > now) {
        return false;
    }
    phase_submitted(p);
    int64_t resp = due + sim_svc_us;
    bool tr = sim_fl_next <= resp;
    if (tr) sim_fl_next += sim_period;
    phase_obs(p, due, resp, tr);
    return true;
}

static void sim_run(phase_t *p, int64_t *now, int64_t until)
{
    int guard = 0;
    while (*now < until && guard++ < 4000) {
        if (sim_pull(p, *now)) continue;
        int64_t due, gap;
        if (phase_due(p, *now, sim_svc_us, &due, &gap) && due > *now) {
            *now = due;                       /* sleep until due     */
        } else {
            *now += 1000000;                  /* idle                */
        }
    }
}

static void test_phase(void)
{
    phase_t p;
    phase_init(&p);
    phase_config(&p, &(phase_cfg_t){ .watch_gap_us = 5000000,
                                     .rate_ppm = 500, .enabled = true,
                                     .refine = false });

    int64_t due, gap;
    CHECK(!phase_due(&p, 0, sim_svc_us, &due, &gap),
          "untracked -> nothing due");

    /* flushes become visible at 13.7 s, then every 60 s            */
    sim_fl_next = 13700000;
    phase_track(&p, "20240103", false);          /* WATCH            */
    int64_t now = 0;

    /* WATCH at 5 s spacing brackets the first flush ~5 s wide      */
    sim_run(&p, &now, 30000000);
    CHECK(p.st == PH_WATCH, "no session -> stays watching");
    CHECK(p.n_brackets == 1, "watch bracket recorded");
    CHECK(p.last_br_hi_us - p.last_br_lo_us <= 6000000 &&
          p.last_br_hi_us - p.last_br_lo_us >= 5000000,
          "watch bracket ~5 s + service");

    /* session opens: the seeded bracket jumps straight to NARROW1  */
    phase_track(&p, "20240103", true);
    CHECK(p.st == PH_NARROW1, "session + bracket -> narrow1");

    /* cycle 2: 2 s grid narrows the bracket, -> NARROW2            */
    sim_run(&p, &now, 105000000);
    CHECK(p.st == PH_NARROW2 && p.n_brackets == 2, "narrow1 -> narrow2");
    CHECK(p.last_br_hi_us - p.last_br_lo_us < 3000000,
          "narrow1 bracket ~2 s + service");

    /* cycle 3: 0.8 s grid -> VERNIER                               */
    sim_run(&p, &now, 170000000);
    CHECK(p.st == PH_VERNIER && p.n_brackets == 3, "narrow2 -> vernier");
    CHECK(p.last_br_hi_us - p.last_br_lo_us < 1600000,
          "narrow2 bracket ~0.8 s + service");

    /* cycle 4: shifted grid -> MAINTAIN (refine disabled)          */
    sim_run(&p, &now, 240000000);
    CHECK(p.st == PH_MAINTAIN && p.n_brackets == 4,
          "vernier -> maintain");

    /* steady maintenance keeps bracketing every cycle              */
    sim_run(&p, &now, 360000000);
    CHECK(p.st == PH_MAINTAIN && p.n_brackets >= 5,
          "maintain brackets each cycle");
    printf("  phase: P=[%lld,%lld] pred=[%lld,%lld] iunc=%lld\n",
           (long long)p.per_lo_us, (long long)p.per_hi_us,
           (long long)p.pred_lo_us, (long long)p.pred_hi_us,
           (long long)p.iunc_us);
    CHECK(p.period_us > 55000000 && p.period_us < 65000000,
          "period bound contains ~60 s");
    CHECK(p.iunc_us < 6000000, "prediction tightened below watch level");
    CHECK(!p.n_misses, "no misses on a stable phase");

    /* flush stops: the next armed window closes quiet -> HUNT      */
    int64_t held = sim_fl_next;
    sim_fl_next = INT64_MAX;
    sim_run(&p, &now, 500000000);
    CHECK(p.st == PH_HUNT && p.n_misses == 1, "miss -> hunt");

    /* flush resumes shifted +20 s: HUNT brackets it at 5 s and the
     * ladder restarts                                               */
    sim_fl_next = held + 20000000;
    sim_run(&p, &now, 700000000);
    CHECK(p.n_brackets >= 5, "reacquired after the phase jump");
    CHECK(p.st != PH_WATCH && p.st != PH_HUNT,
          "ladder re-entered after reacquire");

    /* beacon swap must not reuse the old file's phase              */
    phase_reacquire(&p);
    CHECK(p.st == PH_HUNT && !p.n_obs && !p.mark_us,
          "beacon swap resets the fit");

    /* day change resets everything                                 */
    phase_track(&p, "20240104", false);
    CHECK(p.st == PH_WATCH && !p.n_brackets &&
          !strcmp(p.day, "20240104"), "day change resets");

    /* a >15 s bracket is a phase inconsistency, not evidence       */
    phase_track(&p, "20240104", true);
    phase_obs(&p, 1000000, 1240000, false);
    phase_obs(&p, 3000000, 3240000, true);     /* 2 s bracket        */
    CHECK(p.st == PH_NARROW1 && p.n_brackets == 1, "narrow1 armed");
    /* consumed bound -> fresh unchanged, then a transition whose
     * bracket spans a whole cycle: the phase model was wrong       */
    phase_obs(&p, 10000000, 10240000, false);  /* unch at 10 s       */
    phase_obs(&p, 75000000, 75240000, true);   /* w=65 s             */
    CHECK(p.st == PH_HUNT && p.n_misses >= 1,
          "oversized bracket -> reacquire");

    /* failed pulls count toward the window: deterministic miss     */
    phase_track(&p, "20240105", true);
    phase_obs(&p, 1000000, 1240000, false);
    phase_obs(&p, 3000000, 3240000, true);
    CHECK(p.st == PH_NARROW1, "ladder armed again");
    /* exhaust the armed window with failures — every due pull is
     * submitted then fails; the last one closes the window         */
    int64_t t2 = 59000000;
    int g2 = 0;
    while (p.st == PH_NARROW1 && t2 < 200000000 && g2++ < 4000) {
        int64_t d2, gp;
        phase_due(&p, t2, sim_svc_us, &d2, &gp);
        if (d2 > t2) { t2 = d2; continue; }
        if (p.armed && p.pull_sent < p.pull_total) {
            phase_submitted(&p);
            phase_fail(&p, t2);
        }
        t2 += 50000;
    }
    CHECK(p.st == PH_HUNT, "all-failed window -> miss -> hunt");

    /* profile kill-switch: tracking disabled pins to fixed polls   */
    phase_track(&p, "20240105", true);
    phase_obs(&p, 1000000, 1240000, false);
    phase_obs(&p, 3000000, 3240000, true);
    phase_config(&p, &(phase_cfg_t){ .watch_gap_us = 5000000,
                                     .enabled = false });
    CHECK(phase_due(&p, 80000000, sim_svc_us, &due, &gap) &&
          gap == 0, "disabled -> fixed watch spacing");
}

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
    test_update3();
    test_phase();
    test_edf();
    test_journal();
    if (fails) {
        printf("\n%d FAILURES\n", fails);
        return 1;
    }
    printf("\nALL HOST TESTS PASSED\n");
    return 0;
}
