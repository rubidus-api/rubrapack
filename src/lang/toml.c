// src/lang/toml.c - the `.rpk` TOML subset parser (include/rubrapack/toml.h, RFC-0002 section 1).
//
// Line oriented: a line is blank, a comment, a table header, or `key = value`; only arrays may
// continue over several lines. Errors are collected (up to the diagnostic limit) and the parse
// goes on at the next line, so one run reports several mistakes.

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/text.h"
#include "rubrapack/toml.h"

#include <string.h>

typedef struct {
    const uint8_t     *s;
    size_t             n;
    size_t             i;
    uint32_t           line;
    size_t             line_start;
    rp_srcdiags_t     *d;
    proven_allocator_t alloc;
    rp_tdoc_t         *doc;
    rp_ttable_t       *cur;
    bool               nomem;
} parser_t;

// ---- positions and errors ------------------------------------------------------------------

static rp_pos_t pos_at(const parser_t *p, size_t off) {
    uint32_t col = 1;
    for (size_t k = p->line_start; k < off && k < p->n; ++k) {
        if ((p->s[k] & 0xC0u) != 0x80u) ++col;
    }
    return (rp_pos_t){ p->line, col };
}

#define ERR(p, off, code, ...) rp_srcdiag_add((p)->d, pos_at((p), (off)), (code), false, __VA_ARGS__)

// Position of a byte offset in a text we are not parsing line by line (encoding checks).
static rp_pos_t pos_in(const uint8_t *s, size_t off) {
    rp_pos_t pos = { 1, 1 };
    for (size_t k = 0; k < off; ++k) {
        if (s[k] == '\n') {
            pos.line++;
            pos.col = 1;
        } else if ((s[k] & 0xC0u) != 0x80u) {
            pos.col++;
        }
    }
    return pos;
}

// ---- small helpers ---------------------------------------------------------------------------

static bool at_end(const parser_t *p) { return p->i >= p->n; }
static uint8_t peek(const parser_t *p, size_t k) { return p->i + k < p->n ? p->s[p->i + k] : 0; }

static void skip_ws(parser_t *p) {
    while (!at_end(p) && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) p->i++;
}

static bool is_eol(const parser_t *p) {
    return at_end(p) || p->s[p->i] == '\n' || (p->s[p->i] == '\r' && peek(p, 1) == '\n');
}

static void newline(parser_t *p) {   // at '\n' or "\r\n"
    if (p->s[p->i] == '\r') p->i++;
    p->i++;
    p->line++;
    p->line_start = p->i;
}

static bool is_control(uint8_t c) { return (c < 0x20 && c != '\t') || c == 0x7F; }

// Consumes a comment to the end of the line; control characters are not allowed in it.
static void skip_comment(parser_t *p) {
    while (!is_eol(p)) {
        if (is_control(p->s[p->i])) ERR(p, p->i, "RP1003", "control character in a comment");
        p->i++;
    }
}

static void skip_line(parser_t *p) {
    while (!at_end(p) && p->s[p->i] != '\n') p->i++;
}

static bool bare_key_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static char *copy_str(parser_t *p, const void *src, size_t n) {
    char *s = rp_mem_alloc(p->alloc, n + 1, 1);
    if (s == NULL) {
        p->nomem = true;
        return NULL;
    }
    if (n) memcpy(s, src, n);
    s[n] = '\0';
    return s;
}

static size_t read_bare_key(parser_t *p) {
    size_t start = p->i;
    while (!at_end(p) && bare_key_char(p->s[p->i])) p->i++;
    return p->i - start;
}

static void free_val(proven_allocator_t alloc, rp_tval_t *v) {
    rp_mem_free(alloc, v->str);
    for (size_t k = 0; k < v->count; ++k) free_val(alloc, &v->items[k]);
    rp_mem_free(alloc, v->items);
    memset(v, 0, sizeof *v);
}

// ---- values ----------------------------------------------------------------------------------

