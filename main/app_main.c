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
#include "esp_app_desc.h"
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

/* set by a BLE SET_CONFIG op (ble_ops.c) — same meaning as
 * portal_saved_flag but the app is not in AP mode, so the loop below
 * re-enters STA directly instead of leaving setup first.            */
volatile bool ble_cfg_saved_flag;

/* ---------------- LCD status UI (landscape 240x135) ---------------- */

#define UI_GREEN   LCD_RGB(0, 255, 120)
#define UI_AMBER   LCD_RGB(255, 190, 0)
#define UI_RED     LCD_RGB(255, 70, 70)
#define UI_BLUE    LCD_RGB(80, 180, 255)   /* BT/session — light blue  */
#define UI_BLUE_D  LCD_RGB(70, 120, 180)   /* bonded, idle             */
#define UI_GREY    LCD_RGB(200, 200, 200)
#define UI_MID     LCD_RGB(140, 140, 140)
#define UI_DIM     LCD_RGB(80, 80, 80)

/* boot/runtime faults that must stay visible on the status line */
enum { FAULT_WIFI = 1, FAULT_BLE = 2, FAULT_SCHED = 4, FAULT_CARD = 8 };
static uint8_t s_fault;

/* transient message (portal actions, config commits): highest-priority
 * status line until it expires */
static struct {
    char     text[32];
    uint16_t color;
    int64_t  until_us;
} s_notice;

/* redraw guards: a full frame costs ~20 ms of SPI, so each screen
 * repaints only when its composed state changes */
static char s_status_key[96];
static char s_setup_key[96];
static bool s_pin_was_up;

static void ui_invalidate(void)
{
    s_status_key[0] = '\0';
    s_setup_key[0] = '\0';
}

static void ui_notice(const char *text, uint16_t color, int seconds)
{
    snprintf(s_notice.text, sizeof(s_notice.text), "%s", text);
    s_notice.color = color;
    s_notice.until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
}

/* boot splash doubles as a boot progress bar */
static void ui_splash(int percent)
{
    static bool drawn;
    if (!drawn) {
        lcd_ui_clear();
        lcd_ui_text_center(38, 2, UI_GREEN, "SOMNOBRIDGE");
        lcd_ui_text_center(78, 1, UI_GREY,
                           esp_app_get_description()->version);
        drawn = true;
    }
    lcd_ui_progress(40, 104, 160, 8, percent, UI_GREEN);
}

/* "-8m 34.3s" — drift rounded to 0.1 s */
static void fmt_drift(char *out, size_t n, int64_t ms)
{
    int64_t a = ms < 0 ? -ms : ms;
    int64_t tenths = (a + 50) / 100;
    snprintf(out, n, "%c%lldm %lld.%llds",
             ms < 0 ? '-' : '+',
             (long long)(tenths / 600),
             (long long)((tenths % 600) / 10),
             (long long)(tenths % 10));
}

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
        ui_notice("PEER FORGOTTEN", UI_GREEN, 15);
        jrn_put("event", "\"e\":\"forget_peer\"");
        break;
    case PORTAL_ACTION_RESET_SETTINGS:
        settings_clear();               /* EZShare cfg, keeps peer    */
        drift_sot_invalidate();         /* card may change (U6)       */
        ui_notice("SETTINGS RESET", UI_GREEN, 15);
        jrn_put("event", "\"e\":\"reset_settings\"");
        break;
    case PORTAL_ACTION_CLEAR_EVIDENCE:
        jrn_erase_all();
        ui_notice("EVIDENCE CLEARED", UI_GREEN, 15);
        break;
    case PORTAL_ACTION_REBUILD_CATALOG:
        cat_init();                     /* volatile: drops all state  */
        ui_notice("CATALOG CLEARED", UI_GREEN, 15);
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

/* setup AP screen: the credentials are the only thing the user needs
 * while the portal is up (5 min idle / 15 min absolute, §4.1) */
