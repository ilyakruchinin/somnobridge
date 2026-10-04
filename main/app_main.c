/*
 * SomnoBridge — application entry point
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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "journal.h"
#include "netw.h"
#include "sched.h"
#include "settings.h"
#include "ble_svc.h"

static const char *TAG = "somnobridge";

/* DIAG: temporary heap-corruption bisection — each boot stage checks
 * integrity; the walker crashes on the first corrupt structure, so the
 * last PASS before a crash brackets the corrupting stage.           */
static void heap_ck(const char *tag)
{
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    bool ok = heap_caps_check_integrity_all(false);
    ESP_LOGI(TAG, "heap_ck %s: %s", tag, ok ? "OK" : "CORRUPT");
#else
    (void)tag;
#endif
}

/* set by the setup portal when new config is committed (setup_portal.c) */
volatile bool portal_saved_flag;

/* while the BLE PIN screen is up the status tick must not redraw rows —
 * set on ble_svc_pin_visible(), cleared (with a chrome repaint) once the
 * pairing attempt ends                                            */
static bool s_pin_up;

static void on_wifi_link(bool up, uint32_t ip)
{
    (void)ip;                        /* cb passes our STA addr        */
    sched_set_link(up);
    jrn_put("event", up ? "\"e\":\"link_up\"" : "\"e\":\"link_down\"");
    if (up) {
        sched_set_card_ip(netw_status()->card_ip); /* EZShare host   */
        heap_ck("link_up");
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
        drift_sot_invalidate();         /* card may change (U6)       */
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
    lcd_ui_rowf(3, 0, "WIFI:%s %ddBm",
                n->sta_connected ? "UP" : "DOWN", n->rssi);
    lcd_ui_rowf(4, 0, "BLE:%s%s",
                ble_svc_connected() ? "CONN" : "---",
                ble_svc_authorized() ? " AUTH" : "");
    lcd_ui_rowf(5, 0, "THER:%s%s",
                t->state == THERAPY_IN ? "IN " :
                t->state == THERAPY_OUT ? "OUT" : "???",
                t->therapy_stale ? " STALE" : "");
    lcd_ui_rowf(6, 0, "CAT:%d %s A:%d",
                cat_folder_count(),
                cat_catalog_complete() ? "FULL" : "PART",
                drift_anchor_count());
    lcd_ui_rowf(7, 0, "L8H:%lu BK:%lus",
                (unsigned long)sched_active_8h(),
                (unsigned long)(sched_backoff_ms() / 1000));

    /* D: live estimate; S: persisted source of truth (U10).
     * Nulls render as "—" — never a fabricated value.             */
    drift_record_t dr;
    if (drift_pub_get(&dr) && dr.has_drift) {
        lcd_ui_rowf(8, 0, "D:%c%lldms +/-%lld %s",
                    dr.clock_drift_ms >= 0 ? '+' : '-',
                    (long long)(dr.clock_drift_ms < 0
                        ? -dr.clock_drift_ms : dr.clock_drift_ms),
                    (long long)dr.accuracy_half_ms,
                    dr.tier == ACC_ACCURATE ? "ACC" :
                    dr.tier == ACC_APPROXIMATE ? "APX" : "---");
    } else if (drift_pub_get(NULL)) {
        lcd_ui_row(8, "D: pending", 0);
    } else {
        lcd_ui_row(8, "D: ---", 0);
    }
    drift_sot_t sot;
    if (drift_sot_view(&sot)) {
        lcd_ui_rowf(14, 0, "S:%c%lldms +/-%lld %s",
                    sot.clock_drift_ms >= 0 ? '+' : '-',
                    (long long)(sot.clock_drift_ms < 0
                        ? -sot.clock_drift_ms : sot.clock_drift_ms),
                    (long long)sot.accuracy_half_ms,
                    sot.origin == DRIFT_SOT_NVS ? "NVS" : "RAM");
    } else {
        lcd_ui_row(14, "S: ---", 0);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "SomnoBridge boot (reset reason %d)",
             (int)esp_reset_reason());

    /* DIAG: quiet chatty drivers so NimBLE SM trace survives the UART */
    esp_log_level_set("spi_master", ESP_LOG_WARN);
    esp_log_level_set("nvs", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("event", ESP_LOG_WARN);

    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board_init failed: %s", esp_err_to_name(err));
    }
    lcd_ui_clear();
    lcd_ui_headline("SOMNOBRIDGE");
    lcd_ui_row(1, "BOOT OK", 0);
    heap_ck("board");

    /* 1. settings/security first — NVS must be up before the journal
     *    or the NimBLE bond store touch flash.                      */
    err = settings_init();
    sp_settings_t cfg;
    bool provisioned = err == ESP_OK && settings_load(&cfg) == ESP_OK &&
                       cfg.magic_valid;
    drift_timezone_set(cfg.as10_tz, cfg.timezone_confirmed);
    lcd_ui_rowf(2, 0, "CFG:%s SEC:%.3s",
                provisioned ? "OK" : "NONE", settings_security_label());
    heap_ck("settings");

    /* 2. evidence journal (may run degraded without security cfg)     */
    err = jrn_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "journal degraded: %s", esp_err_to_name(err));
    }
    jrn_put("event", "\"e\":\"boot\"");
    heap_ck("journal");

    /* 3. volatile catalog + therapy FSM + scheduler                  */
    cat_init();
    err = sched_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sched_init failed");
        lcd_ui_row(9, "SCHED FAIL", 1);
    }
    heap_ck("sched");
    drift_reset();
    /* newest accurate-tier SOT from NVS — served immediately on boot
     * until a newer live estimate supersedes it (U6)                */
    if (drift_sot_load() == ESP_OK) {
        drift_sot_t sot;
        if (drift_sot_view(&sot)) {
            ESP_LOGI(TAG, "SOT restored: drift=%lldms ±%lldms seq=%lu",
                     (long long)sot.clock_drift_ms,
                     (long long)sot.accuracy_half_ms,
                     (unsigned long)sot.persisted_seq);
        }
    }
    heap_ck("sot_load");

    /* card worker task — the only HTTP owner (spec §13.1). Created
     * BEFORE netw/BLE init: the 16 KiB stack is one of the largest
     * contiguous internal-RAM allocations, and post-NimBLE the heap
     * is fragmented enough that xTaskCreate can fail silently — a
     * dead card task means no catalog, no therapy detection, while
     * BLE keeps answering (observed in the field). The task only
     * idles on its queue until the link comes up.                 */
    if (xTaskCreate(card_task, "card", 16384, NULL, 5, NULL)
            != pdPASS) {
        sched_note_task_created(false);
        ESP_LOGE(TAG, "card task create failed");
        lcd_ui_row(9, "CARD TASK FAIL", 1);
        char rec[96];
        snprintf(rec, sizeof(rec),
                 "\"e\":\"card_task_fail\",\"int_free\":%lu,"
                 "\"int_largest\":%lu",
                 (unsigned long)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL));
        jrn_put("event", rec);
    } else {
        sched_note_task_created(true);
    }

    /* 4. WiFi — STA with stored config, or fall back to setup AP     */
    err = netw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netw_init failed");
        lcd_ui_row(9, "WIFI FAIL", 1);
    }
    netw_set_link_cb(on_wifi_link);
    if (provisioned) {
        netw_enter_sta(&cfg);
    } else {
        /* unconfigured: straight to setup AP so the user can pair    */
        netw_enter_ap();
        ble_svc_suspend();           /* give the setup AP the radio   */
        show_setup_ap();
    }
    heap_ck("netw");

    /* 5. BLE stack — advertises; pairing window opens on button      */
    err = ble_svc_init();
    if (err == ESP_OK) {
        ble_svc_advertise();
        if (!ble_svc_peer_bonded()) {
            lcd_ui_row(10, "PRESS BTN: PAIR", 0);
        }
    } else {
        ESP_LOGE(TAG, "ble_svc_init failed");
        lcd_ui_row(10, "BLE FAIL", 1);
    }
    heap_ck("ble");
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
    ESP_LOGI(TAG, "mem internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif

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
                ble_svc_resume();
            } else {
                netw_enter_ap();
                ble_svc_suspend();
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
                    lcd_ui_row(11, "PAIRING 120S", 1);
                }
            }
        }

        /* setup AP timeouts: 5 min idle / 15 min absolute (§4.1)      */
        if (netw_mode() == NETW_MODE_AP &&
            (portal_idle_us() > 5LL * 60 * 1000000 ||
             portal_age_us() > 15LL * 60 * 1000000)) {
            lcd_ui_row(11, "SETUP TIMEOUT", 1);
            netw_leave_ap();
            ble_svc_resume();
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                netw_enter_sta(&cfg);
            }
        }

        /* portal committed new config -> leave AP, retry STA         */
        if (portal_saved_flag && netw_mode() == NETW_MODE_AP) {
            portal_saved_flag = false;
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                lcd_ui_row(11, "SAVED - JOINING", 1);
                drift_timezone_set(cfg.as10_tz, cfg.timezone_confirmed);
                netw_leave_ap();
                ble_svc_resume();
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
            if (ble_svc_pin_visible()) {
                s_pin_up = true;        /* PIN screen owns the LCD    */
                char code[8];
                if (ble_svc_pin_take_request(code, sizeof(code))) {
                    lcd_ui_pin_screen("BLE PAIRING", code,
                                      "TYPE PIN IN APP");
                }
            } else {
                if (s_pin_up) {         /* PIN closed — repaint chrome */
                    s_pin_up = false;
                    lcd_ui_clear();
                    lcd_ui_headline("SOMNOBRIDGE");
                }
                ui_tick();
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
