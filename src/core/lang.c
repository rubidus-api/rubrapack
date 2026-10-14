// src/core/lang.c - the language of the command line's texts (include/rubrapack/lang.h).

#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"

#include "proven/heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const rp_langpack_t *pack;       // NULL: English
static const char *lang = "en";

// ---- packs --------------------------------------------------------------------------------------

enum { MAX_PACKS = 24, MAX_PACK_BYTES = 4u << 20 };
static rp_langpack_t *packs[MAX_PACKS];
static size_t npacks;

static bool code_ok(const char *code) {
    size_t n = strlen(code);
    if (n == 0 || n > 15) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = code[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

// The quoted strings at *r (this line's, then every following line that starts with a quote),
// unescaped and joined at *w. Returns the text's start; both pointers move on. Unescaping only
// shortens, so writing never passes reading.
static char *po_text(char **r, char **w) {
    char *start = *w, *p = *r, *o = *w;
    for (;;) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '"') break;
        ++p;
        while (*p && *p != '"' && *p != '\n') {
            if (*p == '\\' && p[1]) {
                ++p;
                *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
                ++p;
            } else {
                *o++ = *p++;
            }
        }
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    *r = p;
    *o++ = '\0';
    *w = o;
    return start;
}

static void header(rp_langpack_t *pk, char *h) {
    for (char *line = h; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ') ++v;
            if (strcmp(line, "X-Rubrapack-Name") == 0 && *v) pk->name = v;
            else if (strcmp(line, "X-Rubrapack-Font") == 0 && *v) pk->font = v;
            else if (strcmp(line, "X-Rubrapack-LangIDs") == 0) {
                for (char *q = v; *q && pk->langid_count < RP_LANGPACK_LANGIDS;) {
                    unsigned long id = strtoul(q, &q, 10);
                    if (id >= 1 && id <= 65535) pk->langids[pk->langid_count++] = (unsigned)id;
                    while (*q && (*q < '0' || *q > '9')) ++q;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

// Reads a pack from `text` (NUL-terminated, changed in place and kept).
static rp_langpack_t *parse_pack(const char *code, char *text) {
    proven_allocator_t heap = proven_heap_allocator();
    rp_langpack_t *pk = rp_mem_alloc(heap, 1, sizeof *pk);
    if (pk == NULL) return NULL;
    memset(pk, 0, sizeof *pk);
    snprintf(pk->code, sizeof pk->code, "%s", code);
    size_t cap = 0;
    char *r = text, *w = text;
    if ((unsigned char)r[0] == 0xEF && (unsigned char)r[1] == 0xBB && (unsigned char)r[2] == 0xBF) r += 3;
    const char *ctxt = NULL, *id = NULL;
    while (*r) {
        while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') ++r;
        if (*r == '#') {
            while (*r && *r != '\n') ++r;
        } else if (strncmp(r, "msgctxt", 7) == 0) {
            r += 7;
            ctxt = po_text(&r, &w);
        } else if (strncmp(r, "msgid", 5) == 0 && r[5] != '_') {
            r += 5;
            id = po_text(&r, &w);
        } else if (strncmp(r, "msgstr", 6) == 0 && r[6] != '[') {
            r += 6;
            char *str = po_text(&r, &w);
            if (id && id[0] == '\0' && ctxt == NULL) {
                header(pk, str);
            } else if (id && id[0] && str[0]) {
                if (pk->count == cap) {
                    size_t ncap = cap ? cap * 2 : 256;
                    rp_lang_row_t *nr = rp_mem_alloc(heap, ncap, sizeof *nr);
                    if (nr == NULL) break;
                    if (pk->rows) memcpy(nr, pk->rows, pk->count * sizeof *nr);
                    rp_mem_free(heap, pk->rows);
                    pk->rows = nr;
                    cap = ncap;
                }
                pk->rows[pk->count++] = (rp_lang_row_t){ ctxt && ctxt[0] ? ctxt : NULL, id, str };
            }
            ctxt = NULL;
            id = NULL;
        } else if (*r) {
            while (*r && *r != '\n') ++r;        // a line this does not read (plural forms, obsolete entries)
        }
    }
    return pk;
}

static char *file_text(const char *dir, const char *sub, const char *code) {
    proven_allocator_t heap = proven_heap_allocator();
    char path[1200];
    int n = snprintf(path, sizeof path, "%s%s/%s.po", dir, sub, code);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_pal_read_file(heap, path, MAX_PACK_BYTES, &data, &len) != PROVEN_OK) return NULL;
    char *text = rp_mem_alloc(heap, len + 1, 1);
    if (text) {
        memcpy(text, data, len);
        text[len] = '\0';
    }
    rp_mem_free(heap, data);
    return text;
}

const rp_langpack_t *rp_langpack_get(const char *code, const char *near) {
    if (code == NULL || !code_ok(code)) return NULL;
    for (size_t i = 0; i < npacks; ++i) {
        if (strcmp(packs[i]->code, code) == 0) return packs[i];
    }
    if (npacks == MAX_PACKS) return NULL;
    proven_allocator_t heap = proven_heap_allocator();
    char *text = NULL;
    {
        char *dir = rp_pal_getenv(heap, "RUBRAPACK_LANG_DIR");
        if (dir && dir[0]) text = file_text(dir, "", code);
        if (dir) rp_mem_free(heap, dir);
    }
    if (text == NULL && near) text = file_text(near[0] ? near : ".", "/lang", code);
    if (text == NULL) {
        char *exe = rp_pal_exe_dir(heap);
        if (exe) text = file_text(exe, "/lang", code);
        if (exe) rp_mem_free(heap, exe);
    }
    for (size_t i = 0; i < rp_lang_embedded_count && text == NULL; ++i) {     // built in: the last resort
        if (strcmp(rp_lang_embedded[i].code, code) != 0) continue;
        text = rp_mem_alloc(heap, rp_lang_embedded[i].len + 1, 1);
        if (text) {
            memcpy(text, rp_lang_embedded[i].data, rp_lang_embedded[i].len);
            text[rp_lang_embedded[i].len] = '\0';
        }
    }
    if (text == NULL) return NULL;
    rp_langpack_t *pk = parse_pack(code, text);
    if (pk == NULL) return NULL;
    packs[npacks++] = pk;
    return pk;
}

const char *rp_langpack_text(const rp_langpack_t *pk, const char *ctxt, const char *id) {
    if (pk == NULL) return NULL;
    for (size_t i = 0; i < pk->count; ++i) {
        const rp_lang_row_t *row = &pk->rows[i];
        if ((row->ctxt == NULL) != (ctxt == NULL) || (ctxt && strcmp(row->ctxt, ctxt) != 0)) continue;
        if (strcmp(row->id, id) == 0) return row->str;
    }
    return NULL;
}

bool rp_lang_set(const char *code) {
    if (strcmp(code, "en") == 0) {
        pack = NULL;
        lang = "en";
        return true;
    }
    const rp_langpack_t *pk = rp_langpack_get(code, NULL);
    if (pk == NULL) return false;
    pack = pk;
    lang = pk->code;
    return true;
}

const char *rp_lang(void) { return lang; }

const rp_langpack_t *rp_lang_pack(void) { return pack; }

// A test hook: with RUBRAPACK_LANG_MISSING=<file>, each text the chosen pack lacks is appended to
// that file, so a run of the tests in a language lists what its pack still needs.
static void note_missing(const char *en) {
    static int state;           // 0 not looked, 1 off, 2 on
    static char path[1024];
    if (state == 0) {
        char *v = rp_pal_getenv(proven_heap_allocator(), "RUBRAPACK_LANG_MISSING");
        state = v && v[0] && strlen(v) < sizeof path ? 2 : 1;
        if (state == 2) memcpy(path, v, strlen(v) + 1);
        if (v) rp_mem_free(proven_heap_allocator(), v);
    }
    if (state != 2) return;
    FILE *f = fopen(path, "ab");
    if (f == NULL) return;
    fputs(en, f);
    fputs("\n\x1e\n", f);
    fclose(f);
}

static const char *find(const char *en) {
    const char *t = rp_langpack_text(pack, NULL, en);
    if (t == NULL) note_missing(en);
    return t;
}

const char *rp_tr(const char *en) {
    const char *t = pack ? find(en) : NULL;
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
    const char *tr = pack ? find(en_fmt) : NULL;
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
                const char *t = rp_langpack_text(pack, NULL, c->v.s);
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
