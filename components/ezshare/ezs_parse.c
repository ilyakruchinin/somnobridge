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
 * Row shapes we accept inside <pre> (from SomnoTrace's files-over-http.md):
 *   dir : <a href="dir?dir=A:\DATALOG\20260713">20260713</a>  ts  <DIR>
 *   file: <a href="download?fname=X&fdir=Y">NAME</a>  ts  SIZE
 * Timestamps may be "-" or absent entirely.
 */

typedef struct {
    const char *p;        /* cursor into body                      */
    bool        eof;      /* ran off the end                       */
} scan_t;

/* Find next '<a href="..."' inside pre body; returns link text span. */
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
    if (!end) {
        end = body + strlen(body);   /* tolerate missing close tag */
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
    /* strip trailing slash from dir display name */
    size_t n = strlen(out);
    if (n > 0 && out[n-1] == '/') {
        out[n-1] = '\0';
    }
    return lt;                       /* resume scan after '<'      */
}

/* Parse trailing timestamp + size after link text end. */
static ezs_err_t row_tail(const char *p, const char *end,
                          ezs_entry_t *e, const char **next)
{
    /* skip whitespace to the rest of the row */
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    /* timestamp token(s): "YYYY-MM-DD HH:MM:SS" or "-" or digits.
     * Take the first space-delimited token, then, if it looks like a
     * date, optionally the following time token. */
    const char *tok = p;
    while (p < end && !isspace((unsigned char)*p) && *p != '<') {
        p++;
    }
    size_t tl = p - tok;
    e->has_timestamp = false;
    e->timestamp_raw[0] = '\0';

    if (tl >= 8) {                   /* looks like a date          */
        char tmp[EZS_STAMP_MAX];
        size_t n = tl < sizeof(tmp)-1 ? tl : sizeof(tmp)-1;
        memcpy(tmp, tok, n); tmp[n] = '\0';
        /* append " HH:MM:SS" if the next token looks like a time   */
        const char *q = p;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q + 8 <= end && isdigit((unsigned char)q[0]) &&
            q[2] == ':' && q[5] == ':') {
            size_t m = n + 1 + 8;
            if (m < sizeof(tmp)) {
                tmp[n] = ' ';
                memcpy(tmp + n + 1, q, 8);
                tmp[m] = '\0';
                q += 8;
            }
            p = q;
        }
        strncpy(e->timestamp_raw, tmp, sizeof(e->timestamp_raw)-1);
        e->timestamp_raw[sizeof(e->timestamp_raw)-1] = '\0';
        e->has_timestamp = true;
    }

    /* size or <DIR> */
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '<') {
        const char *gt = memchr(p, '>', end - p);
        if (!gt) {
            return EZS_ERR_TRUNCATED;
        }
        /* <DIR> marker */
        if (gt - p >= 4 && !strncmp(p, "<DIR>", 5)) {
            e->is_dir = true;
            e->size = 0;
            p = gt + 1;
            *next = p;
            return EZS_OK;
        }
        /* other tag mid-row -> malformed */
        return EZS_ERR_MALFORMED;
    }
    /* digits = size */
    uint64_t sz = 0;
    int nd = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        sz = sz * 10 + (*p - '0');
        if (++nd > 19) {
            return EZS_ERR_MALFORMED;   /* > uint64 range          */
        }
        p++;
    }
    if (nd == 0) {
        return EZS_ERR_MALFORMED;
    }
    e->is_dir = false;
    e->size = sz;
    *next = p;
    return EZS_OK;
}

/* classify an href: dir?dir=... / download?fname=..&fdir=.. / other */
static bool href_is_dir(const char *href)
{
    return strncmp(href, "dir?", 4) == 0 ||
           strncmp(href, "/dir?", 5) == 0;
}
static bool href_is_download(const char *href)
{
    return strncmp(href, "download?", 9) == 0 ||
           strncmp(href, "/download?", 10) == 0;
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

    const char *p = pre;
    while (p < end) {
        /* advance to next anchor tag */
        const char *a = strstr(p, "<a ");
        if (!a || a >= end) {
            /* only whitespace may follow the last row */
            while (p < end && isspace((unsigned char)*p)) {
                p++;
            }
            if (p < end) {
                return EZS_ERR_MALFORMED;
            }
            break;
        }
        p = a;

        char href[256], lname[EZS_NAME_MAX];
        if (href_value(p, end, href, sizeof(href)) < 0) {
            return EZS_ERR_MALFORMED;
        }
        const char *lt = link_text(p, end, lname, sizeof(lname));
        if (!lt) {
            return EZS_ERR_MALFORMED;
        }
        /* lt points at the '<' of the closing </a>; the timestamp/size
         * tail begins after that tag's '>'                           */
        const char *close_end = memchr(lt, '>', end - lt);
        if (!close_end) {
            return EZS_ERR_TRUNCATED;
        }

        ezs_entry_t e;
        memset(&e, 0, sizeof(e));
        const char *next;
        ezs_err_t r = row_tail(close_end + 1, end, &e, &next);
        if (r != EZS_OK) {
            return r;
        }
        p = next;

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
            if (!href_fname(href, e.name, sizeof(e.name))) {
                return EZS_ERR_MALFORMED;
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
