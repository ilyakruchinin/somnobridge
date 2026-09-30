/*
 * SomnoPal — NVS settings store (spec §4.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"

#if CONFIG_NVS_ENCRYPTION
#include "nvs_sec_provider.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SP_SETTINGS_SCHEMA   1
#define SP_SETTINGS_TZ_MAX   96

typedef enum {
    SP_SEC_NOT_PROVISIONED = 0, /* no eFuse HMAC key — fail closed (prod) */
    SP_SEC_DEV_UNPROTECTED,     /* dev build, plain NVS                    */
    SP_SEC_ENCRYPTED,           /* HMAC-scheme NVS encryption active       */
} sp_security_state_t;

typedef struct {
    uint32_t schema;                    /* SP_SETTINGS_SCHEMA            */
    uint8_t  magic_valid;               /* set by settings_load          */

    char     ezshare_ssid[33];          /* 1..32 bytes, UTF-8            */
    char     ezshare_password[65];      /* 8..63 ASCII or 64-hex PSK     */
    uint8_t  allow_open_wifi;           /* explicit open-network ack     */

    uint8_t  ezshare_bssid[6];          /* optional MAC lock             */
    uint8_t  bssid_lock;                /* ezshare_bssid is valid        */

    uint32_t http_host_ipv4;            /* host-order IPv4, 0 = gateway  */

    char     as10_tz[SP_SETTINGS_TZ_MAX + 1];  /* POSIX TZ, "UTC0" init  */
    uint8_t  timezone_confirmed;

    uint8_t  lcd_brightness;            /* 1..100, initial 30            */
} sp_settings_t;

/**
 * Init all NVS partitions and load settings.
 *
 * When the eFuse HMAC key is absent:
 *   - dev build  -> plain NVS, state SP_SEC_DEV_UNPROTECTED
 *   - prod build -> state SP_SEC_NOT_PROVISIONED; settings_load returns
 *     ESP_ERR_NOT_SUPPORTED and nothing is persisted unencrypted.
 *
 * The eFuse key is NEVER auto-burned by this code.
 */
esp_err_t settings_init(void);

esp_err_t settings_load(sp_settings_t *out);
esp_err_t settings_save(const sp_settings_t *in);   /* validates + commits */
esp_err_t settings_validate(const sp_settings_t *s);/* pure — host-testable */
void      settings_defaults(sp_settings_t *out);
esp_err_t settings_clear(void);                     /* wipe sp_config blob */

sp_security_state_t settings_security(void);
const char *settings_security_label(void);

/* Bond-authority helpers (§3.1): the app-level authorized peer identity.
 * NimBLE's own bond records live in the same encrypted NVS partition; this
 * is our application copy used for identity checks. */
esp_err_t settings_load_peer(uint8_t *id_addr_type, uint8_t addr[6]);
esp_err_t settings_save_peer(uint8_t id_addr_type, const uint8_t addr[6]);
esp_err_t settings_clear_peer(void);
bool      settings_has_peer(void);

/* NVS handles for the dedicated partitions (NULL until settings_init). */
nvs_handle_t settings_nvs_config(void);   /* "nvs" partition   */
nvs_handle_t settings_nvs_metadata(void); /* "metadata" partition */

/* NVS encryption key config for the evidence/metadata partitions, or NULL
 * when no eFuse HMAC key is provisioned (dev / not-provisioned states).
 * Journal code must pass this to nvs_flash_secure_init_partition. */
const nvs_sec_cfg_t *settings_sec_cfg(void);

/** True when persistent security is active (credentials may be stored). */
bool settings_storage_protected(void);

#ifdef __cplusplus
}
#endif
