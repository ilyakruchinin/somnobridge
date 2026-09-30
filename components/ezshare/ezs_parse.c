/*
 * SomnoPal — EZShare HTML <pre> listing parser (spec §11.2)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Hand-rolled, no IDF deps -> host-testable. Structural, defensive:
 * every row must conform or the whole listing is rejected.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ezs_parse.h"

/* ---------------- helpers ---------------- */

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode HTML entities (&amp; &lt; &gt; &quot; &#NN;) + %-escapes once. */
static size_t decode_entities(const char *in, size_t in_len,
                              char *out, size_t out_cap)
{
    size_t w = 0;
    for (size_t i = 0; i < in_len && w + 1 < out_cap; ) {
        char c = in[i];
        if (c == '&') {
            size_t j = i + 1;
            if (j < in_len && in[j] == '#') {
                /* &#NN; numeric entity */
                int v = 0, k = 0;
                j++;
                while (j < in_len && isdigit((unsigned char)in[j]) &&
                       k < 3) {
                    v = v * 10 + (in[j] - '0'); j++; k++;
                }
                if (j < in_len && in[j] == ';' && k > 0 && v < 128) {
                    out[w++] = (char)v;
                    i = j + 1;
                    continue;
                }
            } else if (i + 5 <= in_len && !strncmp(in + i, "&amp;", 5)) {
                out[w++] = '&'; i += 5; continue;
            } else if (i + 4 <= in_len && !strncmp(in + i, "&lt;", 4)) {
                out[w++] = '<'; i += 4; continue;
            } else if (i + 4 <= in_len && !strncmp(in + i, "&gt;", 4)) {
                out[w++] = '>'; i += 4; continue;
            } else if (i + 6 <= in_len && !strncmp(in + i, "&quot;", 6)) {
                out[w++] = '"'; i += 6; continue;
            }
        } else if (c == '%' && i + 2 < in_len &&
                   hexv(in[i+1]) >= 0 && hexv(in[i+2]) >= 0) {
            out[w++] = (char)(hexv(in[i+1]) * 16 + hexv(in[i+2]));
            i += 3;
            continue;
        }
        out[w++] = c;
        i++;
    }
    out[w] = '\0';
    return w;
}

/* True if "YYYYMMDD" is a real calendar date (spec §5.1). */
bool ezs_is_valid_day(const char *name)
{
    if (strlen(name) != 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }
    }
    int y = (name[0]-'0')*1000 + (name[1]-'0')*100 +
            (name[2]-'0')*10 + (name[3]-'0');
    int m = (name[4]-'0')*10 + (name[5]-'0');
    int d = (name[6]-'0')*10 + (name[7]-'0');
    if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    static const int mdays[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[m-1];
    if (m == 2) {
        int leap = (y%4==0 && (y%100!=0 || y%400==0));
        dim = leap ? 29 : 28;
    }
    return d <= dim;
}

/* ---------------- row scanner ---------------- */
/*
 * Row shape emitted by the actual EZShare card firmware (captured from
 * a live card, §11.2 "expected syntax comes from evidence"):
 *
 *     <ts>   <size | <DIR>>   <a href="...">name</a>
 *
 * e.g.
 *    2026- 8-14    4: 8:32         &lt;DIR&gt;   <a href="dir?dir=A:%5CDATALOG%5C20260814"> 20260814</a>
 *    2026- 9-30    5: 5: 0             20480   <a href="download?fname=X&fdir=..">X</a>
 *
 * Notes that matter: the metadata precedes the anchor (the reverse of
 * the layout the docs illustrated), the timestamp fields are space
 * padded, <DIR> arrives entity-escaped, and the link text is padded.
 * The trailing "Total Entries:" summary line carries no anchor and is
 * structure, not a row.
 */

/* Parse the metadata prefix before the anchor: timestamp + size/<DIR>. */
static ezs_err_t meta_parse(const char *m, ezs_entry_t *e)
{
    e->has_timestamp = false;
    e->timestamp_raw[0] = '\0';
    e->is_dir = false;
    e->size = 0;

    const char *dir = strstr(m, "<DIR>");        /* already decoded    */
    const char *endp = m + strlen(m);
    while (endp > m && isspace((unsigned char)endp[-1])) {
        endp--;
    }
    const char *tok = endp;                      /* last token         */
    while (tok > m && !isspace((unsigned char)tok[-1])) {
        tok--;
    }

    size_t tl;
    if (dir) {
        e->is_dir = true;
        tl = (size_t)(dir - m);
    } else {
        /* size token: digits with an optional K/M/G[..B] suffix — the
         * card rounds file sizes for display ("1KB", "72KB")          */
        uint64_t sz = 0;
        int nd = 0;
        const char *q = tok;
        while (q < endp && isdigit((unsigned char)*q)) {
            sz = sz * 10 + (uint64_t)(*q - '0');
            if (++nd > 19) {
                return EZS_ERR_MALFORMED;        /* > uint64 range     */
            }
            q++;
        }
        if (nd == 0) {
            return EZS_ERR_MALFORMED;
        }
        unsigned shift = 0;
        if (q < endp) {
            char u = (char)toupper((unsigned char)*q);
            if (u == 'K') {
                shift = 10;
            } else if (u == 'M') {
                shift = 20;
            } else if (u == 'G') {
                shift = 30;
            } else {
                return EZS_ERR_MALFORMED;
            }
            q++;
            if (q < endp && toupper((unsigned char)*q) == 'B') {
                q++;
            }
        }
        if (q != endp || sz > (UINT64_MAX >> shift)) {
            return EZS_ERR_MALFORMED;
        }
        e->size = sz << shift;
        tl = (size_t)(tok - m);
    }

    while (tl > 0 && isspace((unsigned char)m[tl - 1])) {
        tl--;
    }
    if (tl > 0) {
        size_t n = tl < sizeof(e->timestamp_raw) - 1
                     ? tl : sizeof(e->timestamp_raw) - 1;
        memcpy(e->timestamp_raw, m, n);
        e->timestamp_raw[n] = '\0';
        e->has_timestamp = true;                 /* raw text (§11.2)   */
    }
    return EZS_OK;
}

/* Resolve an href to its request path. Relative links are used as-is;
 * absolute ones are accepted only when the authority is an IPv4
 * literal (the card sits on the local AP and links itself that way).
 * Anything with a hostname/userinfo is external -> NULL (§11.1).      */
static const char *href_path(const char *href)
{
    const char *p = href;
    if (!strncmp(p, "http://", 7)) {
        p += 7;
    } else if (!strncmp(p, "https://", 8)) {
        p += 8;
    } else {
        return href;                 /* relative: path already       */
    }
    int oct = 0, dig = 0;
    const char *q = p;
    for (; *q && *q != '/' && *q != ':'; q++) {
        if (*q == '.') {
            if (!dig) {
                return NULL;
            }
            oct++;
            dig = 0;
        } else if (isdigit((unsigned char)*q)) {
            dig++;
        } else {
            return NULL;             /* hostname / userinfo -> out   */
        }
    }
    if (oct != 3 || !dig) {
        return NULL;
    }
    if (*q == ':') {                 /* optional port                */
        q++;
        while (isdigit((unsigned char)*q)) {
            q++;
        }
    }
    return *q == '/' ? q : NULL;
}

/* classify an href: dir?dir=... / download?file|fname=... / other */
static bool href_is_dir(const char *href)
{
    const char *p = href_path(href);
    return p && (!strncmp(p, "dir?", 4) || !strncmp(p, "/dir?", 5));
}
static bool href_is_download(const char *href)
{
    const char *p = href_path(href);
    return p && (!strncmp(p, "download?", 9) ||
                 !strncmp(p, "/download?", 10));
}

/* Extract fname= value from a download href (already entity-decoded). */
static bool href_fname(const char *href, char *out, size_t cap)
{
    const char *f = strstr(href, "fname=");
    if (!f) {
        return false;
    }
    f += 6;
    const char *a = strchr(f, '&');
    size_t n = a ? (size_t)(a - f) : strlen(f);
    if (n == 0 || n >= cap) {
        return false;
    }
    memcpy(out, f, n);
    out[n] = '\0';
    return true;
}


/* Locate the <pre> listing block; returns its start, sets end.
 * *end_out is NULL when the closing tag is missing — a truncated body
 * must be rejected, not silently accepted as a short listing (§11.2). */
static const char *find_pre(const char *body, const char **end_out)
{
    const char *pre = strstr(body, "<pre>");
    if (!pre) {
        pre = strstr(body, "<PRE>");
    }
    if (!pre) {
        return NULL;
    }
    pre += 5;
    const char *end = strstr(pre, "</pre>");
    if (!end) {
        end = strstr(pre, "</PRE>");
    }
    *end_out = end;
    return pre;
}

/* Extract href="..." value; returns length, or -1. */
static int href_value(const char *p, const char *end,
                      char *out, size_t cap)
{
    const char *a = strstr(p, "href=");
    if (!a || a >= end) {
        return -1;
    }
    a += 5;
    char q = *a;
    if (q == '"' || q == '\'') {
        a++;
    } else {
        q = ' ';                     /* unquoted attribute        */
    }
    const char *b = a;
    while (b < end && *b != q &&
           !(q == ' ' && (isspace((unsigned char)*b) || *b == '>'))) {
        b++;
    }
    size_t n = b - a;
    if (n + 1 > cap) {
        return -1;
    }
    decode_entities(a, n, out, cap);
    return (int)strlen(out);
}

/* Link text between '>' and '<' after the href tag close. */
static const char *link_text(const char *p, const char *end,
                             char *out, size_t cap)
{
    const char *gt = memchr(p, '>', end - p);
    if (!gt) {
        return NULL;
    }
    gt++;
    const char *lt = memchr(gt, '<', end - gt);
    if (!lt) {
        return NULL;
    }
    decode_entities(gt, lt - gt, out, cap);
    /* this card pads link text with spaces ("> 20260814</a>") — trim
     * before use or dir names arrive with a leading space            */
    size_t n = strlen(out);
    size_t s = 0;
    while (s < n && isspace((unsigned char)out[s])) {
        s++;
    }
    while (n > s && isspace((unsigned char)out[n - 1])) {
        n--;
    }
    if (s) {
        memmove(out, out + s, n - s);
    }
    out[n - s] = '\0';
    /* strip trailing slash from dir display name */
    n -= s;
    if (n > 0 && out[n-1] == '/') {
        out[n-1] = '\0';
    }
    return lt;                       /* resume scan after '<'      */
}

ezs_err_t ezs_parse_dir(const char *body,
                        ezs_entry_t *entries, int *n_out)
{
    *n_out = 0;
    if (!body) {
        return EZS_ERR_MALFORMED;
    }
    const char *end;
    const char *pre = find_pre(body, &end);
    if (!pre) {
        return EZS_ERR_NOT_LISTING;
    }
    if (!end) {
        return EZS_ERR_TRUNCATED;    /* no </pre> -> partial response */
    }

    const char *p = pre;
    while (p < end) {
        /* rows are line-oriented on this firmware */
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *line = p;
        p = nl ? nl + 1 : end;

        while (line < le && isspace((unsigned char)*line)) {
            line++;
        }
        while (le > line && isspace((unsigned char)le[-1])) {
            le--;
        }

        /* locate the anchor on this line; lines without one are page
         * structure (e.g. the "Total Entries:" summary), not rows   */
        const char *a = NULL;
        for (const char *q = line; q + 2 < le; q++) {
            if (q[0] == '<' && (q[1] == 'a' || q[1] == 'A') &&
                isspace((unsigned char)q[2])) {
                a = q;
                break;
            }
        }
        if (!a) {
            continue;
        }

        char href[256], lname[EZS_NAME_MAX];
        if (href_value(a, le, href, sizeof(href)) < 0) {
            return EZS_ERR_MALFORMED;
        }
        if (!link_text(a, le, lname, sizeof(lname))) {
            return EZS_ERR_MALFORMED;
        }

        char meta[128];
        decode_entities(line, (size_t)(a - line), meta, sizeof(meta));

        ezs_entry_t e;
        memset(&e, 0, sizeof(e));
        ezs_err_t r = meta_parse(meta, &e);
        if (r != EZS_OK) {
            return r;
        }

        /* navigation rows */
        if (!strcmp(lname, ".") || !strcmp(lname, "..")) {
            continue;
        }

        bool is_dir = href_is_dir(href);
        bool is_file = href_is_download(href);
        if (!is_dir && !is_file) {
            /* absolute/external link -> rejected whole listing */
            return EZS_ERR_MALFORMED;
        }
        e.is_dir = is_dir;
        if (is_file) {
            /* the card links files as ?file=<8.3 short name> and puts
             * the exact alias in the link text, so fall back to that;
             * an explicit fname= wins when a variant provides one    */
            if (!href_fname(href, e.name, sizeof(e.name))) {
                if (!lname[0]) {
                    return EZS_ERR_MALFORMED;
                }
                strncpy(e.name, lname, sizeof(e.name) - 1);
            }
        } else {
            strncpy(e.name, lname, sizeof(e.name) - 1);
        }

        if (*n_out >= EZS_ENTRIES_MAX) {
            return EZS_ERR_LIMIT;
        }
        entries[(*n_out)++] = e;
    }
    return EZS_OK;
}
