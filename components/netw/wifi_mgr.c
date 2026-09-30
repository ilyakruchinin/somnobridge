/*
 * SomnoPal — WiFi manager (spec §4.3)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <string.h>

#include "netw.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "netw";

static netw_status_t  s_st;
static netw_link_cb_t s_link_cb;
static esp_netif_t   *s_sta_netif;
static esp_netif_t   *s_ap_netif;
static char           s_ap_ssid[33];
static char           s_ap_pass[16];
static bool           s_wifi_started;
static TaskHandle_t   s_reconn_task;

/* Reconnect backoff ladder (spec §4.3): 1,2,4,8,16,30,60 s, ±10% jitter */
static const uint32_t s_backoff_ms[] = {
    1000, 2000, 4000, 8000, 16000, 30000, 60000
};
#define BACKOFF_STEPS (sizeof(s_backoff_ms)/sizeof(s_backoff_ms[0]))

static uint32_t jittered(uint32_t base_ms)
{
    uint32_t j = base_ms / 10;
    return base_ms - j + (esp_random() % (2 * j + 1));
}

static void set_mode(netw_mode_t m)
{
    s_st.mode = m;
    s_st.sta_connected = false;
    s_st.ezshare_reachable = false;
    s_st.rssi = 0;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id,
                    void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_st.sta_connected = false;
        s_st.ezshare_reachable = false;
        if (s_link_cb) {
            s_link_cb(false, 0);
        }
    } else if (id == WIFI_EVENT_STA_START && s_st.mode == NETW_MODE_STA) {
        esp_wifi_connect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_st.sta_connected = true;
        s_st.sta_ip = ntohl(ev->ip_info.ip.addr);
        /* EZShare host = configured override or the DHCP gateway (§4.3) */
        if (s_st.card_ip == 0) {
            s_st.card_ip = ntohl(ev->ip_info.gw.addr);
        }
        ESP_LOGI(TAG, "STA got ip %lu.%lu.%lu.%lu card=%lu.%lu.%lu.%lu",
                 (s_st.sta_ip >> 24) & 255, (s_st.sta_ip >> 16) & 255,
                 (s_st.sta_ip >> 8) & 255, s_st.sta_ip & 255,
                 (s_st.card_ip >> 24) & 255, (s_st.card_ip >> 16) & 255,
                 (s_st.card_ip >> 8) & 255, s_st.card_ip & 255);
        if (s_link_cb) {
            s_link_cb(true, s_st.sta_ip);
        }
    }
}

static void reconn_task(void *arg)
{
    int step = 0;
    for (;;) {
        if (s_st.mode == NETW_MODE_STA && !s_st.sta_connected) {
            esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(10000));
            if (!s_st.sta_connected) {
                uint32_t wait = jittered(s_backoff_ms[step]);
                step = step + 1 < (int)BACKOFF_STEPS
                           ? step + 1 : (int)BACKOFF_STEPS - 1;
                vTaskDelay(pdMS_TO_TICKS(wait));
            } else {
                step = 0;
            }
        } else {
            step = 0;
            /* reset ladder after 60 s stable association */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t netw_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    return ESP_OK;
}

static esp_err_t ensure_wifi_started(void)
{
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_started = true;
    }
    return ESP_OK;
}

static void wifi_stop(void)
{
    esp_wifi_stop();
    s_wifi_started = false;   /* keep flag in sync with driver state */
}

esp_err_t netw_enter_sta(const sp_settings_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_stop();

    wifi_config_t w = {0};
    memcpy(w.sta.ssid, cfg->ezshare_ssid,
           strlen(cfg->ezshare_ssid) > 32 ? 32 : strlen(cfg->ezshare_ssid));
    size_t plen = strlen(cfg->ezshare_password);
    memcpy(w.sta.password, cfg->ezshare_password, plen > 64 ? 64 : plen);
    w.sta.threshold.authmode =
        plen == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (cfg->bssid_lock) {
        memcpy(w.sta.bssid, cfg->ezshare_bssid, 6);
        w.sta.bssid_set = 1;
    }
    w.sta.pmf_cfg.capable = false;
    w.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &w), TAG, "cfg");
    ESP_RETURN_ON_ERROR(ensure_wifi_started(), TAG, "start");

    s_st.card_ip = cfg->http_host_ipv4;   /* 0 -> resolve from gateway  */
    set_mode(NETW_MODE_STA);
    esp_wifi_connect();

    if (!s_reconn_task) {
        xTaskCreate(reconn_task, "netw_reconn", 3072, NULL, 5,
                    &s_reconn_task);
    }
    return ESP_OK;
}

/* ---- setup AP ---- */

static void gen_ap_credentials(void)
{
    uint32_t r = esp_random();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SomnoPal-Setup-%04lX",
             (unsigned long)(r & 0xFFFF));
    s_ap_pass[0] = '\0';   /* open AP — short-lived setup window only */
}

void netw_ap_credentials(char ssid[33], char pass[16])
{
    if (ssid) {
        memcpy(ssid, s_ap_ssid, 33);
    }
    if (pass) {
        memcpy(pass, s_ap_pass, 16);
    }
}

esp_err_t setup_portal_start(void);
esp_err_t setup_portal_stop(void);
void      portal_note_start(void);
void      dns_server_start(void);
void      dns_server_stop(void);

esp_err_t netw_enter_ap(void)
{
    wifi_stop();
    gen_ap_credentials();

    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 8, 1);
    IP4_ADDR(&ip.gw, 192, 168, 8, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t w = {0};
    memcpy(w.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    w.ap.ssid_len = strlen(s_ap_ssid);
    if (s_ap_pass[0]) {
        memcpy(w.ap.password, s_ap_pass, strlen(s_ap_pass));
        w.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        w.ap.authmode = WIFI_AUTH_OPEN;
    }
    w.ap.max_connection = 1;
    w.ap.channel = 6;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &w), TAG, "ap cfg");
    ESP_RETURN_ON_ERROR(ensure_wifi_started(), TAG, "ap start");
    set_mode(NETW_MODE_AP);
    ESP_LOGI(TAG, "setup AP %s", s_ap_ssid);

    portal_note_start();             /* absolute 15-min window (§4.1) */
    dns_server_start();              /* captive-portal redirection    */
    return setup_portal_start();
}

esp_err_t netw_leave_ap(void)
{
    if (s_st.mode != NETW_MODE_AP) {
        return ESP_ERR_INVALID_STATE;
    }
    dns_server_stop();
    setup_portal_stop();
    wifi_stop();                     /* actually drop the AP radio    */
    set_mode(NETW_MODE_DOWN);
    return ESP_OK;   /* caller decides whether to re-enter STA */
}

netw_mode_t netw_mode(void) { return s_st.mode; }
const netw_status_t *netw_status(void) { return &s_st; }

void netw_set_link_cb(netw_link_cb_t cb) { s_link_cb = cb; }

void netw_refresh_rssi(void)
{
    if (s_st.mode == NETW_MODE_STA && s_st.sta_connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            s_st.rssi = rec.rssi;
        }
    }
}
