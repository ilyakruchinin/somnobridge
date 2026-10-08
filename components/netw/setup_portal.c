/*
 * SomnoBridge — setup portal (AP-mode HTTP server, spec §4.1)
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
    if (httpd_req_get_hdr_value_str(r, "X-SomnoBridge-CSRF", hdr,
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
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return ESP_OK;
}

/* ---------------- handlers ---------------- */

/* Served straight from flash: no %s substitution. The CSRF token is
 * fetched by the page from /api/config (cookie-protected), which keeps
 * the same CSRF guarantee without a RAM copy of the page. */
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SomnoBridge setup</title><style>"
":root{--ac:#4f7cff;--ok:#1a9e55;--err:#d33}"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
"background:#eef1f6;margin:0;min-height:100vh;display:flex;"
"align-items:flex-start;justify-content:center;padding:24px 12px}"
".card{background:#fff;border-radius:14px;"
"box-shadow:0 6px 24px rgba(30,40,70,.12);max-width:420px;width:100%;"
"padding:28px 26px}"
"h1{font-size:1.35em;margin:0 0 2px}"
".sub{color:#68738a;font-size:.85em;margin-bottom:18px}"
"label{display:block;font-weight:600;font-size:.85em;margin:14px 0 4px;color:#2a3145}"
"input[type=text],input[type=password],select{width:100%;padding:10px 12px;"
"border:1px solid #c9d1e0;border-radius:8px;font-size:1em;background:#fbfcfe}"
"input:focus,select:focus{outline:none;border-color:var(--ac);"
"box-shadow:0 0 0 3px rgba(79,124,255,.15)}"
"button{background:var(--ac);color:#fff;border:0;border-radius:8px;"
"padding:11px 16px;font-size:1em;font-weight:600;width:100%;margin-top:18px;cursor:pointer}"
"button:disabled{opacity:.5;cursor:default}"
"button.ghost{background:#e8edf7;color:#2a3145;font-weight:500;width:auto;"
"margin:0;padding:8px 14px;font-size:.85em}"
"#aps{margin-top:8px;border:1px solid #dfe5f0;border-radius:8px;"
"max-height:210px;overflow-y:auto}"
".ap{padding:9px 12px;display:flex;justify-content:space-between;"
"align-items:center;gap:8px;cursor:pointer;border-bottom:1px solid #eef1f6}"
".ap:hover{background:#f2f6ff}.ap.sel{background:#e7efff}"
".ap .n{font-weight:600;font-size:.95em;word-break:break-all}"
".ap .m{color:#68738a;font-size:.78em;white-space:nowrap}"
".badge{font-size:.7em;padding:2px 7px;border-radius:10px;font-weight:700}"
".b-sec{background:#e3f5ea;color:var(--ok)}"
".b-open{background:#fdf0e3;color:#b96a00}"
".chk{display:flex;align-items:center;gap:8px;font-weight:500;font-size:.9em;margin-top:12px}"
".chk input{width:auto}"
"#msg{margin-top:14px;font-size:.9em;min-height:1.2em}"
"#msg.ok{color:var(--ok)}#msg.err{color:var(--err)}"
".spin{display:inline-block;width:14px;height:14px;border:2px solid #c9d1e0;"
"border-top-color:var(--ac);border-radius:50%;animation:r .7s linear infinite;"
"vertical-align:-2px;margin-right:6px}"
"@keyframes r{to{transform:rotate(360deg)}}"
".hint{font-weight:400;color:#68738a;font-size:.78em;margin-top:3px}"
"input[type=range]{width:100%;accent-color:var(--ac)}"
"</style></head><body><div class=card>"
"<h1>SomnoBridge</h1>"
"<div class=sub>Setup &mdash; connect to your EZShare card</div>"
"<label>WiFi network (EZShare card)</label>"
"<button class=ghost id=scanbtn type=button onclick=doScan()>Scan networks</button>"
"<div id=aps></div>"
"<label for=ssid>Network name (SSID)</label>"
"<input id=ssid type=text maxlength=32 autocomplete=off "
"placeholder='Select a scanned network or type the name'>"
"<label for=pass>Password</label>"
"<input id=pass type=password maxlength=64 autocomplete=new-password>"
"<label class=chk><input id=open type=checkbox> Open network (no password)</label>"
"<label for=tz>Machine time zone (GMT offset)</label>"
"<select id=tz></select>"
"<div class=hint>Whole hours only, matching the machine: AirSense 11 offers "
"GMT&minus;12&hellip;GMT+14 in its settings menu. AirSense 10 has no "
"timezone setting &mdash; it is the local time you set on the machine; "
"update it after daylight-saving changes.</div>"
"<label for=bright>LCD brightness <span id=bv></span></label>"
"<input id=bright type=range min=1 max=100>"
"<button id=save type=button onclick=save()>Save &amp; connect</button>"
"<div id=msg></div>"
"</div><script>"
"const $=i=>document.getElementById(i);let CSRF='';"
/* AS11 menu: GMT-12..GMT+14, 1-hour steps only; AS10 has no TZ setting
 * (manual clock). Half-hour zones are reachable via the custom field. */
