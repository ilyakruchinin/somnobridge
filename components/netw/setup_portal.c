/*
 * SomnoPal — setup portal (AP-mode HTTP server, spec §4.1)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Security model: short-lived open AP + random session cookie + CSRF
 * token (minted once per portal run). Bounded bodies, no CORS, POST-only
 * mutations, secrets never returned. Destructive actions are queued and
 * only execute after a separate physical button press (§2.1).
 */
#include <stdio.h>
#include <string.h>

#include "journal.h"
#include "netw.h"
#include "settings.h"
#include "drift.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "portal";

static httpd_handle_t s_srv;
static char           s_sess[17];      /* cookie value, hex       */
static char           s_csrf[17];      /* header value, hex       */
static int64_t        s_last_activity_us;

#define BODY_MAX 2048
/* httpd invokes handlers serially on a single task; a static body buffer
 * keeps ~2KB off the 4KB-default httpd stack. */
static char s_body[BODY_MAX];

/* pending physical-confirm action (§2.1) — set by /api/action, executed
 * by the app after a short press within 30 s. */
static volatile int s_pending_action;  /* portal_action_t (netw.h)   */
static volatile int64_t s_pending_since_us;

/* app-facing helpers ---------------------------------------------------- */

int  portal_pending_action(void)        { return s_pending_action; }
int64_t portal_pending_since_us(void)   { return s_pending_since_us; }
void portal_clear_pending(void)         { s_pending_action = PORTAL_ACTION_NONE; }

static void touch(void) { s_last_activity_us = esp_timer_get_time(); }

/* ---------------- helpers ---------------- */

static bool cookie_ok(httpd_req_t *r)
{
    char hdr[96];
    if (httpd_req_get_hdr_value_str(r, "Cookie", hdr, sizeof(hdr)) !=
        ESP_OK) {
        return false;
    }
    char want[32];
    snprintf(want, sizeof(want), "sp_sess=%s", s_sess);
    return strstr(hdr, want) != NULL;
}

static bool csrf_ok(httpd_req_t *r)
{
    char hdr[24];
    if (httpd_req_get_hdr_value_str(r, "X-SomnoPal-CSRF", hdr,
                                    sizeof(hdr)) != ESP_OK) {
        return false;
    }
    return strcmp(hdr, s_csrf) == 0;
}

