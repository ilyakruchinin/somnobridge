/*
 * SomnoPal — NVS settings store (spec §4.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Storage layout (namespace "sp_config"):
 *   "settings"  -> settings_blob_t (versioned, CRC32-protected)
 *   "peer"      -> 7 bytes: id_addr_type + identity address (BLE peer)
 *
 * Encryption: when the eFuse HMAC key configured by
 * CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID is provisioned (purpose HMAC_UP), all
 * three NVS partitions are opened through nvs_flash_secure_init*. The key
 * is NEVER auto-burned — a missing key means fail-closed (production) or
 * plainly-marked dev mode (CONFIG_SOMNOPAL_DEV_UNPROTECTED).
 */
#include <string.h>

#include "settings.h"

#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "settings";

#define NS_CONFIG   "sp_config"
#define KEY_SETTINGS "settings"
#define KEY_PEER     "peer"

static nvs_handle_t       s_cfg_handle;
static nvs_handle_t       s_meta_handle;
static nvs_sec_cfg_t      s_sec_cfg;
static bool               s_sec_cfg_valid;
static sp_security_state_t s_sec = SP_SEC_NOT_PROVISIONED;

/* ---------------- wire format ---------------- */

typedef struct {
    uint32_t schema;                     /* SP_SETTINGS_SCHEMA */
    uint32_t crc32;                      /* over fields below  */

    char     ezshare_ssid[33];
    char     ezshare_password[65];
    uint8_t  allow_open_wifi;
    uint8_t  ezshare_bssid[6];
    uint8_t  bssid_lock;
    uint32_t http_host_ipv4;
    char     as10_tz[SP_SETTINGS_TZ_MAX + 1];
    uint8_t  timezone_confirmed;
    uint8_t  lcd_brightness;
} settings_blob_t;   /* ~250 bytes; NVS blob, single-commit write */

/* ---------------- validation (host-testable pure part) ---------------- */

