/*
 * SomnoPal — BLE service: NimBLE GATT + security (spec §3, §10)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * One connection, one authorized peer. LE Secure Connections + MITM +
 * bonding + 128-bit key => security level 4. DisplayOnly I/O: SomnoPal
 * shows a fresh 6-digit passkey, SomnoTrace types it.
 */
#include <string.h>

#include "ble_svc.h"
#include "ble_proto.h"
#include "board.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* ble_store_config_init() is defined in the host store but not declared in a
 * public header — ESP-IDF examples forward-declare it the same way.         */
void ble_store_config_init(void);
extern esp_err_t ble_ops_init(void);

static const char *TAG = "ble";

/* ---------------- UUIDs (§10.1) ---------------- */
/* 8ba7d1xx-6e4b-4bb4-9f1d-7306766e6f50, bytes little-endian on air */
static const ble_uuid128_t UUID_SVC =
    BLE_UUID128_INIT(0x50,0x6f,0x6e,0x76,0x06,0x73,0x1d,0x9f,
                     0xb4,0x4b,0x4b,0x6e,0x00,0xd1,0xa7,0x8b);
static const ble_uuid128_t UUID_CTRL =
    BLE_UUID128_INIT(0x50,0x6f,0x6e,0x76,0x06,0x73,0x1d,0x9f,
                     0xb4,0x4b,0x4b,0x6e,0x01,0xd1,0xa7,0x8b);
static const ble_uuid128_t UUID_DATA =
    BLE_UUID128_INIT(0x50,0x6f,0x6e,0x76,0x06,0x73,0x1d,0x9f,
                     0xb4,0x4b,0x4b,0x6e,0x02,0xd1,0xa7,0x8b);

/* ---------------- state ---------------- */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_ctrl_handle, s_data_handle;
static bool     s_ctrl_sub, s_data_sub;
static bool     s_pairing_open;
static bool     s_pin_on_screen;
static bool     s_pin_draw_pending;
static char     s_pin_code[8];
static int64_t  s_pairing_deadline_us;
static int64_t  s_pairing_attempt_us;
static int      s_pairing_fails;
static bool     s_have_peer;

/* ---------------- access gate ---------------- */

/* Every read/write/subscribe on our chars goes through here. */
static int access_gate(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_CTRL.u,
                .access_cb = access_gate,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_INDICATE |
                         BLE_GATT_CHR_F_READ_ENC,    /* subscription too */
                .val_handle = &s_ctrl_handle,
                .cpfd = NULL,
            },
            {
                .uuid = &UUID_DATA.u,
                .access_cb = access_gate,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_data_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---------------- identity / authorization ---------------- */

/* Compare the connected peer's bonded identity address against our
 * stored authorized peer (§3.1). Private random resolvable addresses
 * map to the same identity after bonding, so compare identity addr. */
static bool peer_is_authorized(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) {
        return false;
    }
    if (!(d.sec_state.encrypted && d.sec_state.authenticated &&
          d.sec_state.bonded)) {
        return false;
    }
    if (!s_have_peer) {
        return false;
    }
    uint8_t want_type, want_addr[6];
    if (settings_load_peer(&want_type, want_addr) != ESP_OK) {
        return false;
    }
    /* d.peer_id_addr = identity address (post-bonding) */
    return memcmp(d.peer_id_addr.val, want_addr, 6) == 0;
}

