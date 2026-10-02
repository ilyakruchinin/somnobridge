/*
 * SomnoPal — BLE ops dispatch + file streaming (spec §10.2–10.4)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <stdio.h>
#include <string.h>

#include "ble_proto.h"
#include "ble_svc.h"
#include "catalog.h"
#include "drift.h"
#include "ezshare.h"
#include "journal.h"
#include "netw.h"
#include "sched.h"
#include "settings.h"

#include "edf_hdr.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "host/ble_att.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---------------- state ---------------- */

static blep_reasm_t s_reasm;
static uint32_t     s_resp_id;

/* outbound stream state (one at a time) */
static struct {
    bool        active;
    uint32_t    request_id;
    char        canon_dir[280];
    char        name[EZS_NAME_MAX];
    char        dl_name[16];   /* listing-derived 8.3 ?file= alias   */
    uint64_t    src_size;      /* pre-listing tuple (listed token)   */
    uint8_t     src_unit_shift;/* ceil-unit of the listing display   */
    int64_t     src_clen;      /* exact Content-Length seen (-1 TBD) */
    uint64_t    sent;          /* body bytes notified               */
    uint64_t    acked;         /* contiguous ACK offset             */
    uint32_t    window;        /* remaining credit                  */
    char        src_stamp[EZS_STAMP_MAX];
    bool        has_src_stamp;
    char        day[CAT_DAY_STR];
    bool        is_evidence;   /* evidence stream: no card checks    */
    uint32_t    ev_gaps;       /* journal gaps observed while replay */
    uint8_t    *hdr_cap;       /* EDF prefix capture (diagnostic)    */
    size_t      hdr_got;
    mbedtls_sha256_context sha;
} s_stream;

static TaskHandle_t s_ble_task;

/* ---------------- JSON helpers ---------------- */

/* Append `in` at *o as a JSON string body, escaping \ and ".
 * Card paths are Windows-style ("A:\DATALOG\20260930") so raw
 * backslashes would otherwise make the response invalid JSON. */
static void json_append_escaped(char *buf, size_t cap, size_t *o,
                                const char *in)
{
    for (; in && *in && *o + 3 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\\' || c == '"') {
            buf[(*o)++] = '\\';
        }
        buf[(*o)++] = (char)c;
    }
    buf[*o] = '\0';
}

/* Outbound JSON responses are handed to a worker task rather than sent
 * inline. Every fragment is an ATT indication, and NimBLE will not free
 * its tx buffers until the peer confirms each one — those confirmations
 * are processed by the NimBLE host task's own event loop. Waiting
 * inside the host task (where op handlers run) can therefore never let
 * a confirmation land: vTaskDelay yields the CPU but does not re-enter
 * that loop, so anything past the few fragments that fit in the tx
 * queue is dropped. From a separate task the wait works.            */

typedef struct {
    uint32_t rid;
    size_t   len;
    char    *json;                 /* heap copy                        */
} tx_msg_t;

static QueueHandle_t s_txq;
static TaskHandle_t  s_tx_task;

/* Fragment + send one JSON message. Safe only off the host task. */
static uint16_t send_json_now(uint32_t rid, const char *json, size_t len)
{
    uint16_t mtu = ble_svc_att_mtu();
    /* blep_build_frame takes the whole ATT payload budget (header +
     * payload included), so subtract only the 3-byte ATT opcode and
     * handle cost — subtracting the header here too would make the
     * builder clamp each fragment 16 bytes short while the stream
     * offset still advanced by the full chunk (silent data loss).   */
    uint16_t budget = mtu > 3 ? (uint16_t)(mtu - 3) : 0;
    uint16_t room = budget > sizeof(blep_hdr_t)
                      ? (uint16_t)(budget - sizeof(blep_hdr_t)) : 0;
    if (!room) {
        return 0;
    }
    size_t off = 0;
    uint8_t frame[256];
    while (off < len || (off == 0 && len == 0)) {
        size_t n = len - off;
        if (n > room) n = room;
        bool first = off == 0, last = off + n >= len;
        uint16_t fl = blep_build_frame(frame, budget, BLEP_JSON_RESPONSE,
                                       rid, off,
                                       (const uint8_t *)json + off, n,
                                       first, last);
        int tries = 0;
        while (!ble_svc_control_indicate(frame, fl)) {
            if (!ble_svc_authorized()) {
                return off;
            }
            if (++tries >= 250) {            /* ~5 s */
                ESP_LOGW("ble", "indicate stalled at off=%u/%u",
                         (unsigned)off, (unsigned)len);
                return off;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        off += n;
    }
    return off;
}

/* Queue a JSON response for the BLE worker task. */
static void send_json_response(uint32_t rid, const char *json, size_t len)
{
    if (!s_txq || !json) {
        return;
    }
    tx_msg_t m = { .rid = rid, .len = len, .json = malloc(len + 1) };
    if (!m.json) {
        return;
    }
    memcpy(m.json, json, len);
    m.json[len] = '\0';
    if (xQueueSend(s_txq, &m, 0) != pdTRUE) {
        ESP_LOGW("ble", "tx queue full — response dropped");
        free(m.json);
    }
}

static void ble_tx_task(void *arg)
{
    tx_msg_t m;
    for (;;) {
        if (xQueueReceive(s_txq, &m, portMAX_DELAY) == pdTRUE) {
            send_json_now(m.rid, m.json, m.len);
            free(m.json);
        }
    }
}

static void send_error(uint32_t rid, const char *op, const char *code)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"ok\":false,"
                     "\"error\":{\"code\":\"%s\"}}", op, code);
    send_json_response(rid, buf, n);
}