static bool tz_valid(const char *tz)
{
    if (!tz || !tz[0]) {
        return false;
    }
    size_t n = strlen(tz);
    if (n > SP_SETTINGS_TZ_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        /* POSIX TZ: letters/digits +-,:.<> whitespace-free printable */
        if (tz[i] < 0x21 || tz[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool password_valid(const sp_settings_t *s)
{
    size_t n = strlen(s->ezshare_password);
    if (n == 0) {
        return s->allow_open_wifi;      /* empty only with open ack   */
    }
    if (n == 64) {                      /* 64-hex PSK form            */
        for (size_t i = 0; i < 64; i++) {
            char c = s->ezshare_password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
    if (n < 8 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {    /* WPA2 passphrase: ASCII     */
        if (s->ezshare_password[i] < 0x20 || s->ezshare_password[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

esp_err_t settings_validate(const sp_settings_t *s)
{
    if (!s || s->schema != SP_SETTINGS_SCHEMA) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strnlen(s->ezshare_ssid, sizeof(s->ezshare_ssid));
    if (ssid_len == 0 || ssid_len > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!password_valid(s) || !tz_valid(s->as10_tz)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s->lcd_brightness < 1 || s->lcd_brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    /* http_host_ipv4==0 means "use DHCP gateway". Otherwise reject
     * multicast/broadcast/loopback/reserved ranges (host byte order). */
    if (s->http_host_ipv4) {
        uint32_t a = s->http_host_ipv4;
        uint8_t top = a >> 24;
        if (top == 0 || top >= 224 || (a & 0xFF) == 0xFF) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

void settings_defaults(sp_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = SP_SETTINGS_SCHEMA;
    out->magic_valid = 0;
    strcpy(out->as10_tz, "UTC0");
    out->lcd_brightness = 30;
}

/* ---------------- init / security ---------------- */

static bool efuns_hmac_key_present(void)
{
    /* CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID 0..5 -> EFUSE_BLK_KEY0..KEY5 */
    esp_efuse_block_t blk =
        (esp_efuse_block_t)(EFUSE_BLK_KEY0 + CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
    esp_efuse_purpose_t p = esp_efuse_get_key_purpose(blk);
    ESP_LOGI(TAG, "eFuse key block %d purpose %d", (int)blk, (int)p);
    return p == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}

esp_err_t settings_init(void)
{
    bool key = efuns_hmac_key_present();

    if (key) {
        nvs_sec_config_hmac_t hc = {
            .hmac_key_id =
                (hmac_key_id_t)(CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID),
        };
        nvs_sec_scheme_t *scheme = NULL;
        ESP_RETURN_ON_ERROR(nvs_sec_provider_register_hmac(&hc, &scheme),
                            TAG, "hmac scheme");
        esp_err_t err = nvs_flash_read_security_cfg_v2(scheme, &s_sec_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read security cfg: %s", esp_err_to_name(err));
            return err;
        }
        s_sec_cfg_valid = true;
        err = nvs_flash_secure_init(&s_sec_cfg);
        if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
            err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_secure_init(&s_sec_cfg);
        }
        ESP_RETURN_ON_ERROR(err, TAG, "secure init");
        s_sec = SP_SEC_ENCRYPTED;
    } else if (CONFIG_SOMNOPAL_DEV_UNPROTECTED) {
        ESP_LOGW(TAG, "no eFuse HMAC key — DEV UNPROTECTED storage");
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
            err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
        s_sec = SP_SEC_DEV_UNPROTECTED;
    } else {
        ESP_LOGE(TAG, "SECURITY_NOT_PROVISIONED: eFuse HMAC key missing");
        /* Mount plain NVS anyway so the fault screen can run; every
         * persistence path refuses writes while unprovisioned. */
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            return err;
        }
        s_sec = SP_SEC_NOT_PROVISIONED;
    }

    esp_err_t err = nvs_open(NS_CONFIG, NVS_READWRITE, &s_cfg_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open %s: %s", NS_CONFIG, esp_err_to_name(err));
        return err;
    }

    /* "metadata" partition (spec §9): drift records + load_budget.
     * Only exists when the HMAC key is provisioned — no auto-erase, a
     * missing/broken partition degrades to a NULL handle + flags.    */
    s_meta_handle = 0;
    if (s_sec_cfg_valid) {
        esp_err_t e =
            nvs_flash_secure_init_partition("metadata", &s_sec_cfg);
        if (e == ESP_OK) {
            e = nvs_open_from_partition("metadata", "sp_meta",
                                        NVS_READWRITE, &s_meta_handle);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "metadata partition unusable: %s",
                     esp_err_to_name(e));
            s_meta_handle = 0;
        }
    }
    ESP_LOGI(TAG, "settings ready, security=%s", settings_security_label());
    return ESP_OK;
}

const nvs_sec_cfg_t *settings_sec_cfg(void)
{
    return s_sec_cfg_valid ? &s_sec_cfg : NULL;
}

sp_security_state_t settings_security(void) { return s_sec; }

bool settings_storage_protected(void)
{
    return s_sec != SP_SEC_NOT_PROVISIONED;
}

const char *settings_security_label(void)
{
    switch (s_sec) {
    case SP_SEC_ENCRYPTED:       return "ENCRYPTED";
    case SP_SEC_DEV_UNPROTECTED: return "DEV-UNPROTECTED";
    default:                     return "NOT-PROVISIONED";
    }
}

/* ---------------- load / save ---------------- */

static uint32_t blob_crc(const settings_blob_t *b)
{
    return esp_rom_crc32_le(0, (const uint8_t *)b + 8, sizeof(*b) - 8);
}

esp_err_t settings_load(sp_settings_t *out)
{
    settings_defaults(out);
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    settings_blob_t b;
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(s_cfg_handle, KEY_SETTINGS, &b, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(b) || b.schema != SP_SETTINGS_SCHEMA ||
        b.crc32 != blob_crc(&b)) {
        ESP_LOGW(TAG, "settings blob corrupt/unknown schema");
        return ESP_ERR_NVS_CORRUPT_KEY_PART;
    }
    out->schema = b.schema;
    out->magic_valid = 1;
    memcpy(out->ezshare_ssid, b.ezshare_ssid, sizeof(b.ezshare_ssid));
    memcpy(out->ezshare_password, b.ezshare_password,
           sizeof(b.ezshare_password));
    out->allow_open_wifi = b.allow_open_wifi;
    memcpy(out->ezshare_bssid, b.ezshare_bssid, 6);
    out->bssid_lock = b.bssid_lock;
    out->http_host_ipv4 = b.http_host_ipv4;
    memcpy(out->as10_tz, b.as10_tz, sizeof(b.as10_tz));
    out->timezone_confirmed = b.timezone_confirmed;
    out->lcd_brightness = b.lcd_brightness;
    return ESP_OK;
}

esp_err_t settings_save(const sp_settings_t *in)
{
    ESP_RETURN_ON_ERROR(settings_validate(in), TAG, "validate");
    if (!settings_storage_protected()) {
        return ESP_ERR_NOT_SUPPORTED;   /* SECURITY_NOT_PROVISIONED */
    }
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    settings_blob_t b = {0};
    b.schema = in->schema;
    memcpy(b.ezshare_ssid, in->ezshare_ssid, sizeof(b.ezshare_ssid));
    memcpy(b.ezshare_password, in->ezshare_password,
           sizeof(b.ezshare_password));
    b.allow_open_wifi = in->allow_open_wifi;
    memcpy(b.ezshare_bssid, in->ezshare_bssid, 6);
    b.bssid_lock = in->bssid_lock;
    b.http_host_ipv4 = in->http_host_ipv4;
    memcpy(b.as10_tz, in->as10_tz, sizeof(b.as10_tz));
    b.timezone_confirmed = in->timezone_confirmed;
    b.lcd_brightness = in->lcd_brightness;
    b.crc32 = blob_crc(&b);

    /* Power-loss-safe: NVS blob writes are atomic per page; commit before
     * reporting success (spec §4.2). */
    esp_err_t err = nvs_set_blob(s_cfg_handle, KEY_SETTINGS, &b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_cfg_handle);
}

esp_err_t settings_clear(void)
{
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_erase_key(s_cfg_handle, KEY_SETTINGS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    nvs_commit(s_cfg_handle);
    return err;
}

/* ---------------- authorized BLE peer ---------------- */

esp_err_t settings_load_peer(uint8_t *id_addr_type, uint8_t addr[6])
{
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[7];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(s_cfg_handle, KEY_PEER, buf, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(buf)) {
        return ESP_ERR_NVS_CORRUPT_KEY_PART;
    }
    if (id_addr_type) {
        *id_addr_type = buf[0];
    }
    memcpy(addr, buf + 1, 6);
    return ESP_OK;
}

esp_err_t settings_save_peer(uint8_t id_addr_type, const uint8_t addr[6])
{
    if (!settings_storage_protected()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[7] = { id_addr_type };
    memcpy(buf + 1, addr, 6);
    esp_err_t err = nvs_set_blob(s_cfg_handle, KEY_PEER, buf, sizeof(buf));
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg_handle);
    }
    return err;
}

esp_err_t settings_clear_peer(void)
{
    if (!s_cfg_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_erase_key(s_cfg_handle, KEY_PEER);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    nvs_commit(s_cfg_handle);
    return err;
}

bool settings_has_peer(void)
{
    uint8_t addr[6];
    return settings_load_peer(NULL, addr) == ESP_OK;
}

nvs_handle_t settings_nvs_config(void)   { return s_cfg_handle; }
nvs_handle_t settings_nvs_metadata(void) { return s_meta_handle; }

void settings_set_meta_handle(nvs_handle_t h) { s_meta_handle = h; }
