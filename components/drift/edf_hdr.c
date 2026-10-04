/*
 * SomnoBridge — minimal EDF fixed-header parser (spec §8.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <string.h>

#include "edf_hdr.h"

/* EDF fixed header: 256 bytes, ASCII fields (EDF spec offsets).     */
static int64_t field_i(const uint8_t *p, size_t n, bool *ok)
{
    int64_t v = 0;
    bool any = false, neg = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] >= '0' && p[i] <= '9') {
            v = v * 10 + (p[i] - '0');
            any = true;
        } else if (p[i] == '-') {
            if (neg) { *ok = false; return 0; }
            neg = true;
        } else if (p[i] != ' ') {
            *ok = false;
            return 0;
        }
    }
    if (!any) *ok = false;
    return neg ? -v : v;
}

static bool field_f(const uint8_t *p, size_t n, double *out)
{
    char tmp[16];
    if (n >= sizeof(tmp)) return false;
    size_t w = 0;
    bool started = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ' ') {
            if (started) break;      /* trailing pad ends the field   */
            continue;                /* skip leading pad              */
        }
        started = true;
        tmp[w++] = (char)p[i];
    }
    tmp[w] = '\0';
    if (!w) return false;
    /* tiny strtod: digits [ . digits ] — EDF durations are plain      */
    double v = 0, frac = 0, scale = 1;
    bool dot = false, any = false;
    for (size_t i = 0; i < w; i++) {
        if (tmp[i] == '.') {
            if (dot) return false;
            dot = true;
        } else if (tmp[i] >= '0' && tmp[i] <= '9') {
            any = true;
            if (!dot) v = v * 10 + (tmp[i] - '0');
            else { scale *= 10; frac = frac * 10 + (tmp[i] - '0'); }
        } else {
            return false;
        }
    }
    if (!any) return false;
    *out = v + frac / scale;
    return true;
}

static bool two(const uint8_t *p, int *out)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
    *out = (p[0] - '0') * 10 + (p[1] - '0');
    return true;
}

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int s_md[12] =
    {31,28,31,30,31,30,31,31,30,31,30,31};

/* dd.mm.yy + hh.mm.ss -> seconds since 2000-01-01 (yy pivots at 70). */
static bool local_seconds(const uint8_t *d, const uint8_t *t,
                          int64_t *out)
{
    int dd, mm, yy, hh, mi, ss;
    if (d[2] != '.' || d[5] != '.' ||
        !two(d, &dd) || !two(d + 3, &mm) || !two(d + 6, &yy)) {
        return false;
    }
    if (t[2] != '.' || t[5] != '.' ||
        !two(t, &hh) || !two(t + 3, &mi) || !two(t + 6, &ss)) {
        return false;
    }
    int year = 2000 + yy;
    if (yy >= 70) year = 1900 + yy;   /* EDF y2k convention            */
    if (mm < 1 || mm > 12 || dd < 1 || hh > 23 || mi > 59 || ss > 60) {
        return false;
    }
    int mdays = s_md[mm - 1] + (mm == 2 && leap(year) ? 1 : 0);
    if (dd > mdays) return false;

    int64_t days = 0;
    for (int y = 2000; y < year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < mm; m++)
        days += s_md[m - 1] + (m == 2 && leap(year) ? 1 : 0);
    days += dd - 1;
    *out = days * 86400 + hh * 3600 + mi * 60 + ss;
    return true;
}

bool edf_hdr_parse(const uint8_t *buf, size_t len, edf_hdr_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!buf || len < 256) return false;

    bool ok = true;
    /* version field must be a single "0" (offset 0, 8 bytes)         */
    int64_t ver = field_i(buf, 8, &ok);
    if (!ok || ver != 0) return false;

    int64_t hdr_bytes = field_i(buf + 184, 8, &ok);
    int64_t n_records = field_i(buf + 236, 8, &ok);   /* -1 allowed    */
    double  rec_dur_d = 0;
    bool    dur_ok = field_f(buf + 244, 8, &rec_dur_d);
    int64_t n_signals = field_i(buf + 252, 4, &ok);
    if (!ok || !dur_ok) return false;
    if (n_signals < 1 || n_signals > 64) return false;
    if (hdr_bytes != 256 * (1 + n_signals)) return false;
    if ((size_t)hdr_bytes > len) return false;   /* need the samples   */

    /* samples-per-record array: 8 bytes per signal at
     * 256 + (16+80+8+8+8+8+8+80)*ns = 256 + 216*ns                   */
    size_t sp_off = 256 + 216 * (size_t)n_signals;
    uint64_t rec_bytes = 0;
    for (int64_t i = 0; i < n_signals; i++) {
        bool sok = true;
        int64_t sp = field_i(buf + sp_off + (size_t)i * 8, 8, &sok);
        if (!sok || sp < 0 || sp > (1 << 20)) return false;
        rec_bytes += (uint64_t)sp * 2;
    }
    if (!rec_bytes || rec_bytes > (1u << 24)) return false;
    if (rec_dur_d < 1.0 || rec_dur_d != (double)(uint32_t)rec_dur_d) {
        return false;   /* estimator only supports whole-second recs   */
    }

    int64_t start_s = 0;
    if (!local_seconds(buf + 168, buf + 176, &start_s)) return false;

    out->header_bytes = (uint32_t)hdr_bytes;
    out->record_bytes = (uint32_t)rec_bytes;
    out->record_duration_s = (uint32_t)rec_dur_d;
    out->n_signals = (uint32_t)n_signals;
    out->n_records = n_records;
    out->start_local_s = start_s;
    return true;
}