static void send_ok(uint32_t rid, const char *op, const char *inner_json)
{
    /* inner_json is a full {...} result object body */
    char hdr[80];
    int h = snprintf(hdr, sizeof(hdr), "{\"op\":\"%s\",\"ok\":true,"
                     "\"result\":", op);
    size_t il = inner_json ? strlen(inner_json) : 0;
    size_t total = h + il + 1;
    char *msg = malloc(total + 1);
    if (!msg) {
        send_error(rid, op, EC_RESOURCE_LIMIT);
        return;
    }
    memcpy(msg, hdr, h);
    if (inner_json) {
        memcpy(msg + h, inner_json, il);
    }
    msg[h + il] = '}';
    msg[total] = '\0';
    send_json_response(rid, msg, total);    free(msg);
}

/* The final STREAM_END carries the transfer verdict (byte count and
 * SHA-256). It is an ATT indication, and NimBLE refuses a new
 * indication while the previous one is still unconfirmed (EBUSY) —
 * which is easy to hit when the stream follows hard on STREAM_BEGIN.
 * Dropping it would leave the peer without any verdict, so retry.   */
static void send_stream_end(uint32_t rid, const char *op,
                            const char *inner_json)
{
    char hdr[80];
    int h = snprintf(hdr, sizeof(hdr), "{\"op\":\"%s\",\"ok\":true,"
                     "\"result\":", op);
    size_t il = inner_json ? strlen(inner_json) : 0;
    size_t total = (size_t)h + il + 1;
    char *msg = malloc(total + 1);
    if (!msg) {
        return;
    }
    memcpy(msg, hdr, (size_t)h);
    if (inner_json) {
        memcpy(msg + h, inner_json, il);
    }
    msg[h + il] = '}';
    msg[total] = '\0';
    /* runs on the stream task, so the synchronous send (with its
     * confirmation wait) is safe here — see send_json_now */
    if (send_json_now(rid, msg, total) != total) {
        ESP_LOGW("ble", "STREAM_END indication undelivered");
    }
    free(msg);
}

/* ---------------- GET_STATUS ---------------- */

static void op_get_status(uint32_t rid, cJSON *args)
{
    const therapy_status_t *t = therapy_status();
    const netw_status_t *n = netw_status();
    char phj[400];
    int pn = sched_phase_json(phj, sizeof(phj));
    char dj[192];
    sched_diag_json(dj, sizeof(dj));
    char inner[1344];
    snprintf(inner, sizeof(inner),
        "{\"fw\":\"1\",\"proto\":1,"
        "\"security\":\"%s\","
        "\"mode\":\"%s\",\"sta_connected\":%s,\"rssi\":%d,"
        "\"paired\":%s,\"ble_connected\":%s,"
        "\"therapy\":{\"state\":%d,\"stale\":%s,"
        "\"state_since_us\":\"%lld\",\"last_growth_us\":\"%lld\","
        "\"quiet_since_ms\":%lu,\"expected_append_ms\":%lu,"
        "\"out_timeout_ms\":%lu,\"confidence\":\"%s\","
        "\"reason\":\"%s\",\"transfer_allowed\":%s},"
        "\"budget\":{\"active_8h\":\"%lu\",\"backoff_ms\":%lu,"
        "\"cap\":%d},"
        "%s,%s}",
        settings_security_label(),
        n->mode == NETW_MODE_STA ? "STA" :
        n->mode == NETW_MODE_AP ? "AP" : "DOWN",
        n->sta_connected ? "true" : "false", n->rssi,
        ble_svc_peer_bonded() ? "true" : "false",
        ble_svc_connected() ? "true" : "false",
        (int)t->state, t->therapy_stale ? "true" : "false",
        (long long)t->state_since_us,
        (long long)t->last_growth_us,
        (unsigned long)t->quiet_since_ms,
        (unsigned long)t->expected_append_ms,
        (unsigned long)t->out_timeout_ms,
        t->confidence, t->reason,
        t->transfer_allowed ? "true" : "false",
        (unsigned long)sched_active_8h(),
        (unsigned long)sched_backoff_ms(), SCHED_ACTIVE_CAP_8H,
        pn > 0 ? phj : "\"phase\":null", dj);
    send_ok(rid, OP_GET_STATUS, inner);
}

/* ---------------- GET_CATALOG_STATUS ---------------- */

static void op_catalog_status(uint32_t rid, cJSON *args)
{
    char days_buf[40][CAT_DAY_STR];
    int nh = cat_hot_days(days_buf, 8);
    char hot[96] = "[";
    for (int i = 0; i < nh && i < 4; i++) {
        strcat(hot, "\"");
        strcat(hot, days_buf[i]);
        strcat(hot, i + 1 < nh ? "\"," : "\"");
    }
    strcat(hot, "]");
    const char *st = cat_state() == CAT_READY ? "READY" :
                     cat_state() == CAT_INCOMPLETE ? "INCOMPLETE" :
                     cat_state() == CAT_ERROR ? "ERROR" : "SCANNING";
    char inner[512];
    snprintf(inner, sizeof(inner),
        "{\"state\":\"%s\",\"folders_total\":%d,"
        "\"catalog_complete\":%s,\"hot_ready\":%s,"
        "\"incomplete_count\":%d,\"psram_used\":%d,"
        "\"hot_days\":%s}",
        st, cat_folder_count(),
        cat_catalog_complete() ? "true" : "false",
        cat_hot_ready() ? "true" : "false",
        cat_incomplete_count(), cat_psram_used(), hot);
    send_ok(rid, OP_GET_CATALOG_STATUS, inner);
}

/* ---------------- LIST_RECENT_DAYS / LIST_DAY ---------------- */

