// src/core/lang.c - the language of the command line's texts (include/rubrapack/lang.h).

#include "rubrapack/lang.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const rp_lang_row_t *rows;       // NULL: English
static size_t nrows;
static const char *lang = "en";

bool rp_lang_set(const char *code) {
    if (strcmp(code, "en") == 0) {
        rows = NULL; nrows = 0; lang = "en";
        return true;
    }
    if (strcmp(code, "ko") == 0) {
        rows = rp_lang_ko; nrows = rp_lang_ko_count; lang = "ko";
        return true;
    }
    return false;
}

const char *rp_lang(void) { return lang; }

static const char *find(const char *en) {
    for (size_t i = 0; i < nrows; ++i) {
        if (strcmp(rows[i].en, en) == 0) return rows[i].tr;
    }
    return NULL;
}

const char *rp_tr(const char *en) {
    const char *t = rows ? find(en) : NULL;
    return t ? t : en;
}

// One conversion of the English format, with its value taken from the argument list.
typedef enum { K_INT, K_LONG, K_LLONG, K_SIZE, K_IMAX, K_PTRDIFF, K_UINT, K_ULONG, K_ULLONG, K_UMAX, K_STR, K_PTR } kind_t;

typedef struct {
    char   spec[32];
    int    nstar, star[2];
    kind_t kind;
    union {
        int i; long l; long long ll; size_t z; intmax_t j; ptrdiff_t t;
        unsigned u; unsigned long ul; unsigned long long ull; uintmax_t uj;
        const char *s; void *p;
    } v;
} conv_t;

enum { MAX_CONV = 12 };

// Parses the conversion at `p` (after its '%'). Returns its end, or NULL when it is not one this
// reads (the caller then falls back to the English text).
static const char *parse(const char *p, conv_t *c) {
    const char *start = p;
    c->nstar = 0;
    while (*p && strchr("-+ #0", *p)) ++p;
    if (*p == '*') { c->nstar++; ++p; }
    else while (*p >= '0' && *p <= '9') ++p;
    if (*p == '.') {
        ++p;
        if (*p == '*') { c->nstar++; ++p; }
        else while (*p >= '0' && *p <= '9') ++p;
    }
    int len = 0;        // 0 none, 1 h/hh, 2 l, 3 ll, 4 z, 5 j, 6 t
    if (*p == 'h') { len = 1; ++p; if (*p == 'h') ++p; }
    else if (*p == 'l') { len = 2; ++p; if (*p == 'l') { len = 3; ++p; } }
    else if (*p == 'z') { len = 4; ++p; }
    else if (*p == 'j') { len = 5; ++p; }
    else if (*p == 't') { len = 6; ++p; }
    char ch = *p;
    if (ch == 'd' || ch == 'i') c->kind = len == 2 ? K_LONG : len == 3 ? K_LLONG : len == 4 ? K_SIZE : len == 5 ? K_IMAX : len == 6 ? K_PTRDIFF : K_INT;
    else if (ch == 'u' || ch == 'x' || ch == 'X' || ch == 'o') c->kind = len == 2 ? K_ULONG : len == 3 ? K_ULLONG : len == 4 ? K_SIZE : len == 5 ? K_UMAX : len == 6 ? K_PTRDIFF : K_UINT;
    else if (ch == 'c' && len == 0) c->kind = K_INT;
    else if (ch == 's' && len == 0) c->kind = K_STR;
    else if (ch == 'p' && len == 0) c->kind = K_PTR;
    else return NULL;
    ++p;
    size_t n = (size_t)(p - start);
    if (n + 2 > sizeof c->spec) return NULL;
    c->spec[0] = '%';
    memcpy(c->spec + 1, start, n);
    c->spec[n + 1] = '\0';
    return p;
}

// The value of conversion `c` as text. The format is the English text's own conversion, checked
// where it is written; here it is only one that parse() took.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int one(char *out, size_t cap, const conv_t *c) {
#define PUT(value)                                                                                  \
    (c->nstar == 0 ? snprintf(out, cap, c->spec, value)                                             \
     : c->nstar == 1 ? snprintf(out, cap, c->spec, c->star[0], value)                               \
     : snprintf(out, cap, c->spec, c->star[0], c->star[1], value))
    switch (c->kind) {
    case K_INT: return PUT(c->v.i);
    case K_LONG: return PUT(c->v.l);
    case K_LLONG: return PUT(c->v.ll);
    case K_SIZE: return PUT(c->v.z);
    case K_IMAX: return PUT(c->v.j);
    case K_PTRDIFF: return PUT(c->v.t);
    case K_UINT: return PUT(c->v.u);
    case K_ULONG: return PUT(c->v.ul);
    case K_ULLONG: return PUT(c->v.ull);
    case K_UMAX: return PUT(c->v.uj);
    case K_STR: return PUT(c->v.s);
    case K_PTR: return PUT(c->v.p);
    }
