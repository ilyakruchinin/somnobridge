/*
 * SomnoBridge — EZShare HTML <pre> listing parser (host-testable, spec §11.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Pure C — no ESP-IDF includes. Used both on-device and by host tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EZS_NAME_MAX     96    /* 8.3 alias + UTF-8 headroom          */
#define EZS_STAMP_MAX    24    /* raw "YYYY-MM-DD HH:MM:SS" text      */
#define EZS_ENTRIES_MAX  512   /* per-listing cap, fits scratch        */

typedef struct {
    char     name[EZS_NAME_MAX];          /* exact download alias      */
    uint64_t size;                        /* bytes; 0 for dirs         */
    uint8_t  size_unit_shift;             /* log2 display unit:        */
                                          /* 0=bytes 10=K 20=M 30=G  */
    bool     is_dir;
    bool     has_timestamp;               /* false -> timestamp_raw="" */
    char     timestamp_raw[EZS_STAMP_MAX];/* verbatim card text        */
    char     dl_name[16];                 /* 8.3 basename from file=,  */
                                          /* "" for the fname= form  */
} ezs_entry_t;

/* Listed sizes are display-rounded: listed = ceil(exact/unit) (U9/F3).
 * True iff `exact` falls inside the rounding envelope of `listed`:
 * exact <= listed < exact + unit. unit==1 (shift 0) means exact
 * equality — the card prints raw bytes without a suffix.            */
bool ezs_size_within_listing(uint64_t listed, uint8_t size_unit_shift,
                             uint64_t exact);

typedef enum {
    EZS_OK = 0,
    EZS_ERR_TRUNCATED,     /* buffer ended mid-row/mid-tag             */
    EZS_ERR_MALFORMED,     /* structure/row/link violates grammar      */
    EZS_ERR_LIMIT,         /* entries/body/row over cap                */
    EZS_ERR_NOT_LISTING,   /* login page / no <pre> block found        */
} ezs_err_t;

/**
 * Parse a complete EZShare /dir?dir=... response body.
 *
 * @param body       NUL-terminated HTML text (already fully received)
 * @param entries    caller buffer, EZS_ENTRIES_MAX capacity
 * @param n_out      entries written
 * @param changed    optional: set when content differs from `prev`
 *                   (caller compares; NULL = treat as unknown)
 *
 * Grammar (observed card): rows inside <pre> of the form
 *   <a href="download?fname=NAME&fdir=DIR">NAME</a>  timestamp  size
 *   <a href="dir?dir=DIR">NAME/</a>                  timestamp  <DIR>
 * with HTML entities in names and whitespace variation tolerated.
 * `.`/ `..` navigation rows are skipped; any unrecognised non-empty row
 * is an error (never silently skipped -> no false "complete" listings).
 */
ezs_err_t ezs_parse_dir(const char *body,
                        ezs_entry_t *entries, int *n_out);
bool ezs_find_download_file(const char *body, const char *directory,
                            const char *name, char *file, size_t file_cap,
                            char *size_token, size_t token_cap);

/** True if the row looks like a valid day folder name (YYYYMMDD, real date). */
bool ezs_is_valid_day(const char *name);

#ifdef __cplusplus
}
#endif