static void op_list_recent(uint32_t rid, cJSON *args)
{
    cJSON *v = cJSON_GetObjectItem(args, "days");
    int n = v && cJSON_IsNumber(v) ? v->valueint : 14;
    if (n < 1 || n > 36600) {
        send_error(rid, OP_LIST_RECENT_DAYS, EC_BAD_REQUEST);
        return;
    }
    char days[64][CAT_DAY_STR];
    int nd = cat_list_days(days, 64);
    /* take newest n */
    if (nd > n) nd = n;
    char *buf = malloc(64 * 11 + 256);
    if (!buf) { send_error(rid, OP_LIST_RECENT_DAYS, EC_RESOURCE_LIMIT); return; }
    strcpy(buf, "{\"days\":[");
    for (int i = 0; i < nd; i++) {
        strcat(buf, "\"");
        strcat(buf, days[i]);
        strcat(buf, "\"");
        if (i + 1 < nd) strcat(buf, ",");
    }
    strcat(buf, "],\"catalog_complete\":");
    strcat(buf, cat_catalog_complete() ? "true" : "false");
    strcat(buf, ",\"hot_ready\":");
    strcat(buf, cat_hot_ready() ? "true" : "false");
    strcat(buf, "}");
    send_ok(rid, OP_LIST_RECENT_DAYS, buf);
    free(buf);
}

static void op_list_day(uint32_t rid, cJSON *args)
{
    cJSON *v = cJSON_GetObjectItem(args, "day");
    if (!v || !cJSON_IsString(v) || strlen(v->valuestring) != 8) {
        send_error(rid, OP_LIST_DAY, EC_BAD_REQUEST);
        return;
    }
    const char *day = v->valuestring;
    cat_folder_state_t st = cat_day_state(day);
    if (st == CAT_FOLDER_GONE) {
        send_error(rid, OP_LIST_DAY, EC_NOT_FOUND);
        return;
    }
    if (st == CAT_FOLDER_UNSCANNED || st == CAT_FOLDER_EVICTED) {
        send_error(rid, OP_LIST_DAY, EC_NOT_SCANNED);
        return;
    }

    int n = cat_day_entries(day, NULL, 0);
    /* serialize under a response view (bounded) */
    cat_file_t *files = malloc(n * sizeof(cat_file_t));
    if (!files) { send_error(rid, OP_LIST_DAY, EC_RESOURCE_LIMIT); return; }
    cat_day_entries(day, files, n);

    size_t cap = 96 + (size_t)n * 160;
    char *buf = malloc(cap);
    if (!buf) { free(files); send_error(rid, OP_LIST_DAY, EC_RESOURCE_LIMIT); return; }
    int o = snprintf(buf, cap, "{\"day\":\"%s\",\"folder_stale\":false,"
                     "\"scan_age_us\":\"%lld\",\"files\":[",
                     day, (long long)cat_day_last_scan(day));
    for (int i = 0; i < n && o < (int)cap - 200; i++) {
        o += snprintf(buf + o, cap - o,
            "{\"name\":\"%s\",\"size\":\"%llu\",\"ts\":%s%s%s}%s",
            files[i].name, (unsigned long long)files[i].size,
            files[i].has_timestamp ? "\"" : "",
            files[i].has_timestamp ? files[i].timestamp_raw : "null",
            files[i].has_timestamp ? "\"" : "",
            i + 1 < n ? "," : "");
    }
    snprintf(buf + o, cap - o, "]}");
    send_ok(rid, OP_LIST_DAY, buf);
    free(files);
    free(buf);
}

/* ---------------- LIST_DIRECTORY (live) ---------------- */

static void op_list_dir(uint32_t rid, cJSON *args)
{
    cJSON *v = cJSON_GetObjectItem(args, "directory");
    if (!v || !cJSON_IsString(v)) {
        send_error(rid, OP_LIST_DIRECTORY, EC_BAD_REQUEST);
        return;
    }
    if (!therapy_status()->transfer_allowed) {
        send_error(rid, OP_LIST_DIRECTORY,
                   therapy_status()->state == THERAPY_IN
                       ? EC_THERAPY_ACTIVE : EC_THERAPY_UNKNOWN);
        return;
    }
    char canon[260];
    if (ezp_canon_dir(v->valuestring, canon, sizeof(canon)) != EZP_OK) {
        send_error(rid, OP_LIST_DIRECTORY, EC_PATH_INVALID);
        return;
    }
    if (ezp_is_datalog(canon)) {
        send_error(rid, OP_LIST_DIRECTORY, EC_PATH_INVALID);
        return;
    }
    ezs_entry_t *ents = malloc(EZS_ENTRIES_MAX * sizeof(ezs_entry_t));
    if (!ents) { send_error(rid, OP_LIST_DIRECTORY, EC_RESOURCE_LIMIT); return; }
    int n = 0; ezw_obs_t obs;
    esp_err_t err = ezs_list_dir(canon, ents, &n, &obs);
    if (err != ESP_OK) {
        free(ents);
        send_error(rid, OP_LIST_DIRECTORY,
                   obs.http_status >= 400 ? EC_HTTP_ERROR : EC_TIMEOUT);
        return;
    }
    size_t cap = 96 + (size_t)n * 160;
    char *buf = malloc(cap);
    if (!buf) { free(ents); send_error(rid, OP_LIST_DIRECTORY, EC_RESOURCE_LIMIT); return; }
    size_t o = (size_t)snprintf(buf, cap, "{\"directory\":\"");
    json_append_escaped(buf, cap, &o, canon);
    o += (size_t)snprintf(buf + o, cap - o, "\",\"files\":[");
    for (int i = 0; i < n && o < cap - 200; i++) {
        if (ents[i].is_dir) continue;
        o += (size_t)snprintf(buf + o, cap - o, "{\"name\":\"");
        json_append_escaped(buf, cap, &o, ents[i].name);
        o += (size_t)snprintf(buf + o, cap - o,
                              "\",\"size\":\"%llu\",\"ts\":",
                              (unsigned long long)ents[i].size);
        if (ents[i].has_timestamp) {
            o += (size_t)snprintf(buf + o, cap - o, "\"");
            json_append_escaped(buf, cap, &o, ents[i].timestamp_raw);
            o += (size_t)snprintf(buf + o, cap - o, "\"");
        } else {
            o += (size_t)snprintf(buf + o, cap - o, "null");
        }
        o += (size_t)snprintf(buf + o, cap - o, "},");
    }
    if (o > 0 && buf[o-1] == ',') o--;      /* trim trailing comma */
    snprintf(buf + o, cap - o, "]}");
    send_ok(rid, OP_LIST_DIRECTORY, buf);
    free(ents);
    free(buf);
}

