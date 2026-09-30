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

/* grammar per ezs_parse.h:
 *   <a href="download?fname=N&fdir=D">N</a>  timestamp  size
 *   <a href="dir?dir=D">N/</a>               timestamp  <DIR>          */
static const char *k_listing =
    "<html><body><pre>\r\n"
    "<a href=\"dir?dir=..%5CDATALOG%5C20240101\">20240101/</a>"
    "              2024-01-01 12:00:00 <DIR>\r\n"
    "<a href=\"download?fname=STR.edf&fdir=..\">STR.edf</a>"
    "              2024-01-02 03:04:05 1234\r\n"
    "</pre></body></html>";

static void test_parser(void)
{
    ezs_entry_t e[8];
    int n = 0;
    CHECK(ezs_parse_dir(k_listing, e, &n) == EZS_OK, "valid listing");
    CHECK(n == 2, "two entries");
    if (n == 2) {
        int fi = e[0].is_dir ? 1 : 0;    /* index of the FILE row     */
        CHECK(e[fi].size == 1234 && !strcmp(e[fi].name, "STR.edf"),
              "file row size/name");
        CHECK(e[fi].has_timestamp, "file timestamp kept");
        CHECK(e[1 - fi].is_dir, "dir row flagged");
    }

    const char *login = "<html><form action=\"/login\"></form></html>";
    CHECK(ezs_parse_dir(login, e, &n) == EZS_ERR_NOT_LISTING,
          "login page rejected");

    static char trunc[200];
    size_t cut = 90;                          /* inside a row         */
    memcpy(trunc, k_listing, cut);
    trunc[cut] = '\0';
    CHECK(ezs_parse_dir(trunc, e, &n) != EZS_OK, "truncated rejected");

    const char *evil =
        "<pre><a href=\"http://evil/x\">x.bin</a> 2024-01-01 00:00:00 10</pre>";
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
    CHECK(cat_feed_day("20240103", f, 2, 2000, NULL, NULL) == ESP_OK, "day feed");
    CHECK(cat_feed_day("20240102", f, 1, 3000, NULL, NULL) == ESP_OK, "older feed");
    CHECK(cat_feed_day("20240101", f, 1, 4000, NULL, NULL) == ESP_OK, "oldest feed");
    CHECK(cat_catalog_complete(), "complete after all scanned");

    cat_file_t got;
    CHECK(cat_find_file("20240103", "20240103_220000_PLD.edf", &got) &&
          got.size == 8192, "file lookup");

    /* change detection: identical re-feed -> false; size bump -> true */
    bool ch = true;
    cat_feed_day("20240103", f, 2, 5000, &ch, NULL);
    CHECK(!ch, "unchanged listing -> no change");
    f[1].size = 9000;
    cat_feed_day("20240103", f, 2, 6000, &ch, NULL);
    CHECK(ch, "grown file -> change");
    f[1].size = 8192;
    cat_feed_day("20240103", f, 2, 7000, &ch, NULL);
    CHECK(ch, "shrunk file -> change (rewrite-suspect)");

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

/* ---------------- drift anchors + fit ---------------- */

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

    drift_session_t s;
    drift_sess_begin(&s, 0, 0);
    drift_sess_header(&s, 512, 2000, 60, 0);
    CHECK(drift_sess_observe(&s, 0, 61000000, 512 + 2000), "obs1");
    CHECK(drift_sess_observe(&s, 61000000, 122000000, 512 + 4000), "obs2");
    CHECK(drift_sess_fit(&s, 55000000, 65000000,
                         -90000000, 90000000), "fit converges");
    /* 1µs fp slack: vertices can sit epsilon outside a bound         */
    CHECK(s.p_lo_us >= 55000000 - 2 && s.p_hi_us <= 65000000 + 2,
          "P within prior");

    drift_record_t rec;
    drift_build_record(&s, &rec);
    CHECK(rec.has_period, "period published even without anchors");
    CHECK(strstr(rec.flags, "timing_unvalidated"), "unvalidated flag");

    char js[2048];
    int jl = drift_record_json(&rec, "[]", NULL, js, sizeof(js));
    CHECK(jl > 50 && strstr(js, "\"estimate_state\""), "json emitted");
    CHECK(strstr(js, "\"clock_drift_ms\":null") ||
          strstr(js, "\"clock_drift_ms\":"),
          "drift field present (null or value)");
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
    test_edf();
    test_journal();
    if (fails) {
        printf("\n%d FAILURES\n", fails);
        return 1;
    }
    printf("\nALL HOST TESTS PASSED\n");
    return 0;
}