static void ui_setup_draw(void)
{
    char ssid[33], pass[16];
    netw_ap_credentials(ssid, pass);
    int64_t now = esp_timer_get_time();
    const char *notice = (s_notice.text[0] && now < s_notice.until_us)
        ? s_notice.text : "";
    char key[96];
    snprintf(key, sizeof(key), "%s|%s|%s", ssid, pass, notice);
    if (strcmp(key, s_setup_key) == 0) {
        return;
    }
    snprintf(s_setup_key, sizeof(s_setup_key), "%s", key);
    s_status_key[0] = '\0';        /* status repaints on return */
    lcd_ui_clear();
    lcd_ui_row(0, "SOMNOBRIDGE SETUP", 1);
    lcd_ui_rowf(2, 0, "AP: %s", ssid);
    if (pass[0]) {
        lcd_ui_rowf(3, 0, "PW: %s", pass);
    } else {
        lcd_ui_row(3, "PW: (open network)", 0);
    }
    lcd_ui_row(5, "OPEN http://192.168.8.1", 0);
    lcd_ui_row(6, "BTN: CONFIRM  HOLD: EXIT", 0);
    if (notice[0]) {
        lcd_ui_row(7, notice, 1);
    }
}

/* leave AP mode: the setup screen is redrawn as the status screen by
 * the next tick; the notice bridges the transition */
static void leave_setup(const char *msg)
{
    ui_notice(msg, UI_GREEN, 5);
    netw_leave_ap();
    ble_svc_resume();
}

/* normal status screen: card link + BLE session + transfer in the top
 * bar, clock-drift hero in the middle, one status line at the bottom.
 * Redrawn only when the composed state changes — a full frame costs
 * ~20 ms of SPI time, so an idle screen must not blink once a second. */
