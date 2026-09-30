/*
 * SomnoPal — application entry point
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Thin orchestrator (spec §13.1): board -> settings/security -> journal
 * -> catalog -> WiFi -> card scheduler -> BLE. Button drives setup/pair.
 */
#include <stdio.h>

#include "nvs_flash.h"
#include "board.h"
#include "catalog.h"
#include "drift.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "journal.h"
#include "netw.h"
#include "sched.h"
#include "settings.h"
#include "ble_svc.h"

static const char *TAG = "somnopal";

/* set by the setup portal when new config is committed (setup_portal.c) */
volatile bool portal_saved_flag;

static void on_wifi_link(bool up, uint32_t ip)
{
    (void)ip;                        /* cb passes our STA addr        */
    sched_set_link(up);
    jrn_put("event", up ? "\"e\":\"link_up\"" : "\"e\":\"link_down\"");
    if (up) {
        sched_set_card_ip(netw_status()->card_ip); /* EZShare host   */
    }
}

/* portal destructive actions — each executes only after the physical
 * confirm press while the setup AP is up (§4.1)                      */
static void run_portal_action(int act)
{
    switch (act) {
    case PORTAL_ACTION_FORGET_PEER:
        ble_svc_forget_peer();          /* app identity + bond store  */
        lcd_ui_row(11, "PEER FORGOTTEN", 1);
        jrn_put("event", "\"e\":\"forget_peer\"");
        break;
    case PORTAL_ACTION_RESET_SETTINGS:
        settings_clear();               /* EZShare cfg, keeps peer    */
        lcd_ui_row(11, "SETTINGS RESET", 1);
        jrn_put("event", "\"e\":\"reset_settings\"");
        break;
    case PORTAL_ACTION_CLEAR_EVIDENCE:
        jrn_erase_all();
        lcd_ui_row(11, "EVIDENCE CLEARED", 1);
        break;
    case PORTAL_ACTION_REBUILD_CATALOG:
        cat_init();                     /* volatile: drops all state  */
        lcd_ui_row(11, "CATALOG CLEARED", 1);
        jrn_put("event", "\"e\":\"rebuild_catalog\"");
        break;
    default:
        break;
    }
}

static void card_task(void *arg)
{
    sched_run_forever();
}

static void show_setup_ap(void)
{
    char ssid[33], pass[16];
    lcd_ui_row(11, "SETUP MODE", 1);
    netw_ap_credentials(ssid, pass);
    lcd_ui_rowf(12, 1, "AP:%s", ssid);
    if (pass[0]) {
        lcd_ui_rowf(13, 1, "PW:%s", pass);
    } else {
        lcd_ui_row(13, "OPEN NETWORK", 1);
    }
}

static void ui_tick(void)
{
    const therapy_status_t *t = therapy_status();
    const netw_status_t *n = netw_status();
    lcd_ui_rowf(3, 0, "WIFI:%s RSSI:%d",
                n->sta_connected ? "UP " : "DOWN", n->rssi);
    lcd_ui_rowf(4, 0, "BLE:%s%s",
                ble_svc_connected() ? "CONN" : "---",
                ble_svc_authorized() ? " AUTH" : "");
    lcd_ui_rowf(5, 0, "THERAPY:%s%s",
                t->state == THERAPY_IN ? "IN " :
                t->state == THERAPY_OUT ? "OUT" : "???",
                t->therapy_stale ? " STALE" : "");
    lcd_ui_rowf(6, 0, "ANCHORS:%d CAT:%d/%s",
                drift_anchor_count(), cat_folder_count(),
                cat_catalog_complete() ? "FULL" : "PART");
    lcd_ui_rowf(7, 0, "LOAD8H:%lu BKOFF:%lums",
                (unsigned long)sched_active_8h(),
                (unsigned long)sched_backoff_ms());
}

/* boot-hang bisection: LCD row survives WDT resets (panel RAM) */
void boot_mark(uint8_t n) { lcd_ui_rowf(14, 0, "MARK %02x", n); }
#define MARK(n) boot_mark(n)