/* ---------------- GET_FILE streaming ---------------- */

static bool stream_sink(const uint8_t *d, size_t n, void *ctx);

static void op_get_file(uint32_t rid, cJSON *args)
{
    cJSON *dv = cJSON_GetObjectItem(args, "directory");
    cJSON *nv = cJSON_GetObjectItem(args, "name");
    cJSON *ev = cJSON_GetObjectItem(args, "expected_size");
    if (!dv || !nv || !cJSON_IsString(dv) || !cJSON_IsString(nv)) {
        send_error(rid, OP_GET_FILE, EC_BAD_REQUEST);
        return;
    }
    if (s_stream.active) {
        send_error(rid, OP_GET_FILE, EC_BUSY);
        return;
    }
    if (!therapy_status()->transfer_allowed) {
        send_error(rid, OP_GET_FILE,
                   therapy_status()->state == THERAPY_IN
                       ? EC_THERAPY_ACTIVE : EC_THERAPY_UNKNOWN);
        return;
    }

    char canon[280];
    if (ezp_canon_dir(dv->valuestring, canon, sizeof(canon)) != EZP_OK ||
        ezp_validate_name(nv->valuestring) != EZP_OK) {
        send_error(rid, OP_GET_FILE, EC_PATH_INVALID);
        return;
    }

    /* server-side pre-checks (§12.1): therapy already gated; now
     * refresh the source dir and match the tuple BEFORE streaming   */
    bool in_datalog = ezp_is_datalog(canon);
    char day[CAT_DAY_STR] = "";
    if (in_datalog) {
        if (!ezp_is_datalog_day(canon) ||
            !ezp_datalog_day(canon, day)) {
            send_error(rid, OP_GET_FILE, EC_PATH_INVALID);
            return;
        }
        /* refresh source folder via catalog (the scheduler feeds it);
         * require the resident listing to be fresh (<=90 s)          */
        int64_t last = cat_day_last_scan(day);
        if (esp_timer_get_time() - last > 90LL * 1000 * 1000) {
            sched_job_t j = { .kind = JOB_DAY, .prio = 1,
                              .deadline_us = esp_timer_get_time() +
                                            10LL * 1000 * 1000,
                              .required = true };
            strncpy(j.dir, day, CAT_DAY_STR);
            sched_submit(&j);
            send_error(rid, OP_GET_FILE, EC_STALE);
            return;
        }
    } else {
        /* live listing of the outside dir for the pre-tuple         */
        ezs_entry_t *ents = malloc(EZS_ENTRIES_MAX * sizeof(ezs_entry_t));
        if (!ents) { send_error(rid, OP_GET_FILE, EC_RESOURCE_LIMIT); return; }
        int n = 0; ezw_obs_t o;
        esp_err_t e = ezs_list_dir(canon, ents, &n, &o);
        if (e != ESP_OK) {
            free(ents);
            send_error(rid, OP_GET_FILE, EC_HTTP_ERROR);
            return;
        }
        /* verify source tuple exists */
        bool ok = false;
        for (int i = 0; i < n; i++) {
            if (!ents[i].is_dir && !strcmp(ents[i].name, nv->valuestring)) {
                ok = true;
                s_stream.src_size = ents[i].size;
                s_stream.src_unit_shift = ents[i].size_unit_shift;
                s_stream.has_src_stamp = ents[i].has_timestamp;
                strncpy(s_stream.src_stamp, ents[i].timestamp_raw,
                        sizeof(s_stream.src_stamp)-1);
                strncpy(s_stream.dl_name, ents[i].dl_name,
                        sizeof(s_stream.dl_name)-1);
                break;
            }
        }
        free(ents);
        if (!ok) {
            send_error(rid, OP_GET_FILE, EC_NOT_FOUND);
            return;
        }
    }

    /* resolve catalog tuple for DATALOG files */
    cat_file_t ft;
    if (in_datalog) {
        if (!cat_find_file(day, nv->valuestring, &ft)) {
            send_error(rid, OP_GET_FILE, EC_NOT_FOUND);
            return;
        }
        s_stream.src_size = ft.size;
        s_stream.src_unit_shift = ft.size_unit_shift;
        s_stream.has_src_stamp = ft.has_timestamp;
        strncpy(s_stream.src_stamp, ft.timestamp_raw,
                sizeof(s_stream.src_stamp)-1);
        strncpy(s_stream.dl_name, ft.dl_name,
                sizeof(s_stream.dl_name)-1);
    }
    s_stream.dl_name[sizeof(s_stream.dl_name) - 1] = '\0';
    if (ev && cJSON_IsString(ev)) {
        /* the client pins the LISTED size token it saw — the exact
         * Content-Length check happens inside the envelope (U8)     */
        uint64_t want = strtoull(ev->valuestring, NULL, 10);
        if (want != s_stream.src_size) {
            send_error(rid, OP_GET_FILE, EC_SIZE_MISMATCH);
            return;
        }
    }

    /* STREAM_BEGIN (heap: this runs on the NimBLE host task, whose
     * stack must stay small) */
    char *begin = malloc(640);
    if (!begin) {
        send_error(rid, OP_GET_FILE, EC_RESOURCE_LIMIT);
        return;
    }
    size_t bo = (size_t)snprintf(begin, 640,
        "{\"stream_type\":\"file\",\"directory\":\"");
    json_append_escaped(begin, 640, &bo, canon);
    bo += (size_t)snprintf(begin + bo, 640 - bo, "\",\"name\":\"");
    json_append_escaped(begin, 640, &bo, nv->valuestring);
    bo += (size_t)snprintf(begin + bo, 640 - bo,
        "\",\"download_name\":\"");
    json_append_escaped(begin, 640, &bo,
                        s_stream.dl_name[0] ? s_stream.dl_name
                                            : nv->valuestring);
    snprintf(begin + bo, 640 - bo,
        "\",\"listed_size\":\"%llu\",\"size_unit_shift\":%u,"
        "\"source_before\":{\"size\":\"%llu\",\"ts\":%s%s%s}}",
        (unsigned long long)s_stream.src_size,
        (unsigned)s_stream.src_unit_shift,
        (unsigned long long)s_stream.src_size,
        s_stream.has_src_stamp ? "\"" : "",
        s_stream.has_src_stamp ? s_stream.src_stamp : "null",
        s_stream.has_src_stamp ? "\"" : "");
    s_stream.active = true;
    s_stream.request_id = rid;
    s_stream.sent = s_stream.acked = 0;
    s_stream.window = 0;
    s_stream.is_evidence = false;
    s_stream.src_clen = -1;
    strncpy(s_stream.canon_dir, canon, sizeof(s_stream.canon_dir)-1);
    strncpy(s_stream.name, nv->valuestring, sizeof(s_stream.name)-1);
    strncpy(s_stream.day, day, CAT_DAY_STR-1);
    /* opportunistic EDF prefix capture: the first ≤16 KiB of a DATALOG
     * .edf body feeds the header diagnostic — zero extra card reads  */
    s_stream.hdr_cap = NULL;
    s_stream.hdr_got = 0;
    if (in_datalog && strstr(nv->valuestring, ".edf")) {
        s_stream.hdr_cap = malloc(16 * 1024);
    }
    send_ok(rid, "STREAM_BEGIN", begin);   /* piggybacks rid          */
    free(begin);

    /* initial credit comes as the first ACK (offset 0 + window) — the
     * task below waits for it then drives ezs_download.            */
    xTaskNotifyGive(s_ble_task);
}