"const ZONES=[[-12,0,'Baker Island'],[-11,0,'American Samoa'],"
"[-10,0,'Hawaii'],[-9,0,'Alaska'],[-8,0,'Los Angeles, Vancouver'],"
"[-7,0,'Denver, Calgary'],[-6,0,'Chicago, Mexico City'],"
"[-5,0,'New York, Toronto'],[-4,0,'Santiago, Halifax'],"
"[-3,0,'Sao Paulo, Buenos Aires'],[-2,0,'South Georgia'],"
"[-1,0,'Azores, Cape Verde'],[0,0,'UTC'],"
"[1,0,'Berlin, Paris, Madrid'],[2,0,'Athens, Cairo, Johannesburg'],"
"[3,0,'Moscow, Istanbul, Riyadh'],[4,0,'Dubai, Baku'],"
"[5,0,'Karachi, Tashkent'],[6,0,'Dhaka, Almaty'],"
"[7,0,'Bangkok, Jakarta, Hanoi'],[8,0,'Beijing, Singapore, Perth'],"
"[9,0,'Tokyo, Seoul'],[10,0,'Sydney, Melbourne'],"
"[11,0,'Solomon Islands'],[12,0,'Auckland, Wellington'],[13,0,'Tonga'],"
"[14,0,'Kiritimati']];"
/* POSIX TZ sign is inverted (west-positive): UTC+2 -> "UTC-2" */
"function posix(off){if(!off)return 'UTC0';const s=off>0?'-':'+';"
"const a=Math.abs(off),h=Math.floor(a/60),m=a%60;"
"return 'UTC'+s+h+(m?':'+m:'');}"
/* parse a stored fixed-offset POSIX rule -> minutes EAST of UTC */
"function parseTz(v){if(v==='UTC0'||v==='GMT0')return 0;"
"const m=/^[A-Za-z]{3,}([+-])(\\d{1,2})(?::(\\d{1,2}))?$/.exec(v||'');"
"if(!m)return null;return (m[1]==='+'?-1:1)*((+m[2])*60+(+m[3]||0));}"
"function fillTz(cur){const t=$('tz');const coff=parseTz(cur);let match=false;"
"ZONES.forEach(z=>{const off=z[0]*60+z[1],v=posix(off);"
"const o=document.createElement('option');o.value=v;"
/* label matches the AS11 menu: GMT-12 .. GMT+/-0 .. GMT+14 */
"const l='GMT'+(off===0?'+/-0':(off>0?'+':'-')+Math.abs(off/60))+"
"' \\u2014 '+z[2];"
"o.textContent=l;if(coff!==null&&off===coff){o.selected=true;match=true;}"
"t.appendChild(o);});"
/* stored rule outside the fixed list (e.g. a legacy value): show it as-is */
"if(!match&&cur){const o=document.createElement('option');o.value=cur;"
"o.textContent='Stored: '+cur;o.selected=true;t.insertBefore(o,t.firstChild);}}"
"function pwState(){$('pass').disabled=$('open').checked;}"
"function doScan(){const b=$('scanbtn');b.disabled=true;"
"b.innerHTML='<span class=spin></span>Scanning\\u2026';"
"fetch('/api/scan').then(r=>r.json()).then(j=>{"
"const box=$('aps');box.innerHTML='';const L=j.aps||[];"
"if(!L.length){box.innerHTML='<div class=ap><span class=n style=color:#68738a>"
"No networks found \\u2014 try again or type the name</span></div>';}"
"L.forEach(a=>{const d=document.createElement('div');d.className='ap';"
"const n=document.createElement('span');n.className='n';n.textContent=a.ssid;"
"const m=document.createElement('span');m.className='m';"
"const bd=document.createElement('span');"
"bd.className='badge '+(a.open?'b-open':'b-sec');"
"bd.textContent=a.open?'OPEN':'SECURED';"
"m.appendChild(bd);m.appendChild(document.createTextNode(' '+a.rssi+' dBm'));"
"d.appendChild(n);d.appendChild(m);"
"d.onclick=()=>{document.querySelectorAll('.ap').forEach(x=>x.classList.remove('sel'));"
"d.classList.add('sel');$('ssid').value=a.ssid;$('open').checked=!!a.open;pwState();};"
"box.appendChild(d);});})"
".catch(()=>{$('msg').textContent='Scan failed';$('msg').className='err';})"
".finally(()=>{b.disabled=false;b.textContent='Scan networks';});}"
"function save(){"
"const b={ezshare_ssid:$('ssid').value.trim(),allow_open_wifi:$('open').checked,"
"as10_tz:$('tz').value,"
"timezone_confirmed:true,lcd_brightness:+$('bright').value};"
"if($('pass').value)b.ezshare_password=$('pass').value;"
"if($('open').checked)b.clear_password=true;"
"$('save').disabled=true;$('msg').textContent='Saving\\u2026';$('msg').className='';"
"fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json','X-SomnoBridge-CSRF':CSRF},"
"body:JSON.stringify(b)}).then(r=>r.json()).then(j=>{"
"if(j.ok){$('msg').className='ok';"
"$('msg').textContent='Saved \\u2014 device is connecting to the card. "
"The setup network will close.';}"
"else{$('msg').className='err';$('msg').textContent='Error: '+(j.error||'unknown');"
"$('save').disabled=false;}})"
".catch(()=>{$('msg').className='err';$('msg').textContent='Save failed';"
"$('save').disabled=false;});}"
"fetch('/api/config').then(r=>r.json()).then(j=>{CSRF=j.csrf||'';"
"$('ssid').value=j.ezshare_ssid||'';"
"if(j.password_set)$('pass').placeholder='\\u2022\\u2022\\u2022\\u2022\\u2022\\u2022\\u2022\\u2022"
"  (saved \\u2014 leave blank to keep)';"
"$('open').checked=!!j.allow_open_wifi;"
"$('bright').value=j.lcd_brightness||30;"
"$('bv').textContent='('+$('bright').value+'%)';"
"fillTz(j.as10_tz||'UTC0');pwState();doScan();});"
"$('bright').addEventListener('input',()=>$('bv').textContent='('+$('bright').value+'%)');"
"$('open').addEventListener('change',pwState);"
"</script></body></html>";

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
    return httpd_resp_send(r, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static size_t json_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

static esp_err_t on_scan(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    static netw_ap_t aps[20];       /* httpd handlers run serially */
    static char json[2048];
    int n = netw_wifi_scan(aps, 20);
    if (n < 0) {
        return send_json(r, 500, "{\"ok\":false,\"error\":\"scan\"}");
    }
    size_t o = (size_t)snprintf(json, sizeof(json), "{\"ok\":true,\"aps\":[");
    for (int i = 0; i < n && o < sizeof(json) - 96; i++) {
        char esc[100];
        json_esc(esc, sizeof(esc), aps[i].ssid);
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                              i ? "," : "", esc, aps[i].rssi,
                              aps[i].open ? "true" : "false");
    }
    snprintf(json + o, sizeof(json) - o, "]}");
    return send_json(r, 200, json);
}

static esp_err_t on_get_config(httpd_req_t *r)
{
    touch();
    if (!cookie_ok(r)) {
        return send_json(r, 403, "{\"ok\":false,\"error\":\"auth\"}");
    }
    sp_settings_t s;
    settings_load(&s);
    char json[448];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"csrf\":\"%s\",\"ezshare_ssid\":\"%s\","
             "\"password_set\":%s,"
             "\"allow_open_wifi\":%s,\"bssid_lock\":%s,"
             "\"as10_tz\":\"%s\",\"timezone_confirmed\":%s,"
             "\"lcd_brightness\":%u,\"security\":\"%s\"}",
             s_csrf,
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
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = on_scan },
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