static esp_err_t send_json(httpd_req_t *r, int code, const char *json)
{
    httpd_resp_set_status(r, code == 200 ? "200 OK" :
                          code == 400 ? "400 Bad Request" :
                          code == 403 ? "403 Forbidden" :
                          "500 Internal Error");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *r, char *buf, size_t cap)
{
    if (r->content_len <= 0 || r->content_len > cap - 1) {
        send_json(r, 400, "{\"ok\":false,\"error\":\"body_size\"}");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(r, buf, r->content_len);
    if (got <= 0) {
        return ESP_FAIL;
    }
    buf[got] = '\0';
    return ESP_OK;
}

/* ---------------- handlers ---------------- */

static const char INDEX_HTML[] =
    "<!doctype html><meta charset=utf-8><title>SomnoPal setup</title>"
    "<style>body{font-family:monospace;max-width:34em;margin:2em auto}"
    "label{display:block;margin:.6em 0}input{width:100%%}"
    "button{margin:.4em 0;padding:.5em 1em}</style>"
    "<h1>SomnoPal setup</h1>"
    "<form id=f><label>EZShare SSID<input name=ssid required></label>"
    "<label>Password<input name=pass type=password></label>"
    "<label><input name=open type=checkbox style=width:auto> "
    "open network (no password)</label>"
    "<label>AS10 timezone (POSIX)<input name=tz value=UTC0></label>"
    "<button>Save &amp; connect</button></form>"
    "<p id=m></p>"
    "<script>"
    "const C='%s';"
    "f.onsubmit=e=>{e.preventDefault();"
    "fetch('/api/config',{method:'POST',headers:{'Content-Type':"
    "'application/json','X-SomnoPal-CSRF':C},body:JSON.stringify({"
    "ezshare_ssid:f.ssid.value,ezshare_password:f.pass.value,"
    "allow_open_wifi:f.open.checked,as10_tz:f.tz.value,"
    "timezone_confirmed:true})}).then(r=>r.json()).then(j=>{"
    "m.textContent=j.ok?'Saved — device is connecting to the card.':'Error: '"
    "+(j.error||'unknown')})};"
    "</script>";

static esp_err_t on_index(httpd_req_t *r)
{
    touch();
    /* Issue session cookie + CSRF once per portal run. Captive browsers
     * re-fetch '/' after our 302s; rotating the token would strand the
     * already-rendered page's CSRF and fail its POST. */
    if (!s_sess[0]) {
        snprintf(s_sess, sizeof(s_sess), "%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random());
        snprintf(s_csrf, sizeof(s_csrf), "%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random());
    }

    char setc[96];
    snprintf(setc, sizeof(setc),
             "sp_sess=%s; HttpOnly; SameSite=Strict", s_sess);
    httpd_resp_set_hdr(r, "Set-Cookie", setc);
    httpd_resp_set_type(r, "text/html");

    char page[sizeof(INDEX_HTML) + 48];
    snprintf(page, sizeof(page), INDEX_HTML, s_csrf);
    return httpd_resp_send(r, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t on_get_config(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    sp_settings_t s;
    settings_load(&s);
    char json[384];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"ezshare_ssid\":\"%s\",\"password_set\":%s,"
             "\"allow_open_wifi\":%s,\"bssid_lock\":%s,"
             "\"as10_tz\":\"%s\",\"timezone_confirmed\":%s,"
             "\"lcd_brightness\":%u,\"security\":\"%s\"}",
             s.magic_valid ? s.ezshare_ssid : "",
             (s.magic_valid && s.ezshare_password[0]) ? "true" : "false",
             s.allow_open_wifi ? "true" : "false",
             s.bssid_lock ? "true" : "false",
             s.magic_valid ? s.as10_tz : "UTC0",
             s.timezone_confirmed ? "true" : "false",
             s.lcd_brightness, settings_security_label());
    return send_json(r, 200, json);
}

static esp_err_t on_post_config(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r) || !csrf_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    if (read_body(r, s_body, sizeof(s_body)) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(s_body);
    if (!j) {
        cJSON_Delete(j);
        return send_json(r, 400, "{\"ok\":false,\"error\":\"json\"}");
    }

    sp_settings_t s;
    settings_load(&s);          /* keep unchanged fields             */
    s.schema = SP_SETTINGS_SCHEMA;
    s.magic_valid = 1;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "ezshare_ssid")) &&
        cJSON_IsString(v)) {
        snprintf(s.ezshare_ssid, sizeof(s.ezshare_ssid), "%s",
                 v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(j, "ezshare_password")) &&
        cJSON_IsString(v)) {
        snprintf(s.ezshare_password, sizeof(s.ezshare_password), "%s",
                 v->valuestring);          /* omitted -> keep old     */
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "clear_password"))) {
        s.ezshare_password[0] = '\0';
    }
    if ((v = cJSON_GetObjectItem(j, "allow_open_wifi"))) {
        s.allow_open_wifi = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(j, "as10_tz")) && cJSON_IsString(v)) {
        snprintf(s.as10_tz, sizeof(s.as10_tz), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(j, "timezone_confirmed"))) {
        s.timezone_confirmed = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(j, "lcd_brightness")) &&
        cJSON_IsNumber(v)) {
        s.lcd_brightness = (uint8_t)v->valueint;
    }
    cJSON_Delete(j);

    esp_err_t err = settings_save(&s);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json(r, 500,
                         "{\"ok\":false,\"error\":\"SECURITY_NOT_"
                         "PROVISIONED\"}");
    }
    if (err != ESP_OK) {
        return send_json(r, 400, "{\"ok\":false,\"error\":\"invalid\"}");
    }
    /* Spec §4.1: reply "saved", then close AP and connect. The app loop
     * watches for the flag below. */
    extern volatile bool portal_saved_flag;
    portal_saved_flag = true;
    /* settings change may mean a different card — the persisted drift
     * SOT characterizes the old identity; fail closed (U6)           */
    drift_sot_invalidate();
    jrn_put("config", "\"e\":\"settings_saved\"");   /* evidence (§9) */
    return send_json(r, 200, "{\"ok\":true,\"saved\":true}");
}