/* sink: sends FILE_BYTES frames honoring credit window */
static bool stream_sink(const uint8_t *d, size_t n, void *ctx)
{
    /* capture the EDF prefix while it streams past (≤16 KiB, U2 —
     * diagnostics only; never gates the transfer)                  */
    if (s_stream.hdr_cap && s_stream.hdr_got < 16 * 1024) {
        size_t room = 16 * 1024 - s_stream.hdr_got;
        size_t take = n < room ? n : room;
        memcpy(s_stream.hdr_cap + s_stream.hdr_got, d, take);
        s_stream.hdr_got += take;
    }
    size_t off = 0;
    while (off < n) {
        while (s_stream.window == 0) {
            /* wait for credit — abort/link-loss must also break it    */
            vTaskDelay(pdMS_TO_TICKS(5));
            if (!s_stream.active || !ble_svc_authorized()) {
                return false;
            }
        }
        size_t take = n - off;
        /* budget = whole ATT payload (header + payload); the chunk
         * must not exceed the builder's room, or the frame silently
         * carries fewer bytes than s_stream.sent advances by.       */
        uint16_t mtu = ble_svc_att_mtu();
        uint16_t budget = mtu > 3 ? (uint16_t)(mtu - 3) : 0;
        uint16_t room = budget > sizeof(blep_hdr_t)
                          ? (uint16_t)(budget - sizeof(blep_hdr_t)) : 0;
        if (!room) {
            return false;
        }
        if (take > room) take = room;
        if (take > s_stream.window) take = s_stream.window;

        uint8_t frame[260];
        bool last = (off + take >= n);
        uint16_t fl = blep_build_frame(
            frame, budget, BLEP_FILE_BYTES, s_stream.request_id,
            s_stream.sent, d + off, take,
            s_stream.sent == 0, last);
        if (!ble_svc_send_bytes(frame, fl)) {
            return false;
        }
        mbedtls_sha256_update(&s_stream.sha, d + off, take);
        s_stream.sent += take;
        s_stream.window -= take;
        off += take;
    }
    return true;
}

/* ---------------- GET_EVIDENCE ----------------
 * Streams journal payloads oldest->newest through the same credit +
 * SHA-256 machinery. No card post-checks (§10.2): the source is our
 * own NVS — transport integrity is the only gate.                  */

static bool ev_cb(const char *p, uint16_t len, uint32_t seq, void *ctx)
{
    (void)seq; (void)ctx;
    return stream_sink((const uint8_t *)p, len, NULL);
}

static void op_get_evidence(uint32_t rid)
{
    if (s_stream.active) {
        send_error(rid, OP_GET_EVIDENCE, EC_BUSY);
        return;
    }
    if (jrn_health() == JRN_OFF) {
        send_error(rid, OP_GET_EVIDENCE, EC_METADATA_UNAVAILABLE);
        return;
    }
    char begin[192];
    snprintf(begin, sizeof(begin),
        "{\"stream_type\":\"evidence\",\"journal_seq\":\"%lu\","
        "\"oldest_seq\":\"%lu\",\"truncated\":%s}",
        (unsigned long)jrn_seq(), (unsigned long)jrn_oldest_seq(),
        jrn_truncated() ? "true" : "false");
    s_stream.active = true;
    s_stream.request_id = rid;
    s_stream.sent = s_stream.acked = 0;
    s_stream.window = 0;
    s_stream.is_evidence = true;
    s_stream.ev_gaps = 0;
    s_stream.day[0] = '\0';
    send_ok(rid, "STREAM_BEGIN", begin);
    xTaskNotifyGive(s_ble_task);
}