bool ble_svc_authorized(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE &&
           peer_is_authorized(s_conn);
}
bool ble_svc_connected(void)   { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_svc_peer_bonded(void) { return s_have_peer; }
uint16_t ble_svc_att_mtu(void) { return s_mtu; }
uint16_t ble_svc_conn_handle(void) { return s_conn; }

/* ---------------- GATT access gate ---------------- */

extern int ble_ops_on_write(uint16_t conn, const uint8_t *data,
                            uint16_t len);
extern int ble_ops_on_subscribe(uint16_t conn, uint16_t attr, bool sub);

static int access_gate(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Subscription writes (CCCD) flow through BLE_GATT_ACCESS_OP_WRITE
     * on the descriptor path — NimBLE calls this for chr value access;
     * CCCD changes arrive via ble_gap_event subscribe instead. */
    if (!peer_is_authorized(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* om_data/om_len cover only the first mbuf segment — flatten
         * the chain (bounded by negotiated ATT MTU)                 */
        uint16_t olen = ctxt->om ? OS_MBUF_PKTLEN(ctxt->om) : 0;
        uint8_t buf[544];
        if (olen > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, olen, buf) != 0) {
            return BLE_ATT_ERR_INVALID_PDU;
        }
        return ble_ops_on_write(conn_handle, buf, olen);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

/* ---------------- security ---------------- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static uint32_t gen_passkey(void)
{
#ifdef CONFIG_SOMNOPAL_DIAG_FIXED_PASSKEY
    /* DIAG: fixed PIN so an automated agent can answer without waiting
     * for the LCD/log. Never set in production.                      */
    return 424242;
#else
    return esp_random() % 1000000;   /* six digits incl. leading zeros */
#endif
}

static int on_passkey(struct ble_gap_event *ev, void *arg)
{
    /* never display a PIN outside the physical pairing window —
     * exception: our stored peer re-establishing its own bond      */
    if (!s_pairing_open) {
        bool known = false;
        struct ble_gap_conn_desc d;
        if (s_have_peer &&
            ble_gap_conn_find(s_conn, &d) == 0) {
            uint8_t t, a[6];
            known = settings_load_peer(&t, a) == ESP_OK &&
                    memcmp(d.peer_id_addr.val, a, 6) == 0;
        }
        if (!known) {
            return BLE_HS_EINVAL;
        }
    }
    if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
        uint32_t pk = gen_passkey();
        /* The displayed passkey must be handed back to SM via
         * ble_sm_inject_io — the proc blocks on ADVANCE_ON_IO until
         * proc->tk is filled (writing params.numcmp is a no-op).     */
        struct ble_sm_io io = {
            .action  = BLE_SM_IOACT_DISP,
            .passkey = pk,
        };
        int rc = ble_sm_inject_io(ev->passkey.conn_handle, &io);
        if (rc != 0) {
            ESP_LOGW(TAG, "inject_io failed rc=%d", rc);
            return BLE_HS_EINVAL;
        }
        ble_svc_show_pin(pk);             /* stamps s_pairing_attempt  */
        return 0;
    }
    if (ev->passkey.params.action == BLE_SM_IOACT_INPUT) {
        /* We're display-only; never accept an input action. */
        return BLE_HS_EINVAL;
    }
    return 0;
}

static void bond_to_peer(const struct ble_gap_conn_desc *d)
{
    /* unknown centrals may only bond inside the physical pairing
     * window (§3.1) — checked here at bond creation                 */
    if (!s_have_peer &&
        (!s_pairing_open ||
         esp_timer_get_time() > s_pairing_deadline_us)) {
        ESP_LOGW(TAG, "bond attempt outside pairing window");
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    settings_save_peer(d->peer_id_addr.type, d->peer_id_addr.val);
    s_have_peer = true;
    s_pairing_open = false;         /* window closes on success (§3.1) */
    ble_svc_hide_pin();
    ESP_LOGI(TAG, "peer bonded+authorized");
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
#if !defined(CONFIG_HEAP_POISONING_DISABLED)
        ESP_LOGI(TAG, "heap_ck gap_connect st=%u: %s",
                 (unsigned)ev->connect.status,
                 heap_caps_check_integrity_all(false) ? "OK" : "CORRUPT");
#endif
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_mtu = 23;
            ESP_LOGI(TAG, "connected h=%u", s_conn);
            /* negotiate MTU early; link encrypts on bond/secured req  */
            ble_att_set_preferred_mtu(247);
        } else {
            ble_svc_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%u",
                 ev->disconnect.reason);
        ble_svc_stream_abort(0, "LINK_LOST");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ctrl_sub = s_data_sub = false;
        ble_svc_hide_pin();
        ble_svc_advertise();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        break;

    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "MTU=%u", s_mtu);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_ctrl_handle) {
            s_ctrl_sub = ev->subscribe.cur_indicate != 0;
        } else if (ev->subscribe.attr_handle == s_data_handle) {
            s_data_sub = ev->subscribe.cur_notify != 0;
        }
        /* CCCDs are only honoured on an authorized link — the gate is
         * enforced at access + send time (check before notify). */
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        return on_passkey(ev, arg);

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn, &d) == 0 &&
                d.sec_state.bonded && !s_have_peer) {
                bond_to_peer(&d);
            }
        } else if (s_pairing_open) {
            /* failed pairing attempt counts toward the 3-try limit
             * and a stalled attempt expires after 60 s (§3.1)        */
            ESP_LOGW(TAG, "enc_change fail status=%u",
                     ev->enc_change.status);
            s_pairing_fails++;
            if (s_pairing_fails >= 3) {
                ESP_LOGW(TAG, "3 pairing failures — closing window");
                ble_svc_close_pairing();
            }
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* indication confirm — the streamer tracks via tx failure */
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* already bonded peer re-pairing: allow silently (same bond),
         * strangers never reach this (we refuse below). */
        ESP_LOGI(TAG, "repeat_pairing event");
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            if (s_have_peer) {
                uint8_t t, a[6];
                if (settings_load_peer(&t, a) == ESP_OK &&
                    memcmp(d.peer_id_addr.val, a, 6) == 0) {
                    ble_store_util_delete_peer(&d.peer_id_addr);
                    return BLE_GAP_REPEAT_PAIRING_RETRY;
                }
            }
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    }
    return 0;
}