static bool parse_value(parser_t *p, rp_tval_t *out, bool in_array);

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_basic_string(parser_t *p, rp_tval_t *out) {
    size_t start = p->i;
    if (peek(p, 1) == '"' && peek(p, 2) == '"') {
        ERR(p, start, "RP1103", "multi-line strings are not supported; use one line with \\n");
        size_t k = p->i + 3;        // step over to the closing """ so parsing can go on
        while (k + 2 < p->n && !(p->s[k] == '"' && p->s[k + 1] == '"' && p->s[k + 2] == '"')) {
            if (p->s[k] == '\n') {
                p->line++;
                p->line_start = k + 1;
            }
            ++k;
        }
        p->i = k + 3 <= p->n ? k + 3 : p->n;
        while (!at_end(p) && p->s[p->i] == '"') p->i++;
        return false;
    }
    p->i++;
    rp_buf_t b = rp_buf_new(p->alloc, (size_t)1 << 24);
    bool ok = true;
    for (;;) {
        if (is_eol(p)) {
            ERR(p, start, "RP1100", "string is not closed on this line");
            ok = false;
            break;
        }
        uint8_t c = p->s[p->i];
        if (c == '"') {
            p->i++;
            break;
        }
        if (is_control(c)) {
            ERR(p, p->i, "RP1003", "control character in a string");
            ok = false;
            p->i++;
            continue;
        }
        if (c != '\\') {
            rp_buf_byte(&b, c);
            p->i++;
            continue;
        }
        size_t esc = p->i;
        uint8_t e = peek(p, 1);
        p->i += 2;
        static const char plain_from[] = "btnfr\"\\";
        static const char plain_to[] = "\b\t\n\f\r\"\\";
        const char *hit = e ? strchr(plain_from, e) : NULL;
        if (hit) {
            rp_buf_byte(&b, (uint8_t)plain_to[hit - plain_from]);
        } else if (e == 'u' || e == 'U') {
            int digits = e == 'u' ? 4 : 8;
            uint32_t cp = 0;
            bool good = true;
            for (int k = 0; k < digits; ++k) {
                int h = hexval(peek(p, 0));
                if (h < 0) {
                    good = false;
                    break;
                }
                cp = cp * 16 + (uint32_t)h;
                p->i++;
            }
            if (!good || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                ERR(p, esc, "RP1100", "\\%c escape is not a Unicode scalar value", e);
                ok = false;
            } else {
                uint8_t u[4];
                size_t un = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                if (un == 1) u[0] = (uint8_t)cp;
                else if (un == 2) { u[0] = (uint8_t)(0xC0 | (cp >> 6)); u[1] = (uint8_t)(0x80 | (cp & 0x3F)); }
                else if (un == 3) { u[0] = (uint8_t)(0xE0 | (cp >> 12)); u[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (uint8_t)(0x80 | (cp & 0x3F)); }
                else { u[0] = (uint8_t)(0xF0 | (cp >> 18)); u[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (uint8_t)(0x80 | (cp & 0x3F)); }
                rp_buf_put(&b, u, un);
            }
        } else {
            ERR(p, esc, "RP1100", "unknown escape \\%c (use '...' for text with backslashes)", e >= 0x20 && e < 0x7F ? e : '?');
            ok = false;
        }
    }
    uint8_t *data = NULL;
    size_t len = 0;
    if (rp_buf_take(&b, &data, &len) != PROVEN_OK) {
        p->nomem = true;
        return false;
    }
    if (!ok) {
        rp_mem_free(p->alloc, data);
        return false;
    }
    out->kind = RP_TV_STRING;
    out->str = copy_str(p, data, len);
    out->len = len;
    rp_mem_free(p->alloc, data);
    return out->str != NULL;
}

static bool parse_literal_string(parser_t *p, rp_tval_t *out) {
    size_t start = p->i;
    if (peek(p, 1) == '\'' && peek(p, 2) == '\'') {
        ERR(p, start, "RP1103", "multi-line strings are not supported");
        size_t k = p->i + 3;
        while (k + 2 < p->n && !(p->s[k] == '\'' && p->s[k + 1] == '\'' && p->s[k + 2] == '\'')) {
            if (p->s[k] == '\n') {
                p->line++;
                p->line_start = k + 1;
            }
            ++k;
        }
        p->i = k + 3 <= p->n ? k + 3 : p->n;
        while (!at_end(p) && p->s[p->i] == '\'') p->i++;
        return false;
    }
    p->i++;
    size_t from = p->i;
    bool ok = true;
    while (!is_eol(p) && p->s[p->i] != '\'') {
        if (is_control(p->s[p->i])) {
            ERR(p, p->i, "RP1003", "control character in a string");
            ok = false;
        }
        p->i++;
    }
    if (is_eol(p)) {
        ERR(p, start, "RP1100", "string is not closed on this line");
        return false;
    }
    size_t to = p->i++;
    if (!ok) return false;
    out->kind = RP_TV_STRING;
    out->literal = true;
    out->str = copy_str(p, p->s + from, to - from);
    out->len = to - from;
    return out->str != NULL;
}

static bool value_end(uint8_t c) {
    return c == 0 || c == ' ' || c == '\t' || c == ',' || c == ']' || c == '#' || c == '\n' || c == '\r';
}

static bool is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static bool parse_number(parser_t *p, rp_tval_t *out) {
    size_t start = p->i;
    // Dates and times first: 1979-05-27, 07:32:00.
    if (p->n - p->i >= 5 && is_digit(peek(p, 0)) && is_digit(peek(p, 1)) &&
        ((is_digit(peek(p, 2)) && is_digit(peek(p, 3)) && peek(p, 4) == '-') || peek(p, 2) == ':')) {
        ERR(p, start, "RP1106", "dates and times are not supported; write them as strings");
        while (!is_eol(p) && p->s[p->i] != '#' && p->s[p->i] != ',' && p->s[p->i] != ']') p->i++;
        return false;
    }
    size_t k = p->i;
    while (k < p->n && !value_end(p->s[k])) ++k;
    const uint8_t *t = p->s + p->i;
    size_t tn = k - p->i;
    p->i = k;
    bool neg = false;
    size_t o = 0;
    if (tn > 0 && (t[0] == '+' || t[0] == '-')) {
        neg = t[0] == '-';
        o = 1;
    }
    // inf / nan / floats
    if ((tn - o == 3 && (memcmp(t + o, "inf", 3) == 0 || memcmp(t + o, "nan", 3) == 0)) ||
        memchr(t, '.', tn) != NULL || (tn > 2 && t[1] != 'x' && (memchr(t, 'e', tn) || memchr(t, 'E', tn)))) {
        ERR(p, start, "RP1106", "floating-point numbers are not supported");
        return false;
    }
    if (tn >= 2 && t[0] == '0' && (t[1] == 'o' || t[1] == 'b')) {
        ERR(p, start, "RP1104", "octal and binary numbers are not supported; use decimal or 0x");
        return false;
    }
    bool hex = tn >= 2 && t[0] == '0' && t[1] == 'x';
    if (hex) o = 2;
    if (o >= tn) {
        ERR(p, start, "RP1100", "not a value");
        return false;
    }
    uint64_t v = 0;
    bool underscore = false, bad = false, overflow = false;
    for (size_t j = o; j < tn; ++j) {
        uint8_t c = t[j];
        if (c == '_') {
            underscore = true;
            if (j == o || j + 1 == tn || t[j + 1] == '_') bad = true;
            continue;
        }
        int d = hex ? hexval(c) : (is_digit(c) ? c - '0' : -1);
        if (d < 0) {
            bad = true;
            break;
        }
        uint64_t base = hex ? 16 : 10;
        if (v > (UINT64_MAX - (uint64_t)d) / base) overflow = true;
        else v = v * base + (uint64_t)d;
    }
    if (bad || (!hex && tn - o > 1 && t[o] == '0')) {
        ERR(p, start, "RP1100", "not a valid number");
        return false;
    }
    if (underscore) {
        ERR(p, start, "RP1104", "'_' in numbers is not supported");
        return false;
    }
    if (overflow || (!neg && v > (uint64_t)INT64_MAX) || (neg && v > (uint64_t)INT64_MAX + 1)) {
        ERR(p, start, "RP1104", "number out of range");
        return false;
    }
    out->kind = RP_TV_INT;
    out->i = neg ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

// Skips spaces, newlines and comments inside an array.
static void skip_array_space(parser_t *p) {
    for (;;) {
        skip_ws(p);
        if (at_end(p)) return;
        uint8_t c = p->s[p->i];
        if (c == '#') {
            skip_comment(p);
        } else if (c == '\n' || (c == '\r' && peek(p, 1) == '\n')) {
            newline(p);
        } else {
            return;
        }
    }
}

static bool parse_array(parser_t *p, rp_tval_t *out) {
    size_t start = p->i;
    p->i++;
    out->kind = RP_TV_ARRAY;
    size_t cap = 0;
    bool ok = true;
    for (;;) {
        skip_array_space(p);
        if (at_end(p)) {
            ERR(p, start, "RP1100", "array is not closed");
            return false;
        }
        if (p->s[p->i] == ']') {
            p->i++;
            break;
        }
        rp_tval_t item = { 0 };
        item.pos = pos_at(p, p->i);
        if (!parse_value(p, &item, true)) {
            ok = false;
            skip_line(p);
            return false;
        }
        if (out->count == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            rp_tval_t *v = rp_mem_alloc(p->alloc, ncap, sizeof *v);
            if (v == NULL) {
                p->nomem = true;
                free_val(p->alloc, &item);
                return false;
            }
            if (out->count) memcpy(v, out->items, out->count * sizeof *v);
            rp_mem_free(p->alloc, out->items);
            out->items = v;
            cap = ncap;
        }
        out->items[out->count++] = item;
        skip_array_space(p);
        if (!at_end(p) && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (!at_end(p) && p->s[p->i] == ']') {
            p->i++;
            break;
        }
        ERR(p, p->i, "RP1100", "expected ',' or ']' in the array");
        return false;
    }
    if (out->count == 0) {
        ERR(p, start, "RP1105", "empty arrays are not supported");
        return false;
    }
    for (size_t k = 0; k < out->count; ++k) {
        rp_tkind_t kd = out->items[k].kind;
        if (kd != out->items[0].kind || (kd != RP_TV_STRING && kd != RP_TV_INT)) {
            ERR(p, start, "RP1105", "arrays must hold only strings or only integers");
            return false;
        }
    }
    return ok;
}

static bool parse_value(parser_t *p, rp_tval_t *out, bool in_array) {
    memset(out, 0, sizeof *out);
    out->pos = pos_at(p, p->i);
    if (at_end(p) || is_eol(p)) {
        ERR(p, p->i, "RP1100", "value missing");
        return false;
    }
    uint8_t c = p->s[p->i];
    if (c == '"') return parse_basic_string(p, out);
    if (c == '\'') return parse_literal_string(p, out);
    if (c == '[') {
        if (in_array) {
            ERR(p, p->i, "RP1105", "arrays inside arrays are not supported");
            return false;
        }
        return parse_array(p, out);
    }
    if (c == '{') {
        ERR(p, p->i, "RP1106", "inline tables are not supported; use a [kind.ID] table");
        skip_line(p);
        return false;
    }
    if (c == 't' || c == 'f') {
        const char *w = c == 't' ? "true" : "false";
        size_t wn = strlen(w);
        if (p->n - p->i >= wn && memcmp(p->s + p->i, w, wn) == 0 && value_end(peek(p, wn))) {
            p->i += wn;
            out->kind = RP_TV_BOOL;
            out->b = c == 't';
            return true;
        }
    }
    if (c == 'i' || c == 'n') {
        if (p->n - p->i >= 3 && (memcmp(p->s + p->i, "inf", 3) == 0 || memcmp(p->s + p->i, "nan", 3) == 0)) {
            ERR(p, p->i, "RP1106", "floating-point numbers are not supported");
            p->i += 3;
            return false;
        }
    }
    if (is_digit(c) || c == '+' || c == '-') return parse_number(p, out);
    ERR(p, p->i, "RP1100", "not a value (strings need quotes)");
    return false;
}

// ---- tables and keys ------------------------------------------------------------------------

static rp_ttable_t *add_table(parser_t *p, char *kind, char *id, rp_pos_t pos) {
    for (size_t t = 0; t < p->doc->count; ++t) {
        rp_ttable_t *x = &p->doc->tables[t];
        bool same_id = (x->id == NULL && id == NULL) || (x->id && id && strcmp(x->id, id) == 0);
        if (strcmp(x->kind, kind) == 0 && same_id) {
            rp_srcdiag_add(p->d, pos, "RP1107", false, "table [%s%s%s] is defined twice (first at line %u)", kind,
                           id ? "." : "", id ? id : "", (unsigned)x->pos.line);
            rp_mem_free(p->alloc, kind);
            rp_mem_free(p->alloc, id);
            return NULL;
        }
    }
    if (p->doc->count == p->doc->cap) {
        size_t ncap = p->doc->cap ? p->doc->cap * 2 : 16;
        rp_ttable_t *v = rp_mem_alloc(p->alloc, ncap, sizeof *v);
        if (v == NULL) {
            p->nomem = true;
            rp_mem_free(p->alloc, kind);
            rp_mem_free(p->alloc, id);
            return NULL;
        }
        if (p->doc->count) memcpy(v, p->doc->tables, p->doc->count * sizeof *v);
        rp_mem_free(p->alloc, p->doc->tables);
        p->doc->tables = v;
        p->doc->cap = ncap;
    }
    rp_ttable_t *t = &p->doc->tables[p->doc->count++];
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->id = id;
    t->pos = pos;
    return t;
}

static void parse_header(parser_t *p) {
    size_t start = p->i;
    rp_pos_t pos = pos_at(p, start);
    p->cur = NULL;                  // keys after a bad header belong to no table
    if (peek(p, 1) == '[') {
        ERR(p, start, "RP1101", "[[...]] arrays of tables are not supported; give each item its own [kind.ID]");
        skip_line(p);
        return;
    }
    p->i++;
    char *seg[3] = { NULL, NULL, NULL };
    size_t nseg = 0;
    for (;;) {
        skip_ws(p);
        uint8_t c = at_end(p) ? 0 : p->s[p->i];
        if (c == '"' || c == '\'') {
            ERR(p, p->i, "RP1101", "quoted table names are not supported");
            goto bad;
        }
        size_t from = p->i, len = read_bare_key(p);
        if (len == 0) {
            ERR(p, p->i, "RP1100", "table name expected");
            goto bad;
        }
        if (nseg < 3) seg[nseg] = copy_str(p, p->s + from, len);
        nseg++;
        skip_ws(p);
        c = at_end(p) ? 0 : p->s[p->i];
        if (c == '.') {
            p->i++;
            continue;
        }
        if (c == ']') {
            p->i++;
            break;
        }
        ERR(p, p->i, "RP1100", "expected ']' to close the table name");
        goto bad;
    }
    if (nseg > 2) {
        ERR(p, start, "RP1101", "table names have at most two parts: [kind] or [kind.ID]");
        goto bad;
    }
    p->cur = add_table(p, seg[0], seg[1], pos);
    return;
bad:
    for (int k = 0; k < 3; ++k) rp_mem_free(p->alloc, seg[k]);
    skip_line(p);
}

static void parse_keyval(parser_t *p) {
    size_t start = p->i;
    uint8_t c = p->s[p->i];
    if (c == '"' || c == '\'') {
        ERR(p, start, "RP1102", "quoted keys are not supported");
        skip_line(p);
        return;
    }
    size_t len = read_bare_key(p);
    if (len == 0) {
        ERR(p, start, "RP1100", "expected a key, a [table] or a comment");
        skip_line(p);
        return;
    }
    skip_ws(p);
    if (!at_end(p) && p->s[p->i] == '.') {
        ERR(p, start, "RP1102", "dotted keys are not supported; put the key in its own table");
        skip_line(p);
        return;
    }
    if (at_end(p) || p->s[p->i] != '=') {
        ERR(p, p->i, "RP1100", "expected '=' after the key");
        skip_line(p);
        return;
    }
    p->i++;
    skip_ws(p);
    rp_tkey_t k = { 0 };
    k.pos = pos_at(p, start);
    if (!parse_value(p, &k.val, false)) {
        free_val(p->alloc, &k.val);
        return;
    }
    rp_ttable_t *t = p->cur;
    if (t == NULL) {                // after a bad header: its error is enough
        free_val(p->alloc, &k.val);
        return;
    }
    for (size_t j = 0; j < t->count; ++j) {
        if (strlen(t->keys[j].key) == len && memcmp(t->keys[j].key, p->s + start, len) == 0) {
            rp_srcdiag_add(p->d, k.pos, "RP1107", false, "key '%.*s' is set twice in this table (first at line %u)",
                           (int)len, (const char *)p->s + start, (unsigned)t->keys[j].pos.line);
            free_val(p->alloc, &k.val);
            return;
        }
    }
    k.key = copy_str(p, p->s + start, len);
    if (t->count == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 8;
        rp_tkey_t *v = rp_mem_alloc(p->alloc, ncap, sizeof *v);
        if (v == NULL || k.key == NULL) {
            p->nomem = true;
            rp_mem_free(p->alloc, v);
            rp_mem_free(p->alloc, k.key);
            free_val(p->alloc, &k.val);
            return;
        }
        if (t->count) memcpy(v, t->keys, t->count * sizeof *v);
        rp_mem_free(p->alloc, t->keys);
        t->keys = v;
        t->cap = ncap;
    }
    t->keys[t->count++] = k;
}

// ---- document ------------------------------------------------------------------------------

static void free_table(proven_allocator_t alloc, rp_ttable_t *x) {
    for (size_t k = 0; k < x->count; ++k) {
        rp_mem_free(alloc, x->keys[k].key);
        free_val(alloc, &x->keys[k].val);
    }
    rp_mem_free(alloc, x->keys);
    rp_mem_free(alloc, x->kind);
    rp_mem_free(alloc, x->id);
}

void rp_toml_free(rp_tdoc_t *doc) {
    if (doc == NULL) return;
    free_table(doc->alloc, &doc->root);
    memset(&doc->root, 0, sizeof doc->root);
    for (size_t t = 0; t < doc->count; ++t) free_table(doc->alloc, &doc->tables[t]);
    rp_mem_free(doc->alloc, doc->tables);
    doc->tables = NULL;
    doc->count = doc->cap = 0;
}

proven_err_t rp_toml_parse(proven_allocator_t alloc, const uint8_t *data, size_t len, rp_tdoc_t *doc,
                           rp_srcdiags_t *diags) {
    if (doc == NULL || diags == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    memset(doc, 0, sizeof *doc);
    doc->alloc = alloc;
    uint8_t *converted = NULL;
    const uint8_t *s = data;
    size_t n = len;

    // Encoding (RFC-0001 15.4): UTF-8 with an optional BOM, or UTF-16LE with a BOM.
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
        s += 3;
        n -= 3;
    } else if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
        size_t units = (n - 2) / 2;
        uint16_t *w = rp_mem_alloc(alloc, units, sizeof *w);
        if (w == NULL) return PROVEN_ERR_NOMEM;
        for (size_t k = 0; k < units; ++k) w[k] = (uint16_t)(s[2 + 2 * k] | (s[3 + 2 * k] << 8));
        rp_text_result_t r = rp_utf16_to_utf8(w, units, NULL, 0);
        if ((n - 2) % 2 != 0 || r.err != PROVEN_OK) {
            rp_mem_free(alloc, w);
            rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1001", false, "the UTF-16 file is not valid UTF-16");
            return PROVEN_ERR_INVALID_FORMAT;
        }
        converted = rp_mem_alloc(alloc, r.units, 1);
        if (converted == NULL) {
            rp_mem_free(alloc, w);
            return PROVEN_ERR_NOMEM;
        }
        (void)rp_utf16_to_utf8(w, units, converted, r.units);
        rp_mem_free(alloc, w);
        s = converted;
        n = r.units;
    } else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
        rp_srcdiag_add(diags, (rp_pos_t){ 1, 1 }, "RP1001", false, "UTF-16 big-endian files are not supported; save as UTF-8");
        return PROVEN_ERR_INVALID_FORMAT;
    }
    rp_text_result_t v = rp_utf8_validate(s, n);
    if (v.err != PROVEN_OK) {
        rp_srcdiag_add(diags, pos_in(s, v.offset), "RP1001", false, "not valid UTF-8");
        rp_mem_free(alloc, converted);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    for (size_t k = 0; k < n; ++k) {
        if (s[k] == '\r' && (k + 1 >= n || s[k + 1] != '\n')) {
            rp_srcdiag_add(diags, pos_in(s, k), "RP1002", false, "carriage return without a line feed");
            rp_mem_free(alloc, converted);
            return PROVEN_ERR_INVALID_FORMAT;
        }
    }

    parser_t p = { .s = s, .n = n, .line = 1, .d = diags, .alloc = alloc, .doc = doc, .cur = &doc->root };
    while (!at_end(&p) && !p.nomem) {
        skip_ws(&p);
        if (at_end(&p)) break;
        uint8_t c = p.s[p.i];
        if (c == '\n' || c == '\r') {
            newline(&p);
            continue;
        }
        if (c == '#') {
            skip_comment(&p);
        } else if (is_control(c)) {
            ERR(&p, p.i, "RP1003", "control character");
            skip_line(&p);
        } else if (c == '[') {
            parse_header(&p);
        } else {
            parse_keyval(&p);
        }
        // Only a comment may follow on the same line.
        skip_ws(&p);
        if (!at_end(&p) && p.s[p.i] == '#') skip_comment(&p);
        if (!is_eol(&p)) {
            ERR(&p, p.i, "RP1100", "unexpected text after the value");
            skip_line(&p);
        }
        if (!at_end(&p)) newline(&p);
    }
    rp_mem_free(alloc, converted);
    if (p.nomem) {
        rp_toml_free(doc);
        return PROVEN_ERR_NOMEM;
    }
    if (diags->errors > 0) {
        rp_toml_free(doc);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
}

// ---- JSON dump (test oracle format) ---------------------------------------------------------

typedef struct jnode {
    const char      *name;
    const rp_tval_t *val;       // NULL for an object
    struct jnode    *kids;
    size_t           count, cap;
} jnode_t;

static jnode_t *jchild(proven_allocator_t alloc, jnode_t *parent, const char *name) {
    for (size_t k = 0; k < parent->count; ++k) {
        if (strcmp(parent->kids[k].name, name) == 0) return &parent->kids[k];
    }
    if (parent->count == parent->cap) {
        size_t ncap = parent->cap ? parent->cap * 2 : 8;
        jnode_t *v = rp_mem_alloc(alloc, ncap, sizeof *v);
        if (v == NULL) return NULL;
        if (parent->count) memcpy(v, parent->kids, parent->count * sizeof *v);
        rp_mem_free(alloc, parent->kids);
        parent->kids = v;
        parent->cap = ncap;
    }
    jnode_t *c = &parent->kids[parent->count++];
    memset(c, 0, sizeof *c);
    c->name = name;
    return c;
}

static void jfree(proven_allocator_t alloc, jnode_t *n) {
    for (size_t k = 0; k < n->count; ++k) jfree(alloc, &n->kids[k]);
    rp_mem_free(alloc, n->kids);
}

static void json_string(rp_buf_t *b, const char *s, size_t n) {
    rp_buf_byte(b, '"');
    for (size_t k = 0; k < n; ++k) {
        uint8_t c = (uint8_t)s[k];
        switch (c) {
        case '"': rp_buf_puts(b, "\\\""); break;
        case '\\': rp_buf_puts(b, "\\\\"); break;
        case '\n': rp_buf_puts(b, "\\n"); break;
        case '\r': rp_buf_puts(b, "\\r"); break;
        case '\t': rp_buf_puts(b, "\\t"); break;
        case '\b': rp_buf_puts(b, "\\b"); break;
        case '\f': rp_buf_puts(b, "\\f"); break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                char e[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0 };
                rp_buf_puts(b, e);
            } else {
                rp_buf_byte(b, c);
            }
        }
    }
    rp_buf_byte(b, '"');
}

static void json_value(rp_buf_t *b, const rp_tval_t *v) {
    switch (v->kind) {
    case RP_TV_STRING: json_string(b, v->str, v->len); break;
    case RP_TV_INT: rp_buf_long(b, v->i); break;
    case RP_TV_BOOL: rp_buf_puts(b, v->b ? "true" : "false"); break;
    case RP_TV_ARRAY:
        rp_buf_byte(b, '[');
        for (size_t k = 0; k < v->count; ++k) {
            if (k) rp_buf_puts(b, ", ");
            json_value(b, &v->items[k]);
        }
        rp_buf_byte(b, ']');
        break;
    }
}

static void json_node(rp_buf_t *b, jnode_t *n) {
    if (n->val) {
        json_value(b, n->val);
        return;
    }
    // Keys sorted by bytes, which for UTF-8 is code point order - as Python sorts str.
    for (size_t i = 1; i < n->count; ++i) {
        for (size_t j = i; j > 0 && strcmp(n->kids[j - 1].name, n->kids[j].name) > 0; --j) {
            jnode_t t = n->kids[j];
            n->kids[j] = n->kids[j - 1];
            n->kids[j - 1] = t;
        }
    }
    rp_buf_byte(b, '{');
    for (size_t k = 0; k < n->count; ++k) {
        if (k) rp_buf_puts(b, ", ");
        json_string(b, n->kids[k].name, strlen(n->kids[k].name));
        rp_buf_puts(b, ": ");
        json_node(b, &n->kids[k]);
    }
    rp_buf_byte(b, '}');
}

proven_err_t rp_toml_dump_json(const rp_tdoc_t *doc, proven_allocator_t alloc, uint8_t **out, size_t *len) {
    if (doc == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    jnode_t root = { 0 };
    bool nomem = false;
    for (size_t k = 0; k < doc->root.count && !nomem; ++k) {
        jnode_t *leaf = jchild(alloc, &root, doc->root.keys[k].key);
        if (leaf == NULL) nomem = true;
        else leaf->val = &doc->root.keys[k].val;
    }
    for (size_t t = 0; t < doc->count && !nomem; ++t) {
        const rp_ttable_t *x = &doc->tables[t];
        jnode_t *n = jchild(alloc, &root, x->kind);
        if (n && x->id) n = jchild(alloc, n, x->id);
        for (size_t k = 0; n && k < x->count; ++k) {
            jnode_t *leaf = jchild(alloc, n, x->keys[k].key);
            if (leaf == NULL) break;
            leaf->val = &x->keys[k].val;
        }
        if (n == NULL) nomem = true;
    }
    rp_buf_t b = rp_buf_new(alloc, (size_t)1 << 28);
    if (!nomem) json_node(&b, &root);
    jfree(alloc, &root);
    if (nomem) {
        rp_buf_free(&b);
        return PROVEN_ERR_NOMEM;
    }
    return rp_buf_take(&b, out, len);
}