/* BLE writer task: runs the download + post-checks + STREAM_END */
static void ble_stream_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_stream.active) {
            continue;
        }

        /* wait for initial credit (ACK with window>0) */
        int64_t deadline = esp_timer_get_time() + 10LL * 1000 * 1000;
        while (s_stream.window == 0 &&
               esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_stream.window == 0) {
            goto finish_fail;
        }

        mbedtls_sha256_init(&s_stream.sha);
        mbedtls_sha256_starts(&s_stream.sha, 0);
        bool transport_ok;
        if (s_stream.is_evidence) {
            /* journal replay — no card I/O, no post-checks (§10.2)   */
            transport_ok =
                jrn_walk(ev_cb, NULL, &s_stream.ev_gaps) == ESP_OK;
        } else {
            ezw_obs_t obs;
            esp_err_t err =
                ezs_download(s_stream.canon_dir, s_stream.name,
                             s_stream.dl_name, s_stream.src_size,
                             s_stream.src_unit_shift,
                             stream_sink, NULL, &obs);
            s_stream.src_clen = obs.content_length;
            transport_ok = err == ESP_OK;
        }
        uint8_t hash[32] = {0};
        char hex[65];
        if (transport_ok) {
            mbedtls_sha256_finish(&s_stream.sha, hash);
        }
        mbedtls_sha256_free(&s_stream.sha);
        for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);
        hex[64] = '\0';

        if (s_stream.is_evidence) {
            char end[384];
            snprintf(end, sizeof(end),
                "{\"ok\":%s,\"actual_bytes\":\"%llu\","
                "\"sha256\":\"%s\",\"gaps\":%lu,\"truncated\":%s,"
                "\"completion_allowed\":%s,\"error\":\"%s\"}",
                transport_ok ? "true" : "false",
                (unsigned long long)s_stream.sent, hex,
                (unsigned long)s_stream.ev_gaps,
                jrn_truncated() ? "true" : "false",
                transport_ok ? "true" : "false",
                transport_ok ? "ok" : "journal_error");
            send_stream_end(s_stream.request_id, "STREAM_END", end);
            s_stream.active = false;
            s_stream.is_evidence = false;
            continue;
        }

        /* post-check: therapy + source tuple refresh (§12.1 step 6-7) */
        bool completion = false;
        char reason[40] = "ok";
        if (transport_ok) {
            /* give the scheduler a beat, then refresh */
            sched_job_t j = { .kind = JOB_DAY, .prio = 0,
                              .deadline_us = esp_timer_get_time() +
                                            5LL * 1000 * 1000,
                              .required = true };
            strncpy(j.dir, s_stream.day, CAT_DAY_STR);
            sched_submit(&j);
            /* wait a fixed settle window; the refresh lands in catalog */
            vTaskDelay(pdMS_TO_TICKS(2500));
            const therapy_status_t *t = therapy_status();
            if (t->state == THERAPY_IN) {
                strncpy(reason, "therapy_active", sizeof(reason));
            } else if (t->state != THERAPY_OUT) {
                strncpy(reason, "therapy_unknown", sizeof(reason));
            } else if (s_stream.day[0]) {
                cat_file_t after;
                if (!cat_find_file(s_stream.day, s_stream.name, &after) ||
                    after.size != s_stream.src_size ||
                    (s_stream.has_src_stamp && after.has_timestamp &&
                     strcmp(after.timestamp_raw,
                            s_stream.src_stamp))) {
                    /* tuple moved under us — size *or* stamp (U3)  */
                    strncpy(reason, "source_changed", sizeof(reason));
                } else {
                    completion = true;
                }
            } else {
                /* outside-DATALOG: re-list live                      */
                ezs_entry_t *ents =
                    malloc(EZS_ENTRIES_MAX * sizeof(ezs_entry_t));
                int n = 0; ezw_obs_t o2;
                bool ok2 = ents &&
                    ezs_list_dir(s_stream.canon_dir, ents, &n, &o2) ==
                        ESP_OK;
                bool match = false;
                if (ok2) {
                    for (int i = 0; i < n; i++) {
                        if (!ents[i].is_dir &&
                            !strcmp(ents[i].name, s_stream.name) &&
                            ents[i].size == s_stream.src_size) {
                            match = true;
                        }
                    }
                }
                free(ents);
                completion = match;
                if (!match) strncpy(reason, "source_unverified",
                                    sizeof(reason));
            }
        } else {
            strncpy(reason, "http_error", sizeof(reason));
        }

        /* opportunistic EDF-header diagnostic — parse the captured
         * prefix; never gates completion (U2)                        */
        if (s_stream.hdr_cap) {
            edf_hdr_t h;
            if (edf_hdr_parse(s_stream.hdr_cap, s_stream.hdr_got, &h)) {
                drift_session_t *ls = drift_last();
                if (ls && s_stream.day[0] &&
                    !strcmp(ls->day, s_stream.day)) {
                    drift_sess_header(ls, h.header_bytes, h.record_bytes,
                                      h.record_duration_s,
                                      h.start_local_s);
                    if (h.record_bytes &&
                        s_stream.sent > h.header_bytes) {
                        ls->complete_records =
                            (int64_t)((s_stream.sent - h.header_bytes) /
                                      h.record_bytes);
                    }
                }
            }
            free(s_stream.hdr_cap);
            s_stream.hdr_cap = NULL;
        }

        {
            char end[640];
            snprintf(end, sizeof(end),
                "{\"ok\":%s,\"actual_bytes\":\"%llu\","
                "\"listed_size\":\"%llu\",\"content_length\":\"%lld\","
                "\"sha256\":\"%s\","
                "\"source_unchanged\":%s,"
                "\"therapy_status\":%d,\"monitoring_checked\":true,"
                "\"completion_allowed\":%s,\"error\":\"%s\"}",
                transport_ok ? "true" : "false",
                (unsigned long long)s_stream.sent,
                (unsigned long long)s_stream.src_size,
                (long long)s_stream.src_clen, hex,
                completion ? "true" : "false",
                (int)therapy_status()->state,
                completion ? "true" : "false", reason);
            send_stream_end(s_stream.request_id, "STREAM_END", end);
        }
        s_stream.active = false;
        continue;