/* ---------------- pairing window ---------------- */

esp_err_t ble_svc_open_pairing(void)
{
    if (s_have_peer) {
        return ESP_ERR_INVALID_STATE;   /* bonded: FORGET_PEER first   */
    }
    s_pairing_open = true;
    s_pairing_deadline_us = esp_timer_get_time() + 120LL * 1000 * 1000;
    s_pairing_fails = 0;
    ESP_LOGI(TAG, "pairing window open 120 s");
    return ESP_OK;
}

void ble_svc_close_pairing(void)
{
    s_pairing_open = false;
    ble_svc_hide_pin();
}

bool ble_svc_pairing_open(void) { return s_pairing_open; }

/* enforce the 120-s window + 60-s per-attempt expiry (§3.1);
 * call ~1 Hz from the app loop                                       */
void ble_svc_pairing_tick(int64_t now_us)
{
    if (!s_pairing_open) {
        return;
    }
    if (now_us > s_pairing_deadline_us) {
        ESP_LOGI(TAG, "pairing window expired");
        ble_svc_close_pairing();
        return;
    }
    /* an in-progress attempt that never completed gets 60 s          */
    if (s_pairing_attempt_us &&
        now_us - s_pairing_attempt_us > 60LL * 1000 * 1000) {
        s_pairing_attempt_us = 0;
        s_pairing_fails++;
        ble_svc_hide_pin();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (s_pairing_fails >= 3) {
            ble_svc_close_pairing();
        }
    }
}

void ble_svc_forget_peer(void)
{
    /* delete our authorized record + all NimBLE bond stores */
    uint8_t t, a[6];
    if (settings_load_peer(&t, a) == ESP_OK) {
        ble_addr_t pa = { .type = t };
        memcpy(pa.val, a, 6);
        ble_store_util_delete_peer(&pa);
    }
    settings_clear_peer();
    s_have_peer = false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ESP_LOGW(TAG, "peer forgotten");
}

/* ---------------- advertisement ---------------- */

void ble_svc_advertise(void)
{
    struct ble_gap_adv_params p = {0};
    struct ble_hs_adv_fields f = {0};

    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)"SomnoPal";
    f.name_len = 8;
    f.name_is_complete = 1;
    /* service UUID in adv so scanners can filter w/o connecting —
     * generic label only; no status/patient fields (§3.1)           */
    f.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &p, gap_event, NULL);
}

/* ---------------- notifications / streaming ---------------- */

bool ble_svc_control_indicate(const uint8_t *data, uint16_t len)
{
    if (!ble_svc_authorized() || !s_ctrl_sub) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_indicate_custom(s_conn, s_ctrl_handle, om) == 0;
}

bool ble_svc_send_bytes(const uint8_t *data, uint16_t len)
{
    if (!ble_svc_authorized() || !s_data_sub) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(s_conn, s_data_handle, om) == 0;
}

void ble_svc_show_pin(uint32_t passkey)
{
    /* runs in the NimBLE host task — never touch the LCD here: the app
     * task owns all screen traffic (single s_rowbuf + done semaphore).
     * Record the code and let the app loop render it next tick.       */
    snprintf(s_pin_code, sizeof(s_pin_code), "%06lu",
             (unsigned long)passkey);
    s_pairing_attempt_us = esp_timer_get_time();
    s_pin_on_screen = true;
    s_pin_draw_pending = true;
}
void ble_svc_hide_pin(void)
{
    s_pin_on_screen = false;
    s_pin_draw_pending = false;
}
bool ble_svc_pin_visible(void) { return s_pin_on_screen; }

bool ble_svc_pin_take_request(char *code, size_t len)
{
    if (!s_pin_draw_pending) {
        return false;
    }
    s_pin_draw_pending = false;
    strncpy(code, s_pin_code, len);
    code[len - 1] = '\0';
    return true;
}

/* ---------------- init ---------------- */

static void on_sync(void)
{
    ble_svc_advertise();
    if (settings_has_peer()) {
        s_have_peer = true;
    }
}
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset: %d", reason);
}
static void host_task(void *p) { nimble_port_run(); }

esp_err_t ble_svc_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* security: Secure Connections, MITM, bonding, display-only (§3.1) */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_sc_only = 1;             /* LE Secure Connections only  */

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc) return ESP_FAIL;
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc) return ESP_FAIL;

        /* persistent bond storage — ble_store_config uses NVS when
     * CONFIG_BT_NIMBLE_NVS_PERSIST=y (spec §3.1)                   */
    ble_store_config_init();
    ble_ops_init();

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