void app_main(void)
{
    ESP_LOGI(TAG, "SomnoPal boot");

    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board_init failed: %s", esp_err_to_name(err));
    }
    lcd_ui_clear();
    lcd_ui_headline("SOMNOPAL");
    lcd_ui_row(1, "BOOT OK", 0);

    /* 1. settings/security first — NVS must be up before the journal
     *    or the NimBLE bond store touch flash.                      */
    err = settings_init();
    sp_settings_t cfg;
    bool provisioned = err == ESP_OK && settings_load(&cfg) == ESP_OK &&
                       cfg.magic_valid;
    lcd_ui_rowf(2, 0, "CFG:%s SEC:%s",
                provisioned ? "OK" : "NONE", settings_security_label());
    MARK(0xa1);

    /* 2. evidence journal (may run degraded without security cfg)     */
    err = jrn_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "journal degraded: %s", esp_err_to_name(err));
    }
    MARK(1);
    jrn_put("event", "\"e\":\"boot\"");

    /* 3. volatile catalog + therapy FSM + scheduler                  */
    MARK(2);
    cat_init();
    MARK(3);
    err = sched_init();
    MARK(4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sched_init failed");
        lcd_ui_row(9, "SCHED FAIL", 1);
    }
    drift_reset();
    MARK(5);

    /* 4. WiFi — STA with stored config, or fall back to setup AP     */
    err = netw_init();
    MARK(6);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netw_init failed");
        lcd_ui_row(9, "WIFI FAIL", 1);
    }
    netw_set_link_cb(on_wifi_link);
    if (provisioned) {
        netw_enter_sta(&cfg);
        MARK(7);
    } else {
        /* unconfigured: straight to setup AP so the user can pair    */
        netw_enter_ap();
        show_setup_ap();
        MARK(8);
    }

    /* 5. BLE stack — advertises; pairing window opens on button      */
    MARK(9);
    err = ble_svc_init();
    if (err == ESP_OK) {
        ble_svc_advertise();
        if (!ble_svc_peer_bonded()) {
            lcd_ui_row(10, "PRESS BTN TO PAIR", 0);
        }
    } else {
        ESP_LOGE(TAG, "ble_svc_init failed");
        lcd_ui_row(10, "BLE FAIL", 1);
    }
    MARK(10);

    /* 6. card worker task — the only HTTP owner (spec §13.1)         */
    xTaskCreate(card_task, "card", 8192, NULL, 5, NULL);
    MARK(11);

    esp_task_wdt_add(NULL);

    /* 7. UI / input loop                                             */
    int64_t last = 0;
    for (;;) {
        board_btn_event_t ev = board_button_poll();
        if (ev == BOARD_BTN_LONG) {
            /* enter setup AP (§4.1) — stays until portal timeout or
             * second long press exits                               */
            if (netw_mode() == NETW_MODE_AP) {
                lcd_ui_row(11, "LEAVING SETUP", 1);
                netw_leave_ap();
            } else {
                netw_enter_ap();
                show_setup_ap();
            }
        } else if (ev == BOARD_BTN_SHORT) {
            /* in AP mode a press confirms the pending portal action
             * (§4.1 destructive-action physical gate); otherwise it
             * opens the BLE pairing window                          */
            int pend = portal_pending_action();
            int64_t ago = pend ? esp_timer_get_time()
                                 - portal_pending_since_us() : 0;
            if (pend && ago > 30LL * 1000 * 1000) {
                portal_clear_pending();      /* 30-s confirm window   */
                pend = PORTAL_ACTION_NONE;
            }
            if (netw_mode() == NETW_MODE_AP && pend != PORTAL_ACTION_NONE) {
                portal_clear_pending();
                run_portal_action(pend);
            } else if (!ble_svc_peer_bonded()) {
                if (ble_svc_open_pairing() == ESP_OK) {
                    lcd_ui_row(11, "PAIR 120S — PIN ->", 1);
                }
            }
        }

        /* setup AP timeouts: 5 min idle / 15 min absolute (§4.1)      */
        if (netw_mode() == NETW_MODE_AP &&
            (portal_idle_us() > 5LL * 60 * 1000000 ||
             portal_age_us() > 15LL * 60 * 1000000)) {
            lcd_ui_row(11, "SETUP TIMEOUT", 1);
            netw_leave_ap();
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                netw_enter_sta(&cfg);
            }
        }

        /* portal committed new config -> leave AP, retry STA         */
        if (portal_saved_flag && netw_mode() == NETW_MODE_AP) {
            portal_saved_flag = false;
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                lcd_ui_row(11, "SAVED — JOINING", 1);
                netw_leave_ap();
                netw_enter_sta(&cfg);
                provisioned = true;
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last > 1000 * 1000) {
            last = now;
            jrn_tick(now);         /* 60-s evidence batch flush        */
            ble_svc_pairing_tick(now);   /* 120-s window / 60-s try   */
            netw_refresh_rssi();
            if (netw_mode() != NETW_MODE_AP &&
                !ble_svc_pairing_open() &&          /* keep PIN shown  */
                !(ble_svc_connected() && !ble_svc_authorized())) {
                ui_tick();
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
