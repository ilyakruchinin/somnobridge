/* host-test stub — in-memory NVS KV store for journal tests */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int nvs_handle_t;
typedef struct { int unused; } nvs_sec_cfg_t;
#define NVS_READWRITE 0
#define ESP_ERR_NVS_NOT_FOUND     0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1103
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1104
#define ESP_ERR_NVS_INVALID_LENGTH    0x1105
#define ESP_ERR_NVS_INVALID_STATE     0x1106

/* test-controlled backing store (implemented in host_test.c) */
esp_err_t nvs_flash_secure_init_partition(const char *part, void *cfg);
esp_err_t nvs_open_from_partition(const char *part, const char *ns,
                                  int mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out,
                       size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in,
                       size_t len);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_flash_erase_partition(const char *part);
const char *esp_err_to_name(esp_err_t e);

/* corruption/failure hooks for the journal tests */
void nvs_stub_reset(void);
void nvs_stub_corrupt(const char *key);   /* flip a byte in the blob  */
void nvs_stub_fail_writes(int n);         /* next n set_blob fail     */