#undef PUT
    return -1;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int rp_lang_vformat(char *out, size_t cap, const char *en_fmt, va_list ap) {
    const char *tr = rows ? find(en_fmt) : NULL;
    if (tr == NULL) return vsnprintf(out, cap, en_fmt, ap);

    // The English conversions, in order, each with its value.
    conv_t conv[MAX_CONV];
    size_t n = 0;
    va_list cp;
    va_copy(cp, ap);
    bool ok = true;
    for (const char *p = en_fmt; *p && ok;) {
        if (*p != '%') { ++p; continue; }
        if (p[1] == '%') { p += 2; continue; }
        if (n == MAX_CONV) { ok = false; break; }
        conv_t *c = &conv[n];
        const char *e = parse(p + 1, c);
        if (e == NULL) { ok = false; break; }
        for (int s = 0; s < c->nstar; ++s) c->star[s] = va_arg(cp, int);
        switch (c->kind) {
        case K_INT: c->v.i = va_arg(cp, int); break;
        case K_LONG: c->v.l = va_arg(cp, long); break;
        case K_LLONG: c->v.ll = va_arg(cp, long long); break;
        case K_SIZE: c->v.z = va_arg(cp, size_t); break;
        case K_IMAX: c->v.j = va_arg(cp, intmax_t); break;
        case K_PTRDIFF: c->v.t = va_arg(cp, ptrdiff_t); break;
        case K_UINT: c->v.u = va_arg(cp, unsigned); break;
        case K_ULONG: c->v.ul = va_arg(cp, unsigned long); break;
        case K_ULLONG: c->v.ull = va_arg(cp, unsigned long long); break;
        case K_UMAX: c->v.uj = va_arg(cp, uintmax_t); break;
        case K_STR:
            // A phrase handed in as a value (a reason, a kind of thing) has its own row; a name the
            // user wrote has no space-and-row match.
            c->v.s = va_arg(cp, const char *);
            if (c->v.s && strchr(c->v.s, ' ')) {
                const char *t = find(c->v.s);
                if (t) c->v.s = t;
            }
            break;
        case K_PTR: c->v.p = va_arg(cp, void *); break;
        }
        ++n;
        p = e;
    }
    va_end(cp);
    if (!ok) return vsnprintf(out, cap, en_fmt, ap);       // a conversion this does not read: English

    size_t len = 0;         // the whole length; bytes past cap - 1 are counted, not written
    char tmp[1024];
    for (const char *p = tr; *p; ++p) {
        const char *piece = p;
        size_t plen = 1;
        if (p[0] == '{' && p[1] == '{') {
            ++p;
        } else if (p[0] == '{' && p[1] == '-' && p[2] >= '1' && p[2] <= '9' && (p[3] == '}' || (p[3] >= '0' && p[3] <= '9' && p[4] == '}'))) {
            p += p[3] == '}' ? 3 : 4;      // {-N}: the English conversion N has no place in this language (a plural "s")
            continue;
        } else if (p[0] == '{' && p[1] >= '1' && p[1] <= '9') {
            size_t k = (size_t)(p[1] - '0');
            const char *q = p + 2;
            if (*q >= '0' && *q <= '9') { k = k * 10 + (size_t)(*q - '0'); ++q; }
            if (*q == '}' && k <= n) {
                int m = one(tmp, sizeof tmp, &conv[k - 1]);
                if (m < 0) m = 0;
                piece = tmp;
                plen = (size_t)m < sizeof tmp ? (size_t)m : sizeof tmp - 1;
                p = q;
            }
        }
        for (size_t i = 0; i < plen; ++i, ++len) {
            if (cap && len + 1 < cap) out[len] = piece[i];
        }
    }
    if (cap) out[len < cap ? len : cap - 1] = '\0';
    return (int)len;
}

int rp_lang_format(char *out, size_t cap, const char *en_fmt, ...) {
    va_list ap;
    va_start(ap, en_fmt);
    int n = rp_lang_vformat(out, cap, en_fmt, ap);
    va_end(ap);
    return n;
}
