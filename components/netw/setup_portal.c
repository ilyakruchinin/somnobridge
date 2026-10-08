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
".machines{display:flex;gap:10px;margin-top:4px}"
".mach{position:relative;flex:1;border:1.5px solid #c9d1e0;border-radius:10px;"
"padding:10px 6px 8px;text-align:center;cursor:pointer;background:#fbfcfe;"
"opacity:.55;filter:grayscale(.55);transition:opacity .15s,filter .15s}"
".mach.sel{border-color:var(--ok);background:#eaf7ef;opacity:1;filter:none;"
"box-shadow:0 0 0 3px rgba(26,158,85,.18)}"
".mach::after{content:'\\2713';position:absolute;top:6px;right:8px;width:20px;"
"height:20px;line-height:20px;border-radius:50%;background:var(--ok);"
"color:#fff;font-weight:700;font-size:.85em;opacity:0;transform:scale(.5);"
"transition:.15s}"
".mach.sel::after{opacity:1;transform:scale(1)}"
".mach img{width:100%;height:64px;object-fit:contain;"
"display:block;margin:0 auto 4px}"
".mach .t{font-weight:600;font-size:.9em}"
".mach .d{color:#68738a;font-size:.72em;margin-top:3px;line-height:1.35}"
".warn{display:flex;gap:6px;align-items:center;background:#fdf0e3;"
"color:#b96a00;font-size:.78em;font-weight:600;border-radius:8px;"
"padding:7px 10px;margin-top:6px}"
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
"<label>Your CPAP machine</label>"
"<div class=warn>&#9888; Ensure you choose the model you actually have "
"&mdash; it changes how file times are read.</div>"
"<div class=machines>"
"<div class=mach id=m10>"
"<img src='data:image/jpeg;base64,"
"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'"
" alt='AirSense 10'>"
"<div class=t>ResMed 10-series</div>"
"<div class=d>AirSense 10 &mdash; clock set by hand, no timezone menu</div>"
"</div>"
"<div class=mach id=m11>"
"<img src='data:image/jpeg;base64,"
"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"
"ECZoSoQg/9k='"
" alt='AirSense 11'>"
"<div class=t>ResMed 11-series</div>"
"<div class=d>AirSense 11 &mdash; GMT offset in its settings menu</div>"
"</div>"
"</div>"
"<div id=tzblock hidden>"
"<label for=tz>Machine time zone (GMT offset)</label>"
"<select id=tz></select>"
"<div class=hint>Whole hours only &mdash; match the GMT offset chosen in the "
"machine's settings menu (GMT&minus;12&hellip;GMT+14).</div>"
"</div>"
"<label for=bright>LCD brightness <span id=bv></span></label>"
"<input id=bright type=range min=1 max=100>"
"<button id=save type=button onclick=save()>Save &amp; connect</button>"
"<div id=msg></div>"
"</div><script>"
"const $=i=>document.getElementById(i);let CSRF='';"
/* AS11 menu: GMT-12..GMT+14, 1-hour steps only. AS10 has no TZ setting
 * (manual clock) and is stored as UTC0 — zero offset, no shift. */
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
"let MACH='as10';"
"function pick(m){MACH=m;"
"$('m10').classList.toggle('sel',m==='as10');"
"$('m11').classList.toggle('sel',m==='as11');"
"$('tzblock').hidden=m!=='as11';}"
"$('m10').onclick=()=>pick('as10');"
"$('m11').onclick=()=>pick('as11');"
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
"as10_tz:MACH==='as11'?$('tz').value:'UTC0',"
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
"const cur=j.as10_tz||'UTC0';fillTz(cur);"
"pick(parseTz(cur)===0?'as10':'as11');pwState();doScan();});"
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
