/*
 * SomnoBridge — BLE service (spec §3, §10)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_svc_init(void);
void      ble_svc_advertise(void);
void      ble_svc_suspend(void);   /* SETUP AP: yield the radio      */
void      ble_svc_resume(void);

/** Open the 120-s physical pairing window (§3.1). Only callable from
 * the app after a button press; denied if a peer is already bonded. */
esp_err_t ble_svc_open_pairing(void);
void      ble_svc_close_pairing(void);
bool      ble_svc_pairing_open(void);
/** Enforce window/attempt expiry — call ~1 Hz from the app loop. */
void      ble_svc_pairing_tick(int64_t now_us);

bool      ble_svc_peer_bonded(void);
bool      ble_svc_connected(void);
bool      ble_svc_authorized(void);   /* encrypted + bonded + identity  */
/* DIAG: runtime advert state for serial bring-up/debug (adv_active,
 * suspended, conn handle)                                       */
void      ble_svc_diag_state(bool *adv_active, bool *suspended,
                             uint16_t *conn_handle, bool *synced,
                             int *init_stage, bool *host_task_up);

/** Forget the authorized peer: disconnect + erase bond + app record. */
void      ble_svc_forget_peer(void);

/** Request one NTP-style exchange round from the peer (§8.1). */
void      ble_svc_ping_time(void);

/* Show pairing PIN state on the LCD via board helpers. */
void      ble_svc_show_pin(uint32_t passkey);
void      ble_svc_hide_pin(void);
/* true while the PIN screen is displayed — the app's status tick must
 * not redraw rows over it                                       */
bool      ble_svc_pin_visible(void);
/* App loop drains a pending PIN-screen draw (LCD is app-task-owned);
 * returns true once per show_pin, code buffer filled when it does.  */
bool      ble_svc_pin_take_request(char *code, size_t len);

/* streaming control used by ble_ops */
void      ble_svc_stream_abort(uint32_t request_id, const char *why);
bool      ble_svc_send_bytes(const uint8_t *data, uint16_t len);
bool      ble_svc_control_indicate(const uint8_t *data, uint16_t len);

/* connection props for the streaming sender */
uint16_t  ble_svc_att_mtu(void);
uint16_t  ble_svc_conn_handle(void);

#ifdef __cplusplus
}
#endif
