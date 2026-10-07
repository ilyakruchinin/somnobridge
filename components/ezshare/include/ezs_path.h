/*
 * SomnoBridge — EZShare path canonicalization (host-testable, spec §11.1)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EZP_DIR_MAX   240
#define EZP_NAME_MAX  96

typedef enum {
    EZP_OK = 0,
    EZP_ERR_EMPTY,
    EZP_ERR_LEN,
    EZP_ERR_DRIVE,        /* not A:                               */
    EZP_ERR_COMPONENT,    /* empty / "." / ".." / bad chars       */
    EZP_ERR_TRAVERSAL,    /* % or encoded-input syntax            */
    EZP_ERR_CHAR,         /* FAT-invalid or control chars         */
    EZP_ERR_NAME,         /* bad filename component               */
} ezp_err_t;

/**
 * Canonicalize a directory into "A:\comp\..." form.
 * Accepts: "A:", "A:\", "a:/x/y", mixed slashes/case drive.
 * Rejects: other drives, "."/"..", empty internal components, '%',
 *          control chars, FAT-invalid chars, trailing dot/space comps.
 */
ezp_err_t ezp_canon_dir(const char *in, char *out, size_t cap);

/**
 * Canonicalize a client-supplied (API) directory path. Everything
 * ezp_canon_dir accepts PLUS root-relative forms — "/DATALOG/20240101",
 * "\DATALOG", "DATALOG/20240101" — which all mean the card root (= A:\).
 * An explicit drive letter is still validated (only A:/a: passes).
 * The card-agnostic wire form is "/"; the canonical A:\ spelling stays
 * an internal detail of the EZShare adapter.
 */
ezp_err_t ezp_logical_to_canon(const char *in, char *out, size_t cap);

/**
 * Render a canonical dir for API output: "A:\X\Y" -> "/X/Y",
 * "A:\" -> "/". Returns bytes written, 0 on non-canonical input.
 */
size_t ezp_canon_to_logical(const char *canon, char *out, size_t cap);

/** Validate a single filename component (8.3 alias or LFN). */
ezp_err_t ezp_validate_name(const char *name);

/** Case-insensitive: canonical dir == A:\DATALOG or a descendant. */
bool ezp_is_datalog(const char *canon_dir);

/** Canonical dir is exactly A:\DATALOG\YYYYMMDD (valid day). */
bool ezp_is_datalog_day(const char *canon_dir);

/** Number of path components after the drive (A:\ -> 0). */
int ezp_component_count(const char *canon_dir);

/** Extract the day name (YYYYMMDD) from A:\DATALOG\YYYYMMDD -> out[9]. */
bool ezp_datalog_day(const char *canon_dir, char day[9]);

/** Percent-encode one query value, once (everything but unreserved). */
size_t ezp_urlencode(const char *in, char *out, size_t cap);

int ezp_build_card_url(char *out, size_t cap, uint32_t card_ip,
                       const char *endpoint,
                       const char *q1_name, const char *q1_val,
                       const char *q2_name, const char *q2_val);

#ifdef __cplusplus
}
#endif