static void ui_status_draw(void)
{
    const netw_status_t *n = netw_status();
    int pct = ble_svc_stream_percent();

    uint16_t bt;
    if (ble_svc_connected() && ble_svc_authorized()) {
        bt = UI_BLUE;                       /* SomnoTrace session live */
    } else if (ble_svc_connected()) {
        bt = UI_AMBER;                      /* link up, not authorized */
    } else if (ble_svc_peer_bonded()) {
        bt = UI_BLUE_D;                     /* paired, idle            */
    } else {
        static bool blink;
        blink = !blink;
        bt = (ble_svc_pairing_open() && blink) ? UI_AMBER : UI_DIM;
    }

    /* hero: clock-drift estimate. Nulls render as "---" — never a
     * fabricated value (U10). */
    char val[28];
    char sub[40] = "";
    uint16_t vc = UI_DIM;
    drift_record_t dr;
    if (drift_pub_get(&dr) && dr.has_drift) {
        fmt_drift(val, sizeof(val), dr.clock_drift_ms);
        int64_t half = (dr.accuracy_half_ms + 50) / 100;   /* 0.1 s */
        snprintf(sub, sizeof(sub), "+/-%lld.%llds",
                 (long long)(half / 10), (long long)(half % 10));
        vc = dr.tier == ACC_ACCURATE ? UI_GREEN :
             dr.tier == ACC_APPROXIMATE ? UI_AMBER : UI_GREY;
    } else {
        drift_sot_t sot;
        if (drift_sot_view(&sot)) {
            fmt_drift(val, sizeof(val), sot.clock_drift_ms);
            snprintf(sub, sizeof(sub), "LAST KNOWN");
            vc = UI_GREY;
        } else {
            snprintf(val, sizeof(val), "---");
        }
    }

    /* status line: notice > fault > pairing > therapy > transfer */
    const char *line = "";
    uint16_t lc = 0xFFFF;
    int64_t now = esp_timer_get_time();
    static char sync_line[20];
    if (s_notice.text[0] && now < s_notice.until_us) {
        line = s_notice.text;
        lc = s_notice.color;
    } else if (s_fault) {
        line = (s_fault & FAULT_WIFI) ? "WIFI FAIL" :
               (s_fault & FAULT_BLE)  ? "BLE FAIL" :
               (s_fault & FAULT_CARD) ? "CARD FAIL" : "SCHED FAIL";
        lc = UI_RED;
    } else if (!ble_svc_peer_bonded() && !ble_svc_connected()) {
        line = ble_svc_pairing_open() ? "PAIRING - WAIT FOR APP"
                                      : "HOLD BTN TO PAIR";
        lc = UI_AMBER;
    } else if (therapy_status()->state == THERAPY_IN) {
        line = "THERAPY - TRANSFERS PAUSED";
        lc = UI_BLUE;
    } else if (pct >= 0) {
        snprintf(sync_line, sizeof(sync_line), "SYNCING %d%%", pct);
        line = sync_line;
    }

    char key[96];
    snprintf(key, sizeof(key), "%d|%d|%u|%s|%s|%s|%d",
             n->sta_connected, n->rssi / 10, (unsigned)bt,
             val, sub, line, pct);
    if (strcmp(key, s_status_key) == 0) {
        return;
    }
    snprintf(s_status_key, sizeof(s_status_key), "%s", key);
    s_setup_key[0] = '\0';         /* setup repaints on return */

    lcd_ui_clear();
    lcd_ui_wifi_bars(8, 5, n->rssi, n->sta_connected);
    lcd_ui_bt_rune(40, 5, bt);
    if (pct >= 0) {
        lcd_ui_sync(BOARD_LCD_HRES - 8, 7, UI_BLUE, pct);
    }
    lcd_ui_rect(0, 24, BOARD_LCD_HRES, 1, LCD_RGB(60, 60, 60));

    lcd_ui_text_center(30, 1, UI_GREY, "CLOCK DRIFT");
    lcd_ui_text_center(46, 2, vc, val);
    if (sub[0]) {
        lcd_ui_text_center(84, 1, UI_MID, sub);
    }
    if (line[0]) {
        lcd_ui_text_center(112, 1, lc, line);
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
    ui_splash(15);
    heap_ck("board");

    /* 1. settings/security first — NVS must be up before the journal
     *    or the NimBLE bond store touch flash.                      */
    err = settings_init();
    sp_settings_t cfg;
    bool provisioned = err == ESP_OK && settings_load(&cfg) == ESP_OK &&
                       cfg.magic_valid;
    drift_timezone_set(cfg.as10_tz, cfg.timezone_confirmed);
    if (provisioned) {
        board_backlight_set(cfg.lcd_brightness);
    }
    ui_splash(35);
    heap_ck("settings");

    /* 2. evidence journal (may run degraded without security cfg)     */
    err = jrn_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "journal degraded: %s", esp_err_to_name(err));
    }
    jrn_put("event", "\"e\":\"boot\"");
    ui_splash(50);
    heap_ck("journal");

    /* 3. volatile catalog + therapy FSM + scheduler                  */
    cat_init();
    err = sched_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sched_init failed");
        s_fault |= FAULT_SCHED;
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
        s_fault |= FAULT_CARD;
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

    /* 4. BLE stack — moved BEFORE netw_init: NimBLE needs ~42 KiB of
     * contiguous internal-RAM allocs in one shot (MSYS pools, ble_tx,
     * ble_stream 16 KiB, nimble_host 12 KiB), and nimble_port's own
     * xTaskCreatePinnedToCore failure is silent — post-WiFi the heap
     * was fragmented enough (largest block 7.6 KiB) that nimble_host
     * never spawned, leaving the device alive but permanently
     * non-advertising. Same failure class as the card_task comment
     * above. BLE adv already overlaps WiFi attach regardless.       */
    err = ble_svc_init();
    if (err == ESP_OK) {
        ble_svc_advertise();
    } else {
        ESP_LOGE(TAG, "ble_svc_init failed");
        s_fault |= FAULT_BLE;
    }
    ui_splash(70);
    heap_ck("ble");

    /* 5. WiFi — STA with stored config, or fall back to setup AP     */
    err = netw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netw_init failed");
        s_fault |= FAULT_WIFI;
    }
    netw_set_link_cb(on_wifi_link);
    if (provisioned) {
        netw_enter_sta(&cfg);
    } else {
        /* unconfigured: straight to setup AP so the user can pair    */
        netw_enter_ap();
        ble_svc_suspend();           /* give the setup AP the radio   */
        ui_setup_draw();
    }
    ui_splash(90);
    heap_ck("netw");

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
                leave_setup("LEAVING SETUP");
                /* One press is the whole fresh-bridge gesture: resume
                 * BLE, join the card if config already exists (e.g. it
                 * was pushed over BLE earlier), and — when nobody is
                 * bonded yet — open the 120 s pairing window so the
                 * same physical press both proves presence and invites
                 * the new peer.                                      */
                if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                    netw_enter_sta(&cfg);
                    provisioned = true;
                }
                if (!ble_svc_peer_bonded() &&
                    ble_svc_open_pairing() == ESP_OK) {
                    ui_notice("PAIRING 120S", UI_AMBER, 5);
                }
            } else {
                netw_enter_ap();
                ble_svc_suspend();
                ui_setup_draw();
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
                    ui_notice("PAIRING 120S", UI_AMBER, 5);
                }
            }
        }

        /* setup AP timeouts: 5 min idle / 15 min absolute (§4.1)      */
        if (netw_mode() == NETW_MODE_AP &&
            (portal_idle_us() > 5LL * 60 * 1000000 ||
             portal_age_us() > 15LL * 60 * 1000000)) {
            leave_setup("SETUP TIMEOUT");
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                netw_enter_sta(&cfg);
            }
        }

        /* BLE-committed config (paired path): apply immediately —
         * the link stays up while STA re-joins the (possibly new)
         * EZShare card.                                              */
        if (ble_cfg_saved_flag) {
            ble_cfg_saved_flag = false;
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                drift_timezone_set(cfg.as10_tz, cfg.timezone_confirmed);
                board_backlight_set(cfg.lcd_brightness);
                if (netw_mode() != NETW_MODE_AP) {
                    ui_notice("SAVED - JOINING", UI_GREEN, 10);
                    netw_enter_sta(&cfg);
                    provisioned = true;
                }
            }
        }

        /* portal committed new config -> leave AP, retry STA         */
        if (portal_saved_flag && netw_mode() == NETW_MODE_AP) {
            portal_saved_flag = false;
            if (settings_load(&cfg) == ESP_OK && cfg.magic_valid) {
                drift_timezone_set(cfg.as10_tz, cfg.timezone_confirmed);
                board_backlight_set(cfg.lcd_brightness);
                leave_setup("SAVED - JOINING");
                netw_enter_sta(&cfg);
                provisioned = true;
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last > 1000 * 1000) {
            last = now;
            static int s_tick;
            if (++s_tick % 10 == 0) {
                bool adv, susp, syncd, htask; uint16_t ch; int stg;
                uint16_t itvl; uint8_t tph, rph;
                ble_svc_diag_state(&adv, &susp, &ch, &syncd, &stg, &htask,
                                   &itvl, &tph, &rph);
                ESP_LOGI(TAG,
                         "diag adv=%d susp=%d sync=%d stage=%d htask=%d "
                         "conn=%u bonded=%d heap_i=%u lrg_i=%u stream=%d "
                         "itvl=%u phy=%u/%u",
                         adv, susp, syncd, stg, htask, ch,
                         ble_svc_peer_bonded(),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL),
                         xTaskGetHandle("ble_stream") != NULL,
                         itvl, tph, rph);
                static const char *const tasks[] = {
                    "main", "card", "nimble_host", "ble_tx",
                    "ble_stream", "netw_reconn"
                };
                for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
                    TaskHandle_t task = xTaskGetHandle(tasks[i]);
                    if (task) {
                        ESP_LOGI(TAG, "stack task=%s free_min=%u", tasks[i],
                                 (unsigned)uxTaskGetStackHighWaterMark(task));
                    }
                }
                if (!adv && !susp && ch == 0xFFFF) {
                    ESP_LOGI(TAG, "diag: re-arming advertising");
                    ble_svc_advertise();
                }
            }
            jrn_tick(now);         /* 60-s evidence batch flush        */
            ble_svc_pairing_tick(now);   /* 120-s window / 60-s try   */
            netw_refresh_rssi();
            if (ble_svc_pin_visible()) {
                char code[8];           /* PIN screen owns the LCD    */
                if (ble_svc_pin_take_request(code, sizeof(code))) {
                    lcd_ui_pin_screen("BLE PAIRING", code,
                                      "TYPE PIN IN APP");
                }
                s_pin_was_up = true;
            } else if (netw_mode() == NETW_MODE_AP) {
                if (s_pin_was_up) {
                    s_pin_was_up = false;
                    ui_invalidate();
                }
                ui_setup_draw();
            } else {
                if (s_pin_was_up) {
                    s_pin_was_up = false;
                    ui_invalidate();
                }
                ui_status_draw();
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
