/*
 * SomnoBridge — EZShare path canonicalization (spec §11.1)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Pure C, host-testable. Structured strings in, canonical A:\ path out —
 * never a URL. The HTTP adapter builds the single allowed endpoint URLs
 * itself; client-supplied traversal is impossible.
 */
#include <ctype.h>
#include <string.h>

#include "ezs_path.h"

/* FAT-invalid in a component, plus ':' (drive confusion), '%'
 * (encoded-input syntax), and control chars. */
static bool bad_char(char c)
{
    const char *bad = "<>:\"|?*";
    unsigned char u = (unsigned char)c;
    if (u < 0x20 || u == 0x7F || c == '%' || strchr(bad, c)) {
        return true;
    }
    return false;
}

/* One component: non-empty, no bad chars, not "." or "..", no trailing
 * dot or space. */
static bool comp_ok(const char *s, size_t n)
{
    if (n == 0 || n > EZP_NAME_MAX) {
        return false;
    }
    if ((n == 1 && s[0] == '.') ||
        (n == 2 && s[0] == '.' && s[1] == '.')) {
        return false;
    }
    if (s[n-1] == '.' || s[n-1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (bad_char(s[i])) {
            return false;
        }
    }
    return true;
}

ezp_err_t ezp_canon_dir(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap < 4) {
        return EZP_ERR_LEN;
    }
    size_t n = strlen(in);
    if (n == 0) {
        return EZP_ERR_EMPTY;
    }
    if (n > EZP_DIR_MAX) {
        return EZP_ERR_LEN;
    }
    /* drive */
    if (!((in[0] == 'A' || in[0] == 'a') && in[1] == ':')) {
        return EZP_ERR_DRIVE;
    }
    size_t w = 0;
    out[w++] = 'A';
    out[w++] = ':';

    /* skip leading separators -> root "A:\" */
    size_t i = 2;
    while (i < n && (in[i] == '\\' || in[i] == '/')) {
        i++;
    }
    if (i >= n) {
        out[w++] = '\\';
        out[w] = '\0';
        return EZP_OK;
    }

    /* components */
    while (i <= n) {
        size_t j = i;
        while (j < n && in[j] != '\\' && in[j] != '/') {
            j++;
        }
        if (!comp_ok(in + i, j - i)) {
            return (j - i == 0) ? EZP_ERR_COMPONENT :
                   EZP_ERR_COMPONENT;
        }
        if (w + 1 + (j - i) >= cap || w + 1 + (j - i) > EZP_DIR_MAX + 1) {
            return EZP_ERR_LEN;
        }
        out[w++] = '\\';
        memcpy(out + w, in + i, j - i);
        w += j - i;
        i = j + 1;
        if (i > n) {
            break;
        }
        /* consecutive separators -> empty component when not at end   */
        if (i == n) {
            break;                  /* trailing sep: tolerated      */
        }
        if (in[i] == '\\' || in[i] == '/') {
            return EZP_ERR_COMPONENT;
        }
    }
    out[w] = '\0';
    return EZP_OK;
}

ezp_err_t ezp_logical_to_canon(const char *in, char *out, size_t cap)
{
    if (!in || in[0] == '\0') {
        return EZP_ERR_EMPTY;
    }
    /* An explicit drive prefix ("X:...") is validated as usual —
     * only A:/a: will pass. Anything else is root-relative.      */
    if (isalpha((unsigned char)in[0]) && in[1] == ':') {
        return ezp_canon_dir(in, out, cap);
    }
    size_t n = strlen(in);
    if (n + 2 > EZP_DIR_MAX) {
        return EZP_ERR_LEN;
    }
    char tmp[2 + EZP_DIR_MAX + 1];
    tmp[0] = 'A';
    tmp[1] = ':';
    memcpy(tmp + 2, in, n + 1);         /* e.g. "A:/DATALOG/x"   */
    return ezp_canon_dir(tmp, out, cap);
}

size_t ezp_canon_to_logical(const char *canon, char *out, size_t cap)
{
    if (!canon || !out || cap < 2 ||
        strncmp(canon, "A:\\", 3) != 0) {
        if (out && cap) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t w = 0;
    for (const char *p = canon + 2; *p && w + 1 < cap; p++) {
        out[w++] = (*p == '\\' || *p == '/') ? '/' : *p;
    }
    out[w] = '\0';
    return w;
}

ezp_err_t ezp_validate_name(const char *name)
{
    if (!name) {
        return EZP_ERR_NAME;
    }
    size_t n = strlen(name);
    if (n == 0 || n > EZP_NAME_MAX) {
        return EZP_ERR_NAME;
    }
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        return EZP_ERR_NAME;
    }
    if (!comp_ok(name, n)) {
        return EZP_ERR_NAME;
    }
    return EZP_OK;
}

/* case-insensitive compare of a span against literal */
static bool ci_eq(const char *s, const char *lit)
{
    while (*lit) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*lit)) {
            return false;
        }
        s++; lit++;
    }
    return *s == '\0' || true;   /* caller checks following char   */
}

bool ezp_is_datalog(const char *canon_dir)
{
    if (!canon_dir) {
        return false;
    }
    if (strncmp(canon_dir, "A:\\", 3) != 0) {
        return false;
    }
    const char *p = canon_dir + 3;
    if (!ci_eq(p, "DATALOG")) {
        return false;
    }
    /* matched "DATALOG" prefix (ci_eq returns true at NUL too) */
    p += 7;
    return *p == '\0' || *p == '\\';
}

bool ezp_is_datalog_day(const char *canon_dir)
{
    if (!ezp_is_datalog(canon_dir)) {
        return false;
    }
    const char *p = canon_dir + 3;      /* past "A:\" */
    if (strlen(p) != 7 + 1 + 8) {       /* DATALOG\YYYYMMDD */
        return false;
    }
    if (p[7] != '\\') {
        return false;
    }
    /* day validity checked by caller (ezs_parse or catalog) */
    for (int i = 8; i < 16; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return false;
        }
    }
    return true;
}

int ezp_component_count(const char *canon_dir)
{
    if (!canon_dir || strncmp(canon_dir, "A:\\", 3) != 0) {
        return -1;
    }
    if (canon_dir[3] == '\0') {
        return 0;
    }
    int c = 1;
    for (const char *p = canon_dir + 3; *p; p++) {
        if (*p == '\\') {
            c++;
        }
    }
    return c;
}

bool ezp_datalog_day(const char *canon_dir, char day[9])
{
    if (!ezp_is_datalog_day(canon_dir)) {
        return false;
    }
    memcpy(day, canon_dir + 3 + 7 + 1, 8);
    day[8] = '\0';
    return true;
}

size_t ezp_urlencode(const char *in, char *out, size_t cap)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t w = 0;
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (unres) {
            if (w + 1 >= cap) break;
            out[w++] = (char)c;
        } else {
            if (w + 3 >= cap) break;
            out[w++] = '%';
            out[w++] = hexd[c >> 4];
            out[w++] = hexd[c & 15];
        }
    }
    if (w < cap) {
        out[w] = '\0';
    }
    return w;
}
