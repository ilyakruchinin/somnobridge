/*
 * SomnoBridge — WiFi manager + setup portal (spec §4.1/§4.3)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETW_MODE_DOWN = 0,   /* nothing started yet                  */
    NETW_MODE_STA,        /* connected/connecting to EZShare      */
    NETW_MODE_AP,         /* setup SoftAP (portal)                */
    NETW_MODE_FAULT,      /* unrecoverable config/security fault  */
} netw_mode_t;

typedef struct {
    netw_mode_t mode;
    bool        sta_connected;
    bool        ezshare_reachable;   /* HTTP probe succeeded        */
    int8_t      rssi;
    uint32_t    sta_ip;              /* host order                  */
    uint32_t    card_ip;             /* resolved EZShare IPv4       */
} netw_status_t;

/** Init WiFi driver + netif. Then call netw_enter_sta or netw_enter_ap. */
esp_err_t netw_init(void);

/**
 * Start STA mode with the given settings. Reconnects internally with the
 * §4.3 backoff; resolve EZShare to settings.http_host_ipv4 or the DHCP
 * gateway. Blocks until associated+IP or first failure window.
 */
esp_err_t netw_enter_sta(const sp_settings_t *cfg);

/** Stop STA and bring up the setup AP + portal. Returns immediately. */
esp_err_t netw_enter_ap(void);

/** Stop AP/portal and (if valid config exists) return to STA. */
esp_err_t netw_leave_ap(void);

netw_mode_t    netw_mode(void);
const netw_status_t *netw_status(void);
void           netw_refresh_rssi(void);  /* STA RSSI poll (~1 Hz)    */

/* Setup AP credentials — generated per session (spec §4.1). */
void netw_ap_credentials(char ssid[33], char pass[16]);

/* Scan result entry (setup AP mode only). */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;      /* no auth required */
} netw_ap_t;

/**
 * Blocking scan of surrounding APs while the setup AP is up (APSTA).
 * Deduplicates by SSID (strongest RSSI kept), skips hidden networks and
 * the setup AP itself, strongest first. Returns count, or -1 on error.
 */
int netw_wifi_scan(netw_ap_t *out, int cap);

/* Portal destructive-action queue + timeouts (spec §4.1):
 * actions arrive via POST /api/action and execute only after a
 * physical button confirm — the app loop owns that pairing.        */
typedef enum {
    PORTAL_ACTION_NONE = 0,
    PORTAL_ACTION_FORGET_PEER,
    PORTAL_ACTION_RESET_SETTINGS,
    PORTAL_ACTION_CLEAR_EVIDENCE,
    PORTAL_ACTION_REBUILD_CATALOG,
} portal_action_t;

int     portal_pending_action(void);   /* PORTAL_ACTION_* or NONE    */
int64_t portal_pending_since_us(void); /* when the action was queued */
void    portal_clear_pending(void);
int64_t portal_idle_us(void);          /* since last portal request  */
int64_t portal_age_us(void);           /* since AP entered           */

/** Register a callback invoked when STA got/lost IP. Called on event task. */
typedef void (*netw_link_cb_t)(bool up, uint32_t ip);
void netw_set_link_cb(netw_link_cb_t cb);

#ifdef __cplusplus
}
#endif