finish_fail:
        s_stream.active = false;
    }
}

/* called when BLE link drops — abort in-flight stream */
void ble_svc_stream_abort(uint32_t request_id, const char *why)
{
    if (s_stream.active) {
        s_stream.active = false;
        s_stream.window = 0;
        send_error(s_stream.request_id, "STREAM_END", EC_LINK_LOST);
    }
}

/* ---------------- time sync (§8.1 / §10.4) ---------------- */

static void on_time_request(uint32_t rid, const uint8_t *pl, uint16_t n)
{
    if (n < 16) return;
    uint64_t t1; uint32_t unc, epoch;
    memcpy(&t1, pl, 8);
    memcpy(&unc, pl + 8, 4);
    memcpy(&epoch, pl + 12, 4);
    int64_t m2 = esp_timer_get_time();   /* complete-request receipt  */
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI("ble", "heap_ck time_req pre-begin: %s",
             heap_caps_check_integrity_all(false) ? "OK" : "CORRUPT");
#endif
    if (drift_time_begin(rid, t1, unc, epoch, m2) != ESP_OK) {
        send_error(rid, "TIME_REQUEST", EC_BAD_REQUEST);
        return;
    }
    /* §10.4: t1:u64, m2:u64, m3:u64, echoed epoch:u32 = 28 bytes.
     * The epoch must follow m3 — writing it at offset 20 (as if the
     * reply were 24 bytes) overwrites m3's high dword and destroys
     * the RTT computation on the peer.                              */
    uint8_t buf[28];
    memcpy(buf, &t1, 8);
    memcpy(buf + 8, &m2, 8);
    int64_t m3 = esp_timer_get_time();   /* just before first fragment */
    memcpy(buf + 16, &m3, 8);
    memcpy(buf + 24, &epoch, 4);
    drift_time_reply_tsp(rid, m3);
    uint8_t frame[64];
    uint16_t fl = blep_build_frame(frame, sizeof(frame),
                                   BLEP_TIME_REPLY, rid, 0,
                                   buf, sizeof(buf), true, true);
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI("ble", "heap_ck time_req pre-indicate: %s",
             heap_caps_check_integrity_all(false) ? "OK" : "CORRUPT");
#endif
    ble_svc_control_indicate(frame, fl);
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI("ble", "heap_ck time_req done: %s",
             heap_caps_check_integrity_all(false) ? "OK" : "CORRUPT");
#endif
}

static void on_time_commit(uint32_t rid, const uint8_t *pl, uint16_t n)
{
    if (n < 20) return;
    uint64_t t1, t4; uint32_t ep;
    memcpy(&t1, pl, 8);
    memcpy(&t4, pl + 8, 8);
    memcpy(&ep, pl + 16, 4);
    (void)t1; (void)ep;   /* drift layer validates exact correlation  */
    uint32_t unc = 0;
    anchor_result_t r = drift_time_commit(rid, t4,
                                          esp_timer_get_time(), &unc);
    static const char *why[] = {
        "ok", "rtt", "uncertainty", "stale", "new_epoch",
        "discontinuity", "bad",
    };
    bool ok = r == ANCHOR_OK || r == ANCHOR_EPOCH;
    char inner[192];
    snprintf(inner, sizeof(inner),
             "{\"accepted\":%s,\"reason\":\"%s\","
             "\"uncertainty_us\":%lu,\"anchors\":%d}",
             ok ? "true" : "false", why[r],
             (unsigned long)unc, drift_anchor_count());
    /* evidence: anchors are the drift record's backbone (§9)          */
    if (ok) {
        char rec[192];
        snprintf(rec, sizeof(rec),
                 "\"u_us\":%lu,\"n\":%d",
                 (unsigned long)unc, drift_anchor_count());
        jrn_put("anchor", rec);
    } else {
        char rec[96];
        snprintf(rec, sizeof(rec), "\"rej\":\"%s\"", why[r]);
        jrn_put("anchor", rec);
    }
    send_ok(rid, "TIME_COMMIT", inner);
}

/* ---------------- GET_DRIFT ----------------
 * U10: takes no file arguments. Returns an envelope with the live (or
 * last closed) session estimate plus the persisted source of truth.
 * Approximate estimates are reported as their true interval; only an
 * accurate-tier value ever appears in source_of_truth — and when no
 * accurate value exists the field is an explicit null, never a guess. */

