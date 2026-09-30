/*
 * SomnoPal — minimal EDF fixed-header parser (spec §8.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Reads only the fixed header + the per-signal "samples per record"
 * array — never touches patient fields' contents beyond skipping them.
 * Pure C; host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t header_bytes;       /* H: value of the bytes-in-header field */
    uint32_t record_bytes;       /* R: 2 * sum(samples per record)        */
    uint32_t record_duration_s;  /* D_record, whole seconds only          */
    uint32_t n_signals;          /* ns                                    */
    int64_t  n_records;          /* -1 when EDF declares "continuous"     */
    int64_t  start_local_s;      /* startdate+starttime as AS10-local
                                    seconds since 2000-01-01 (rough; the
                                    estimator treats it as a hint only)   */
} edf_hdr_t;

/** Parse `len` bytes of a file prefix. Fails cleanly on truncation or
 * implausible fields — callers treat failure as "header unknown".   */
bool edf_hdr_parse(const uint8_t *buf, size_t len, edf_hdr_t *out);

#ifdef __cplusplus
}
#endif
