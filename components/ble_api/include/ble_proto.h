/*
 * SomnoBridge — BLE protocol framing (spec §10.1) — host-testable
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- UUIDs (spec §10.1) ---- */
/* 8ba7d100-6e4b-4bb4-9f1d-7306766e6f50 / ...01.. / ...02.. */
#define BLEP_UUID_SERVICE   { 0x50,0x6f,0x6e,0x73,0x76,0x73,0x9f,0x1d, \
                              0x4b,0x4b,0x4b,0x6e,0x4b,0x73,0x06,0x76, \
                              0x66,0x6e,0x50 }
/* NimBLE stores 128-bit UUIDs little-endian on air; constants above are
 * the on-air byte order used with ble_uuid128_t. */

typedef enum {
    BLEP_JSON_REQUEST   = 1,
    BLEP_JSON_RESPONSE  = 2,
    BLEP_FILE_BYTES     = 3,
    BLEP_ACK            = 4,
    BLEP_ABORT          = 5,
    BLEP_TIME_REQUEST   = 6,
    BLEP_TIME_REPLY     = 7,
    BLEP_TIME_COMMIT    = 8,
    BLEP_EVIDENCE_BYTES = 9,
    BLEP_STREAM_DATA    = 10,   /* compact data frame (update7 §10.1) */
} blep_kind_t;

#define BLEP_FLAG_START 0x0001
#define BLEP_FLAG_END   0x0002

/* 16-byte little-endian message header (spec §10.1) */
typedef struct __attribute__((packed)) {
    uint8_t  version;      /* =1   */
    uint8_t  kind;
    uint16_t flags;
    uint32_t request_id;
    uint64_t offset;       /* stream byte offset / next-acked       */
} blep_hdr_t;

_Static_assert(sizeof(blep_hdr_t) == 16, "header wire size");

/* JSON envelope (spec §10.1):
 *   request : {"op":"NAME","args":{...}}
 *   response: {"op":"NAME","ok":true,"result":{...}}
 *             {"op":"NAME","ok":false,"error":{"code":"X",...}}
 */
#define BLEP_JSON_MAX   4096
#define BLEP_REASM_MS   30000

typedef struct {
    uint8_t  buf[BLEP_JSON_MAX];
    uint32_t len;
    uint32_t expected;      /* running offset for fragments         */
    uint32_t request_id;
    bool     open;
    int64_t  deadline_ms;   /* esp_timer /1000 — shim for host       */
} blep_reasm_t;

void  blep_reasm_init(blep_reasm_t *r);

/**
 * Feed one inbound Control write (full ATT value).
 * Returns true when a complete JSON document is ready in r->buf;
 * *rid_out receives the request_id.
 * Errors -> false and r closed (caller may send error response).
 */
bool blep_reasm_feed(blep_reasm_t *r, const uint8_t *data, uint16_t len,
                     int64_t now_ms, uint32_t *rid_out);

/** Build one fragment (header+payload slice) of an outbound stream. */
uint16_t blep_build_frame(uint8_t *out, uint16_t mtu_payload,
                          blep_kind_t kind, uint32_t request_id,
                          uint64_t offset,
                          const uint8_t *payload, uint16_t plen,
                          bool first, bool last);

/* Compact stream-data frame (update7 §10.1): 4-byte header
 *   ver:u8 | kind:u8 | seq:u16 | payload[N]
 * replaces the 16-byte blep_hdr_t on the Data characteristic for bulk
 * stream bytes. seq counts frames mod 65536; the receiver derives byte
 * offsets from in-order delivery, so there is no per-frame offset and
 * no request_id — streams are exclusive on the link and bounded by the
 * rid-keyed STREAM_BEGIN/STREAM_END JSON indications.               */
#define BLEP_CDATA_HDR_LEN 4

uint16_t blep_build_cframe(uint8_t *out, uint16_t out_cap,
                           uint16_t seq,
                           const uint8_t *payload, uint16_t plen);

/* ---- op names (spec §10.2) ---- */
#define OP_GET_STATUS        "GET_STATUS"
#define OP_GET_CATALOG_STATUS "GET_CATALOG_STATUS"
#define OP_LIST_RECENT_DAYS  "LIST_RECENT_DAYS"
#define OP_LIST_DAY          "LIST_DAY"
#define OP_LIST_DIRECTORY    "LIST_DIRECTORY"
#define OP_GET_FILE          "GET_FILE"
#define OP_PREPARE_DAYS      "PREPARE_DAYS"
#define OP_STREAM_TEST       "STREAM_TEST"
#define OP_GET_DRIFT         "GET_DRIFT"
#define OP_GET_EVIDENCE      "GET_EVIDENCE"
/* Provisioning over BLE (§4 alternative to the setup-AP portal): the
 * SomnoTrace portal mirrors these so the card can be configured in the
 * same flow as pairing, without AP mode. */
#define OP_GET_CONFIG        "GET_CONFIG"
#define OP_SET_CONFIG        "SET_CONFIG"
#define OP_WIFI_SCAN         "WIFI_SCAN"
#define OP_EZSHARE_CHECK     "EZSHARE_CHECK"
#define OP_EZSHARE_ADMIN     "EZSHARE_ADMIN"
#define OP_CARD_GET          "CARD_GET"     /* temporary diagnostic    */

/* ---- error codes (spec §10.2) ---- */
#define EC_BAD_REQUEST        "BAD_REQUEST"
#define EC_UNSUPPORTED_OP     "UNSUPPORTED_OP"
#define EC_BUSY               "BUSY"
#define EC_NOT_FOUND          "NOT_FOUND"
#define EC_NOT_SCANNED        "NOT_SCANNED"
#define EC_SCANNING           "SCANNING"
#define EC_CATALOG_INCOMPLETE "CATALOG_INCOMPLETE"
#define EC_STALE              "STALE"
#define EC_THERAPY_ACTIVE     "THERAPY_ACTIVE"
#define EC_THERAPY_UNKNOWN    "THERAPY_UNKNOWN"
#define EC_SETUP_ACTIVE       "SETUP_ACTIVE"
#define EC_SIZE_MISMATCH      "SIZE_MISMATCH"
#define EC_SOURCE_CHANGED     "SOURCE_CHANGED"
#define EC_PATH_INVALID       "PATH_INVALID"
#define EC_HTTP_ERROR         "HTTP_ERROR"
#define EC_PARSE_ERROR        "PARSE_ERROR"
#define EC_TIMEOUT            "TIMEOUT"
#define EC_RESOURCE_LIMIT     "RESOURCE_LIMIT"
#define EC_BUDGET_EXHAUSTED   "BUDGET_EXHAUSTED"
#define EC_ABORTED            "ABORTED"
#define EC_LINK_LOST          "LINK_LOST"
#define EC_METADATA_UNAVAILABLE "METADATA_UNAVAILABLE"
#define EC_STORAGE_ERROR      "STORAGE_ERROR"
#define EC_SECURITY_NOT_PROVISIONED "SECURITY_NOT_PROVISIONED"

#ifdef __cplusplus
}
#endif