static void op_get_drift(uint32_t rid, cJSON *args)
{
    /* Legacy readers get the qualified-only source_of_truth. A new
     * reader must explicitly ask for the best-available view before a
     * provisional persisted record is exposed to it.                 */
    bool best_req = false;
    if (args) {
        cJSON *b = cJSON_GetObjectItem(args, "best_available");
        cJSON *p = cJSON_GetObjectItem(args, "include_provisional");
        best_req = cJSON_IsTrue(b) || cJSON_IsTrue(p);
    }
    char *buf = malloc(4096);
    if (!buf) {
        send_error(rid, OP_GET_DRIFT, EC_RESOURCE_LIMIT);
        return;
    }
    /* Checked serializer: every append is bounded, a truncated field
     * pins o at the cap, and an overlong payload is an explicit error
     * rather than a clipped document (update3 §11).                */
    size_t o = 0;
#define DAPP(...)                                                     \
    do {                                                              \
        if (o < 4096) {                                               \
            int w_ = snprintf(buf + o, 4096 - o, __VA_ARGS__);        \
            if (w_ >= 0 && (size_t)w_ < 4096 - o) o += (size_t)w_;    \
            else o = 4096;                                            \
        }                                                             \
    } while (0)
#define DJSON(fn, arg)                                                \
    do {                                                              \
        int jn_ = fn((arg), buf + (o < 4096 ? o : 4095),              \
                     4096 - (o < 4096 ? o : 4095));                   \
        if (jn_ < 0 || o + (size_t)jn_ >= 4096) o = 4096;             \
        else o += (size_t)jn_;                                        \
    } while (0)
    DAPP("{\"schema\":3,\"estimate\":");
    drift_record_t rec;
    if (o < 4096 && drift_pub_get(&rec)) {
        DJSON(drift_estimate_json, &rec);
    } else {
        DAPP("null");
    }
    DAPP(",\"source_of_truth\":");
    drift_sot_t sot;
    if (o < 4096 && drift_sot_view(&sot)) {
        DJSON(drift_sot_json, &sot);
    } else {
        DAPP("null");
    }
    DAPP(",\"qualified_source_of_truth\":");
    if (o < 4096 && drift_sot_slot_view(DRIFT_SOT_QUALIFIED, &sot)) {
        DJSON(drift_sot_json, &sot);
    } else {
        DAPP("null");
    }
    DAPP(",\"provisional_source_of_truth\":");
    if (o < 4096 && drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, &sot)) {
        DJSON(drift_sot_json, &sot);
    } else {
        DAPP("null");
    }
    DAPP(",\"best_available_opt_in_required\":true,"
         "\"best_available_offset\":");
    bool have_best = o < 4096 && best_req && drift_sot_best(&sot);
    if (have_best) {
        DJSON(drift_sot_json, &sot);
    } else {
        DAPP("null");
    }
    DAPP(",\"best_available_reason\":\"%s\"",
         !best_req ? "client_opt_in_required" :
         have_best ? "selected_by_quality_then_width" :
                     "no_persisted_record");
    if (o >= 4095) {
        send_error(rid, OP_GET_DRIFT, EC_RESOURCE_LIMIT);
        free(buf);
        return;
    }
    snprintf(buf + o, 4096 - o, "}");
    send_ok(rid, OP_GET_DRIFT, buf);
    free(buf);
#undef DAPP
#undef DJSON
}

/* ---------------- write dispatch ---------------- */

int ble_ops_on_write(uint16_t conn, const uint8_t *data, uint16_t len)
{
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI("ble", "heap_ck on_write len=%u: %s", (unsigned)len,
             heap_caps_check_integrity_all(false) ? "OK" : "CORRUPT");
#endif
    if (len < sizeof(blep_hdr_t)) {
        return BLE_ATT_ERR_INVALID_PDU;
    }
    blep_hdr_t h;
    memcpy(&h, data, sizeof(h));

    switch (h.kind) {
    case BLEP_JSON_REQUEST: {
        uint32_t rid;
        bool done = blep_reasm_feed(&s_reasm, data, len,
                                    esp_timer_get_time() / 1000, &rid);
        if (!done) {
            return 0;                 /* more fragments coming        */
        }
        s_resp_id = rid;
        cJSON *j = cJSON_ParseWithLength((const char *)s_reasm.buf,
                                         s_reasm.len);
        if (!j) {
            send_error(rid, "?", EC_BAD_REQUEST);
            return 0;
        }
        cJSON *op = cJSON_GetObjectItem(j, "op");
        cJSON *args = cJSON_GetObjectItem(j, "args");
        const char *opn = cJSON_IsString(op) ? op->valuestring : "";
        if (!strcmp(opn, OP_GET_STATUS)) op_get_status(rid, args);
        else if (!strcmp(opn, OP_GET_CATALOG_STATUS))
            op_catalog_status(rid, args);
        else if (!strcmp(opn, OP_LIST_RECENT_DAYS))
            op_list_recent(rid, args);
        else if (!strcmp(opn, OP_LIST_DAY)) op_list_day(rid, args);
        else if (!strcmp(opn, OP_LIST_DIRECTORY)) op_list_dir(rid, args);
        else if (!strcmp(opn, OP_GET_FILE)) op_get_file(rid, args);
        else if (!strcmp(opn, OP_GET_DRIFT)) op_get_drift(rid, args);
        else if (!strcmp(opn, OP_GET_EVIDENCE)) op_get_evidence(rid);
        else send_error(rid, opn, EC_UNSUPPORTED_OP);
        cJSON_Delete(j);
        return 0;
    }
    case BLEP_ACK: {
        const uint8_t *pl = data + sizeof(h);
        if (len >= sizeof(h) + 4 && s_stream.active &&
            h.request_id == s_stream.request_id) {
            uint32_t w;
            memcpy(&w, pl, 4);
            s_stream.acked = h.offset;
            s_stream.window = w;
        }
        return 0;
    }
    case BLEP_ABORT:
        if (s_stream.active && h.request_id == s_stream.request_id) {
            s_stream.active = false;
        }
        return 0;
    case BLEP_TIME_REQUEST:
        on_time_request(h.request_id, data + sizeof(h),
                        len - sizeof(h));
        return 0;
    case BLEP_TIME_COMMIT:
        on_time_commit(h.request_id, data + sizeof(h),
                       len - sizeof(h));
        return 0;
    default:
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
}

int ble_ops_on_subscribe(uint16_t conn, uint16_t attr, bool sub)
{
    return 0;
}

esp_err_t ble_ops_init(void)
{
    blep_reasm_init(&s_reasm);
    s_txq = xQueueCreate(8, sizeof(tx_msg_t));
    if (!s_txq) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(ble_tx_task, "ble_tx", 4096, NULL, 6, &s_tx_task);
    xTaskCreate(ble_stream_task, "ble_stream", 6144, NULL, 8,
                &s_ble_task);
    return ESP_OK;
}
