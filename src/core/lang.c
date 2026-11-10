// src/core/lang.c - the language of the command line's texts (include/rubrapack/lang.h).

#include "rubrapack/lang.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/str.h"

#include "proven/fmt.h"
#include "proven/heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const rp_langpack_t *pack;       // NULL: English
static const char *lang = "en";

// ---- packs --------------------------------------------------------------------------------------

enum { MAX_PACKS = 64, MAX_PACK_BYTES = 4u << 20 };
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
            else if (strcmp(line, "X-Rubrapack-Direction") == 0) pk->rtl = strncmp(v, "rtl", 3) == 0;
            else if (strcmp(line, "X-Rubrapack-Group") == 0) pk->group = *v >= '1' && *v <= '8' && (v[1] == 0 || v[1] == ' ') ? *v - '0' : 0;
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
    (void)proven_fmt_to(pk->code, sizeof pk->code, "{}", proven_arg_cstr(code));
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
    int n = rp_fmt_n(proven_fmt_to(path, sizeof path, "{}{}/{}.po", proven_arg_cstr(dir), proven_arg_cstr(sub), proven_arg_cstr(code)));
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

// One value of the English proven format, rendered with its own placeholder (`{}` or `{:spec}`).
static size_t one_arg(char *out, size_t cap, const char *spec, size_t spec_len, proven_arg_t arg) {
    char piece[40];
    if (spec_len + 3 > sizeof piece) return 0;
    piece[0] = '{';
    memcpy(piece + 1, spec, spec_len);
    piece[spec_len + 1] = '}';
    piece[spec_len + 2] = '\0';
    const proven_arg_t args[2] = { proven_arg_none(), arg };
    proven_fmt_result_t r = proven_fmt_to_internal(out, cap, piece, args, 2);
    return r.written;
}

int rp_lang_format_a(char *out, size_t cap, const char *en_fmt, const proven_arg_t *args, size_t nargs) {
    const char *tr = pack ? find(en_fmt) : NULL;
    if (tr == NULL) {
        proven_fmt_result_t r = proven_fmt_to_internal(out, cap, en_fmt, args, nargs);
        if (r.err != PROVEN_OK && r.err != PROVEN_ERR_OUT_OF_BOUNDS) {
            if (cap) out[0] = '\0';
            return 0;
        }
        return (int)r.required;
    }

    // The English placeholders, in order: `{}` or `{:spec}` (the formats rubrapack writes name no
    // argument by number), each with its value; args[0] is proven's empty first argument.
    enum { MAX_ARGS = 16 };
    struct { const char *spec; size_t len; } ph[MAX_ARGS];
    proven_arg_t val[MAX_ARGS + 1];
    size_t n = 0;
    bool ok = nargs >= 1 && nargs <= MAX_ARGS + 1;
    for (const char *p = en_fmt; *p && ok; ++p) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) { ++p; continue; }
        if (*p != '{') continue;
        const char *q = strchr(p, '}');
        if (q == NULL || n + 1 >= nargs || (p[1] != '}' && p[1] != ':')) { ok = false; break; }
        ph[n].spec = p + 1;
        ph[n].len = (size_t)(q - p - 1);
        val[n + 1] = args[n + 1];
        // A phrase handed in as a value (a reason, a kind of thing) has its own row; a name the
        // user wrote has no space-and-row match.
        if (val[n + 1].type == PROVEN_ARG_CSTR && val[n + 1].value.cstr && strchr(val[n + 1].value.cstr, ' ')) {
            const char *t = rp_langpack_text(pack, NULL, val[n + 1].value.cstr);
            if (t) val[n + 1].value.cstr = t;
        }
        ++n;
        p = q;
    }
    if (!ok || n + 1 != nargs) {        // not a format this reads, or not its arguments: English
        proven_fmt_result_t r = proven_fmt_to_internal(out, cap, en_fmt, args, nargs);
        if (r.err != PROVEN_OK && r.err != PROVEN_ERR_OUT_OF_BOUNDS) {
            if (cap) out[0] = '\0';
            return 0;
        }
        return (int)r.required;
    }

    size_t len = 0;         // the whole length; bytes past cap - 1 are counted, not written
    char tmp[1024];
    for (const char *p = tr; *p; ++p) {
        const char *piece = p;
        size_t plen = 1;
        if (p[0] == '{' && p[1] == '{') {
            ++p;
        } else if (p[0] == '{' && p[1] == '-' && p[2] >= '1' && p[2] <= '9' && (p[3] == '}' || (p[3] >= '0' && p[3] <= '9' && p[4] == '}'))) {
            p += p[3] == '}' ? 3 : 4;      // {-N}: the English value N has no place in this language (a plural "s")
            continue;
        } else if (p[0] == '{' && p[1] >= '1' && p[1] <= '9') {
            size_t k = (size_t)(p[1] - '0');
            const char *q = p + 2;
            if (*q >= '0' && *q <= '9') { k = k * 10 + (size_t)(*q - '0'); ++q; }
            if (*q == '}' && k <= n) {
                plen = one_arg(tmp, sizeof tmp, ph[k - 1].spec, ph[k - 1].len, val[k]);
                piece = tmp;
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