static esp_err_t on_status(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    char json[384];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":\"SETUP\",\"security\":\"%s\","
             "\"uptime_s\":%lld}",
             settings_security_label(),
             (long long)(esp_timer_get_time() / 1000000));
    return send_json(r, 200, json);
}

static esp_err_t on_action(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r) || !csrf_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    if (read_body(r, s_body, sizeof(s_body)) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(s_body);
    const char *a = j && cJSON_IsString(cJSON_GetObjectItem(j, "action"))
        ? cJSON_GetObjectItem(j, "action")->valuestring : NULL;

    int act = PORTAL_ACTION_NONE;
    if (a && !strcmp(a, "FORGET_PEER")) {
        act = PORTAL_ACTION_FORGET_PEER;
    } else if (a && !strcmp(a, "RESET_SETTINGS")) {
        act = PORTAL_ACTION_RESET_SETTINGS;
    } else if (a && !strcmp(a, "CLEAR_EVIDENCE")) {
        act = PORTAL_ACTION_CLEAR_EVIDENCE;
    } else if (a && !strcmp(a, "REBUILD_CATALOG")) {
        act = PORTAL_ACTION_REBUILD_CATALOG;
    }
    cJSON_Delete(j);
    if (act == PORTAL_ACTION_NONE) {
        return send_json(r, 400, "{\"ok\":false,\"error\":\"action\"}");
    }
    s_pending_action = act;
    s_pending_since_us = esp_timer_get_time();
    return send_json(r, 200,
                     "{\"ok\":true,\"awaiting_physical_confirm\":true}");
}

/* ---------------- lifecycle ---------------- */

/* Captive-portal assist: any URI we don't serve (OS probe paths like
 * /generate_204, /hotspot-detect.html, /ncsi.txt) redirects to the portal. */
static esp_err_t on_404(httpd_req_t *r, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.8.1/");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

esp_err_t setup_portal_start(void)
{
    if (s_srv) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.stack_size = 6144;   /* handlers build ~1KB index page + cJSON trees */
    s_sess[0] = s_csrf[0] = '\0';

    ESP_RETURN_ON_ERROR(httpd_start(&s_srv, &cfg), TAG, "httpd start");

    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = on_index },
        { .uri = "/api/config",  .method = HTTP_GET,
          .handler = on_get_config },
        { .uri = "/api/config",  .method = HTTP_POST,
          .handler = on_post_config },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = on_status },
        { .uri = "/api/action",  .method = HTTP_POST, .handler = on_action },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_srv, &uris[i]);
    }
    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, on_404);
    touch();
    ESP_LOGI(TAG, "portal up at 192.168.8.1");
    return ESP_OK;
}

esp_err_t setup_portal_stop(void)
{
    if (s_srv) {
        httpd_stop(s_srv);
        s_srv = NULL;
    }
    s_pending_action = PORTAL_ACTION_NONE;
    s_sess[0] = s_csrf[0] = '\0';
    return ESP_OK;
}

/* 5-min idle / 15-min absolute setup timeout helpers (spec §4.1) */
static int64_t s_portal_started_us;
int64_t portal_idle_us(void)  { return esp_timer_get_time() - s_last_activity_us; }
int64_t portal_age_us(void)   { return esp_timer_get_time() - s_portal_started_us; }
void    portal_note_start(void) { s_portal_started_us = esp_timer_get_time(); }
