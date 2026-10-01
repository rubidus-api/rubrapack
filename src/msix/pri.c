// src/msix/pri.c - resources.pri (include/rubrapack/pri.h, RFC-0016 3; format notes F8).
//
// A PRI file: "mrm_pri2", its size, a table of contents, sections, "de fa ff de" + size +
// "mrm_pri2". Every section is a 16-byte name, 16 bytes with its length, the data (to a multiple of
// 8 bytes), "de fa f5 de" + length. The sections written here, in this order:
//   [mrm_decn_info]   qualifiers (Language, Scale), qualifier sets (one qualifier each, or none),
//                     decisions (the ordered sets a resource's candidates have);
//   [mrm_pridescex]   which section is which;
//   [mrm_hschemaex]   the resource names as a tree of scopes (Resources, Files, Assets) and items;
//   [mrm_res_map2_]   per item its decision and candidates, each pointing at a data item;
//   [mrm_dataitem]    the strings and paths, one section per qualifier set.
// Worked out from makepri.exe's output and checked with its dump of our files (T112).

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/pri.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_QUALS = 64, MAX_NAMES = 1024 };

static const struct { const char *scale; uint16_t score; } scales[] = {
    { "100", 1000 }, { "125", 937 }, { "150", 875 }, { "200", 750 }, { "400", 437 },
};

typedef struct {
    int         type;           // RP_PRI_LANGUAGE or RP_PRI_SCALE
    const char *value;
    uint16_t    score;          // how well it does as the default, in thousandths
} qual_t;

// A node of the name tree: a scope (with children) or an item.
typedef struct node {
    char         name[128];
    bool         item;
    int          parent;        // node index
    int          entry, scope_index, item_index;
    int          children[MAX_NAMES / 4];
    int          nchildren;
    char         full[512];
} node_t;

typedef struct {
    proven_allocator_t alloc;
    node_t            *nodes;
    int                nnodes;
} tree_t;

static int ascii_icmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || x == 0) return x - y;
    }
}

static bool is_ascii(const char *s) {
    for (; *s; ++s) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

// The node for `name` ("A/B/c"), made on the way; `item` for the last part.
static int tree_add(tree_t *t, const char *name, bool item) {
    int cur = 0;
    const char *p = name;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        bool last = slash == NULL;
        if (n == 0 || n >= sizeof t->nodes[0].name) return -1;
        int found = -1;
        for (int k = 0; k < t->nodes[cur].nchildren; ++k) {
            const node_t *c = &t->nodes[t->nodes[cur].children[k]];
            if (strlen(c->name) == n && strncmp(c->name, p, n) == 0) found = t->nodes[cur].children[k];
        }
        if (found >= 0) {
            if (t->nodes[found].item != (last && item)) return -1;       // a name both scope and item
        } else {
            if (t->nnodes == MAX_NAMES || t->nodes[cur].nchildren == (int)(sizeof t->nodes[0].children / sizeof(int))) return -1;
            found = t->nnodes++;
            node_t *c = &t->nodes[found];
            memset(c, 0, sizeof *c);
            memcpy(c->name, p, n);
            c->item = last && item;
            c->parent = cur;
            size_t pl = strlen(t->nodes[cur].full);
            if (pl + 1 + n >= sizeof c->full) return -1;
            memcpy(c->full, t->nodes[cur].full, pl);
            if (cur) c->full[pl++] = '/';
            memcpy(c->full + pl, c->name, n + 1);
            t->nodes[cur].children[t->nodes[cur].nchildren++] = found;
        }
        cur = found;
        p = last ? p + n : slash + 1;
    }
    return cur;
}

static void sort_children(tree_t *t, int n) {
    node_t *x = &t->nodes[n];
    for (int i = 1; i < x->nchildren; ++i) {
        for (int j = i; j > 0 && ascii_icmp(t->nodes[x->children[j - 1]].name, t->nodes[x->children[j]].name) > 0; --j) {
            int s = x->children[j];
            x->children[j] = x->children[j - 1];
            x->children[j - 1] = s;
        }
    }
    for (int i = 0; i < x->nchildren; ++i) {
        if (!t->nodes[x->children[i]].item) sort_children(t, x->children[i]);
    }
}

// Scope and item numbers in depth-first order over the sorted tree.
static void number(tree_t *t, int n, int *scopes, int *items, int *order_scopes, int *order_items) {
    node_t *x = &t->nodes[n];
    if (x->item) {
        order_items[*items] = n;
        x->item_index = (*items)++;
        return;
    }
    order_scopes[*scopes] = n;
    x->scope_index = (*scopes)++;
    for (int i = 0; i < x->nchildren; ++i) number(t, x->children[i], scopes, items, order_scopes, order_items);
}

// ---- sections ----

static void pad8(rp_buf_t *b) {
    while (b->len % 8) rp_buf_byte(b, 0);
}

static void put_u16s(rp_buf_t *b, const char *utf8) {
    proven_u16 w[512];
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)utf8, strlen(utf8), w, 511);
    size_t n = r.err == PROVEN_OK ? r.units : 0;
    for (size_t i = 0; i < n; ++i) rp_buf_u16le(b, w[i]);
    rp_buf_u16le(b, 0);
}

static size_t u16_len(const char *utf8) {
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)utf8, strlen(utf8), NULL, 0);
    return r.err == PROVEN_OK ? r.units : 0;
}

typedef struct {
    const char *name;           // 16 bytes as stored
    rp_buf_t    data;
} section_t;

proven_err_t rp_pri_write(proven_allocator_t alloc, const char *identity, const char *default_language,
                          const rp_pri_candidate_t *cands, size_t count, uint8_t **out, size_t *len) {
    return rp_pri_write_part(alloc, identity, default_language, cands, count, NULL, out, len);
}

proven_err_t rp_pri_write_part(proven_allocator_t alloc, const char *identity, const char *default_language,
                               const rp_pri_candidate_t *cands, size_t count, const char *part, uint8_t **out, size_t *len) {
    if (identity == NULL || default_language == NULL || (cands == NULL && count) || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    const size_t limit = (size_t)1 << 26;
    // Which candidates this file carries (RFC-0019): all; the main package's part (no language but
    // the default one); or one language's (a resource package). Every name stays in the schema.
    if (count > 4096) return PROVEN_ERR_OUT_OF_BOUNDS;
    bool use[4096];
    for (size_t i = 0; i < count; ++i) {
        bool lang = cands[i].qualifier == RP_PRI_LANGUAGE && cands[i].qvalue;
        use[i] = part == NULL || (part[0] == '\0' ? !lang || ascii_icmp(cands[i].qvalue, default_language) == 0
                                                   : lang && ascii_icmp(cands[i].qvalue, part) == 0);
    }

    // Qualifiers: languages (the default first), then scales from small to large.
    qual_t q[MAX_QUALS + 1];
    int nq = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            if (cands[i].qualifier == RP_PRI_NONE || !use[i]) continue;
            if (cands[i].qvalue == NULL) return PROVEN_ERR_INVALID_ARG;
            bool lang = cands[i].qualifier == RP_PRI_LANGUAGE;
            if ((pass == 0) != lang) continue;
            bool have = false;
            for (int k = 1; k <= nq; ++k) have |= q[k].type == cands[i].qualifier && strcmp(q[k].value, cands[i].qvalue) == 0;
            if (have) continue;
            if (nq == MAX_QUALS) return PROVEN_ERR_OUT_OF_BOUNDS;
            uint16_t score = 0;
            if (lang) {
                score = ascii_icmp(cands[i].qvalue, default_language) == 0 ? 1000 : 0;
            } else {
                bool ok = false;
                for (size_t s = 0; s < sizeof scales / sizeof scales[0]; ++s) {
                    if (strcmp(scales[s].scale, cands[i].qvalue) == 0) {
                        score = scales[s].score;
                        ok = true;
                    }
                }
                if (!ok) return PROVEN_ERR_INVALID_ARG;
            }
            q[++nq] = (qual_t){ cands[i].qualifier, cands[i].qvalue, score };
        }
        if (pass == 0) {        // the default language first
            for (int k = 2; k <= nq; ++k) {
                if (q[k].score == 1000 && q[1].score != 1000) {
                    qual_t s = q[1];
                    q[1] = q[k];
                    q[k] = s;
                }
            }
        } else {                // scales in size order
            for (int a = 1; a <= nq; ++a) {
                for (int b = a + 1; b <= nq; ++b) {
                    if (q[a].type == RP_PRI_SCALE && q[b].type == RP_PRI_SCALE && atoi(q[b].value) < atoi(q[a].value)) {
                        qual_t s = q[a];
                        q[a] = q[b];
                        q[b] = s;
                    }
                }
            }
        }
    }
    int qindex[4096];
    if (count > sizeof qindex / sizeof qindex[0]) return PROVEN_ERR_OUT_OF_BOUNDS;
    for (size_t i = 0; i < count; ++i) {
        qindex[i] = 0;
        for (int k = 1; k <= nq; ++k) {
            if (cands[i].qualifier != RP_PRI_NONE && q[k].type == cands[i].qualifier && strcmp(q[k].value, cands[i].qvalue) == 0) qindex[i] = k;
        }
    }

    // The name tree.
    tree_t t = { .alloc = alloc };
    t.nodes = rp_mem_alloc(alloc, MAX_NAMES, sizeof *t.nodes);
    int *order_scopes = rp_mem_alloc(alloc, MAX_NAMES, sizeof(int)), *order_items = rp_mem_alloc(alloc, MAX_NAMES, sizeof(int));
    int *node_of = rp_mem_alloc(alloc, count + 1, sizeof(int));
    proven_err_t err = t.nodes && order_scopes && order_items && node_of ? PROVEN_OK : PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK) {
        memset(&t.nodes[0], 0, sizeof t.nodes[0]);
        t.nnodes = 1;
    }
    for (size_t i = 0; err == PROVEN_OK && i < count; ++i) {
        if (cands[i].name == NULL || cands[i].value == NULL || !is_ascii(cands[i].name)) err = PROVEN_ERR_INVALID_ARG;
        else if ((node_of[i] = tree_add(&t, cands[i].name, true)) < 0) err = PROVEN_ERR_INVALID_ARG;
    }
    int nscopes = 0, nitems = 0;
    if (err == PROVEN_OK) {
        sort_children(&t, 0);
        number(&t, 0, &nscopes, &nitems, order_scopes, order_items);
        // Entries: the root, then each scope's children (sorted) in scope order.
        int e = 0;
        t.nodes[0].entry = e++;
        for (int s = 0; s < nscopes; ++s) {
            const node_t *x = &t.nodes[order_scopes[s]];
            for (int i = 0; i < x->nchildren; ++i) t.nodes[x->children[i]].entry = e++;
        }
    }

    // Per item: its candidates in decision order (worst default first; ties: later qualifier first).
    int *cand_of = rp_mem_alloc(alloc, count + 1, sizeof(int)), *first = rp_mem_alloc(alloc, (size_t)nitems + 1, sizeof(int));
    int *ncand = rp_mem_alloc(alloc, (size_t)nitems + 1, sizeof(int)), *decision = rp_mem_alloc(alloc, (size_t)nitems + 1, sizeof(int));
    int (*dec_sets)[MAX_QUALS + 1] = rp_mem_alloc(alloc, (size_t)nitems + 3, sizeof *dec_sets);
    int *dec_n = rp_mem_alloc(alloc, (size_t)nitems + 3, sizeof(int));
    if (err == PROVEN_OK && (!cand_of || !first || !ncand || !decision || !dec_sets || !dec_n)) err = PROVEN_ERR_NOMEM;
    int nc = 0, ndec = 2;
    for (int it = 0; err == PROVEN_OK && it < nitems; ++it) {
        first[it] = nc;
        for (size_t i = 0; i < count; ++i) {
            if (node_of[i] != order_items[it] || !use[i]) continue;
            int k = nc++;
            while (k > first[it]) {
                int a = qindex[cand_of[k - 1]], b = qindex[i];
                int sa = a ? q[a].score : 0, sb = b ? q[b].score : 0;
                if (sa < sb || (sa == sb && a > b)) break;
                cand_of[k] = cand_of[k - 1];
                --k;
            }
            cand_of[k] = (int)i;
        }
        ncand[it] = nc - first[it];
        if (ncand[it] == 0) {           // not in this part: no decision (decision 0)
            decision[it] = 0;
            continue;
        }
        bool unq = false, qual = false;
        for (int k = first[it]; k < nc; ++k) {
            unq |= qindex[cand_of[k]] == 0;
            qual |= qindex[cand_of[k]] != 0;
            for (int j = first[it]; j < k; ++j) {
                if (qindex[cand_of[j]] == qindex[cand_of[k]]) err = PROVEN_ERR_INVALID_ARG;     // two candidates, one set
            }
        }
        if (unq && qual) err = PROVEN_ERR_INVALID_ARG;
        if (unq) {
            decision[it] = 1;
            continue;
        }
        int d = -1;
        for (int k = 2; k < ndec && d < 0; ++k) {
            bool same = dec_n[k] == ncand[it];
            for (int j = 0; same && j < ncand[it]; ++j) same = dec_sets[k][j] == qindex[cand_of[first[it] + j]];
            if (same) d = k;
        }
        if (d < 0) {
            d = ndec++;
            dec_n[d] = ncand[it];
            for (int j = 0; j < ncand[it]; ++j) dec_sets[d][j] = qindex[cand_of[first[it] + j]];
        }
        decision[it] = d;
    }

    // Data item sections: one per qualifier set in use (by set number, the empty set last).
    int section_of_set[MAX_QUALS + 1], nsec = 0, set_order[MAX_QUALS + 1];
    for (int s = 0; s <= nq; ++s) section_of_set[s] = -1;
    for (int pass = 0; pass < 2; ++pass) {
        for (int s = pass ? 0 : 1; pass ? s == 0 : s <= nq; ++s) {
            bool used = false;
            for (int k = 0; err == PROVEN_OK && k < nc; ++k) used |= qindex[cand_of[k]] == s;
            if (used) {
                section_of_set[s] = nsec;
                set_order[nsec++] = s;
            }
        }
    }
    section_t sec[4 + MAX_QUALS + 1];
    size_t nsections = 4 + (size_t)nsec;
    for (size_t s = 0; s < nsections; ++s) sec[s].data = rp_buf_new(alloc, limit);
    sec[0].name = "[mrm_decn_info]\0";
    sec[1].name = "[mrm_pridescex]\0";
    sec[2].name = "[mrm_hschemaex] ";
    sec[3].name = "[mrm_res_map2_]\0";
    for (int s = 0; s < nsec; ++s) sec[4 + s].name = "[mrm_dataitem] \0";

    // [mrm_decn_info]
    if (err == PROVEN_OK) {
        rp_buf_t *b = &sec[0].data;
        int nidx = 1 + nq;
        for (int d = 2; d < ndec; ++d) nidx += dec_n[d];
        size_t chars = 1;
        for (int k = 1; k <= nq; ++k) chars += u16_len(q[k].value) + 1;
        rp_buf_u16le(b, (uint16_t)(nq + 1));
        rp_buf_u16le(b, (uint16_t)(nq + 1));
        rp_buf_u16le(b, (uint16_t)(nq + 1));
        rp_buf_u16le(b, (uint16_t)ndec);
        rp_buf_u16le(b, (uint16_t)nidx);
        rp_buf_u16le(b, (uint16_t)chars);
        // decisions: 0 none, 1 the empty set, then the lists (after the sets' index entries)
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 1);
        int at = 1 + nq;
        for (int d = 2; d < ndec; ++d) {
            rp_buf_u16le(b, (uint16_t)at);
            rp_buf_u16le(b, (uint16_t)dec_n[d]);
            at += dec_n[d];
        }
        // qualifier sets: 0 empty, k = qualifier k alone
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        for (int k = 1; k <= nq; ++k) {
            rp_buf_u16le(b, (uint16_t)k);
            rp_buf_u16le(b, 1);
        }
        // qualifiers: distinct qualifier, priority, score as the default
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        for (int k = 1; k <= nq; ++k) {
            rp_buf_u16le(b, (uint16_t)k);
            rp_buf_u16le(b, q[k].type == RP_PRI_LANGUAGE ? 700 : 200);
            rp_buf_u16le(b, q[k].score);
            rp_buf_u16le(b, 0);
        }
        // distinct qualifiers: type (Language 0, Scale 2) and the value's place in the strings
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 1);
        rp_buf_u32le(b, 0);
        uint32_t off = 1;
        for (int k = 1; k <= nq; ++k) {
            rp_buf_u16le(b, 2);
            rp_buf_u16le(b, q[k].type == RP_PRI_LANGUAGE ? 0 : 2);
            rp_buf_u16le(b, 0);
            rp_buf_u16le(b, 10);
            rp_buf_u32le(b, off);
            off += (uint32_t)u16_len(q[k].value) + 1;
        }
        // the index table: set 0's entry, one per set, then the decisions' lists
        for (int k = 0; k <= nq; ++k) rp_buf_u16le(b, (uint16_t)k);
        for (int d = 2; d < ndec; ++d) {
            for (int j = 0; j < dec_n[d]; ++j) rp_buf_u16le(b, (uint16_t)dec_sets[d][j]);
        }
        rp_buf_u16le(b, 0);
        for (int k = 1; k <= nq; ++k) put_u16s(b, q[k].value);
        pad8(b);
    }

    // [mrm_pridescex]
    if (err == PROVEN_OK) {
        rp_buf_t *b = &sec[1].data;
        static const uint16_t head[] = { 2, 0xFFFF, 0, 1, 1, 1, 3, 0 };
        for (size_t i = 0; i < sizeof head / sizeof head[0]; ++i) rp_buf_u16le(b, head[i]);
        rp_buf_u16le(b, (uint16_t)nsec);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 2);         // the schema
        rp_buf_u16le(b, 0);         // the decisions
        rp_buf_u16le(b, 3);         // the resource map
        for (int s = 0; s < nsec; ++s) rp_buf_u16le(b, (uint16_t)(4 + s));
        pad8(b);
    }

    // [mrm_hschemaex]
    if (err == PROVEN_OK) {
        rp_buf_t *b = &sec[2].data;
        char unique[300];
        snprintf(unique, sizeof unique, "ms-appx://%s/", identity);
        size_t un = u16_len(unique) + 1, nn = u16_len(identity) + 1;
        rp_buf_u16le(b, 1);
        rp_buf_u16le(b, (uint16_t)un);
        rp_buf_u16le(b, (uint16_t)nn);
        rp_buf_u16le(b, 0);
        rp_buf_put(b, "[def_hnamesx]  \0", 16);
        rp_buf_u32le(b, 1);
        rp_buf_u32le(b, 0);
        // A check value over the full names (FNV-1a of the upper-cased paths): makepri writes a
        // value of its own that no reader was seen to check (format notes F8).
        uint32_t check = 2166136261u;
        for (int s = 0; s < t.nnodes; ++s) {
            for (const char *p = t.nodes[s].full; *p; ++p) check = (check ^ (uint8_t)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p)) * 16777619u;
            check = (check ^ '/') * 16777619u;
        }
        rp_buf_u32le(b, check);
        rp_buf_u32le(b, (uint32_t)nscopes);
        rp_buf_u32le(b, (uint32_t)nitems);
        put_u16s(b, unique);
        put_u16s(b, identity);
        while (b->len % 4) rp_buf_byte(b, 0);
        size_t body = b->len;
        // names: ASCII, the root's empty name first, then the scopes', then the items'
        uint32_t *name_off = rp_mem_alloc(alloc, (size_t)t.nnodes + 1, sizeof *name_off);
        if (name_off == NULL) {
            err = PROVEN_ERR_NOMEM;
        } else {
            uint32_t ascii = 1, maxfull = 0;
            name_off[0] = 0;
            for (int s = 1; s < nscopes; ++s) {
                name_off[order_scopes[s]] = ascii;
                ascii += (uint32_t)strlen(t.nodes[order_scopes[s]].name) + 1;
            }
            for (int i = 0; i < nitems; ++i) {
                name_off[order_items[i]] = ascii;
                ascii += (uint32_t)strlen(t.nodes[order_items[i]].name) + 1;
            }
            for (int s = 0; s < t.nnodes; ++s) {
                if (strlen(t.nodes[s].full) > maxfull) maxfull = (uint32_t)strlen(t.nodes[s].full);
            }
            rp_buf_u32le(b, maxfull);
            rp_buf_u32le(b, (uint32_t)t.nnodes);
            rp_buf_u32le(b, (uint32_t)nscopes);
            rp_buf_u32le(b, (uint32_t)nitems);
            rp_buf_u32le(b, 0);                 // UTF-16 name characters: none, the names are ASCII
            size_t size_at = b->len;
            rp_buf_u32le(b, 0);                 // the body's size, below
            rp_buf_u32le(b, ascii);
            // entries in entry order
            int *by_entry = rp_mem_alloc(alloc, (size_t)t.nnodes + 1, sizeof(int));
            if (by_entry == NULL) err = PROVEN_ERR_NOMEM;
            for (int s = 0; by_entry && s < t.nnodes; ++s) by_entry[t.nodes[s].entry] = s;
            for (int e = 0; by_entry && e < t.nnodes; ++e) {
                const node_t *x = &t.nodes[by_entry[e]];
                size_t nl = strlen(x->name);
                rp_buf_u16le(b, (uint16_t)(e ? t.nodes[x->parent].entry : 0));
                rp_buf_u16le(b, (uint16_t)strlen(x->full));
                rp_buf_u16le(b, (uint16_t)(nl ? (x->name[0] >= 'a' && x->name[0] <= 'z' ? x->name[0] - 32 : x->name[0]) : 0));
                rp_buf_byte(b, (uint8_t)nl);
                rp_buf_byte(b, (uint8_t)((x->item ? 0 : 0x10) | (nl ? 0x20 : 0)));
                rp_buf_u16le(b, (uint16_t)name_off[by_entry[e]]);
                rp_buf_u16le(b, (uint16_t)(x->item ? x->item_index : x->scope_index));
            }
            for (int s = 0; s < nscopes; ++s) {
                const node_t *x = &t.nodes[order_scopes[s]];
                rp_buf_u16le(b, (uint16_t)x->entry);
                rp_buf_u16le(b, (uint16_t)x->nchildren);
                rp_buf_u16le(b, (uint16_t)(x->nchildren ? t.nodes[x->children[0]].entry : 0));
                rp_buf_u16le(b, 0);
            }
            for (int i = 0; i < nitems; ++i) rp_buf_u16le(b, (uint16_t)t.nodes[order_items[i]].entry);
            rp_buf_byte(b, 0);
            for (int s = 1; s < nscopes; ++s) rp_buf_put(b, t.nodes[order_scopes[s]].name, strlen(t.nodes[order_scopes[s]].name) + 1);
            for (int i = 0; i < nitems; ++i) rp_buf_put(b, t.nodes[order_items[i]].name, strlen(t.nodes[order_items[i]].name) + 1);
            pad8(b);
            if (b->err == PROVEN_OK) {
                uint32_t size = (uint32_t)(b->len - body);
                for (int k = 0; k < 4; ++k) b->data[size_at + (size_t)k] = (uint8_t)(size >> (8 * k));
            }
            rp_mem_free(alloc, by_entry);
            rp_mem_free(alloc, name_off);
        }
    }

    // [mrm_dataitem] sections, and each candidate's place in them
    int *item_no = rp_mem_alloc(alloc, count + 1, sizeof(int));
    uint8_t *vtype = rp_mem_alloc(alloc, count + 1, 1);
    if (err == PROVEN_OK && (item_no == NULL || vtype == NULL)) err = PROVEN_ERR_NOMEM;
    for (int s = 0; err == PROVEN_OK && s < nsec; ++s) {
        rp_buf_t *b = &sec[4 + s].data, v = rp_buf_new(alloc, limit);
        int n = 0;
        size_t entries_at;
        rp_buf_u32le(b, 0);
        for (int k = 0; k < nc; ++k) n += qindex[cand_of[k]] == set_order[s];
        rp_buf_u16le(b, (uint16_t)n);
        rp_buf_u16le(b, 0);
        size_t len_at = b->len;
        rp_buf_u32le(b, 0);
        entries_at = b->len;
        for (int k = 0; k < n; ++k) rp_buf_u32le(b, 0);
        int j = 0;
        for (int k = 0; k < nc; ++k) {
            int i = cand_of[k];
            if (qindex[i] != set_order[s]) continue;
            while (v.len % 4) rp_buf_byte(&v, 0);
            size_t start = v.len;
            bool ascii = is_ascii(cands[i].value);
            if (ascii) rp_buf_put(&v, cands[i].value, strlen(cands[i].value) + 1);
            else put_u16s(&v, cands[i].value);
            vtype[i] = (uint8_t)(cands[i].path ? (ascii ? 5 : 1) : (ascii ? 3 : 0));
            item_no[i] = j;
            if (b->err == PROVEN_OK) {
                uint8_t *e = b->data + entries_at + 4 * (size_t)j;
                e[0] = (uint8_t)start;
                e[1] = (uint8_t)(start >> 8);
                e[2] = (uint8_t)(v.len - start);
                e[3] = (uint8_t)((v.len - start) >> 8);
            }
            ++j;
        }
        rp_buf_put(b, v.data, v.len);
        pad8(b);
        if (b->err == PROVEN_OK) {
            uint32_t dl = (uint32_t)(b->len - entries_at - 4 * (size_t)n);
            for (int k = 0; k < 4; ++k) b->data[len_at + (size_t)k] = (uint8_t)(dl >> (8 * k));
        }
        if (v.err != PROVEN_OK) err = v.err;
        rp_buf_free(&v);
    }

    // [mrm_res_map2_]
    if (err == PROVEN_OK) {
        rp_buf_t *b = &sec[3].data;
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 2);                 // the schema's section
        rp_buf_u16le(b, 0);
        rp_buf_u16le(b, 0);                 // the decisions' section
        rp_buf_u16le(b, 7);                 // value types
        rp_buf_u16le(b, 1);                 // item-to-group entries
        rp_buf_u16le(b, nitems > 1 ? 1 : 0);        // item groups
        rp_buf_u32le(b, (uint32_t)nitems);
        rp_buf_u32le(b, (uint32_t)nc);
        rp_buf_u32le(b, 0);
        rp_buf_u32le(b, 0);
        for (uint32_t k = 0; k < 7; ++k) {  // String, Path, EmbeddedData, AsciiString, Utf8String, AsciiPath, Utf8Path
            rp_buf_u32le(b, 4);
            rp_buf_u32le(b, k);
        }
        rp_buf_u16le(b, 0);                 // from item 0: the group (or the item info) 0
        rp_buf_u16le(b, 0);
        if (nitems > 1) {
            rp_buf_u16le(b, (uint16_t)nitems);
            rp_buf_u16le(b, 0);
        }
        for (int it = 0; it < nitems; ++it) {
            rp_buf_u16le(b, (uint16_t)decision[it]);
            rp_buf_u16le(b, (uint16_t)first[it]);
        }
        for (int k = 0; k < nc; ++k) {
            int i = cand_of[k];
            rp_buf_byte(b, 1);
            rp_buf_byte(b, vtype[i]);
            rp_buf_u16le(b, 0);
            rp_buf_u16le(b, (uint16_t)item_no[i]);
            rp_buf_u16le(b, (uint16_t)(4 + section_of_set[qindex[i]]));
        }
        pad8(b);
    }

    // The file: header, table of contents, sections, footer.
    rp_buf_t f = rp_buf_new(alloc, limit);
    if (err == PROVEN_OK) {
        size_t toc = 32, start = toc + 32 * nsections, total = start + 16;
        for (size_t s = 0; s < nsections; ++s) {
            if (sec[s].data.err != PROVEN_OK) err = sec[s].data.err;
            total += 40 + sec[s].data.len;
        }
        rp_buf_put(&f, "mrm_pri2", 8);
        rp_buf_u16le(&f, 0);
        rp_buf_u16le(&f, 1);
        rp_buf_u32le(&f, (uint32_t)total);
        rp_buf_u32le(&f, (uint32_t)toc);
        rp_buf_u32le(&f, (uint32_t)start);
        rp_buf_u16le(&f, (uint16_t)nsections);
        rp_buf_u16le(&f, 0xFFFF);
        rp_buf_u32le(&f, 0);
        uint32_t off = 0;
        for (size_t s = 0; s < nsections; ++s) {
            rp_buf_put(&f, sec[s].name, 16);
            rp_buf_u32le(&f, 0);
            rp_buf_u32le(&f, 0);
            rp_buf_u32le(&f, off);
            rp_buf_u32le(&f, (uint32_t)(40 + sec[s].data.len));
            off += (uint32_t)(40 + sec[s].data.len);
        }
        for (size_t s = 0; s < nsections; ++s) {
            uint32_t sl = (uint32_t)(40 + sec[s].data.len);
            rp_buf_put(&f, sec[s].name, 16);
            rp_buf_u32le(&f, 0);
            rp_buf_u32le(&f, 0);
            rp_buf_u32le(&f, sl);
            rp_buf_u32le(&f, 0);
            rp_buf_put(&f, sec[s].data.data, sec[s].data.len);
            rp_buf_put(&f, "\xDE\xFA\xF5\xDE", 4);
            rp_buf_u32le(&f, sl);
        }
        rp_buf_put(&f, "\xDE\xFA\xFF\xDE", 4);
        rp_buf_u32le(&f, (uint32_t)total);
        rp_buf_put(&f, "mrm_pri2", 8);
        if (err == PROVEN_OK && f.err == PROVEN_OK && f.len != total) err = PROVEN_ERR_INVALID_STATE;
    }
    for (size_t s = 0; s < nsections; ++s) rp_buf_free(&sec[s].data);
    rp_mem_free(alloc, t.nodes);
    rp_mem_free(alloc, order_scopes);
    rp_mem_free(alloc, order_items);
    rp_mem_free(alloc, node_of);
    rp_mem_free(alloc, cand_of);
    rp_mem_free(alloc, first);
    rp_mem_free(alloc, ncand);
    rp_mem_free(alloc, decision);
    rp_mem_free(alloc, dec_sets);
    rp_mem_free(alloc, dec_n);
    rp_mem_free(alloc, item_no);
    rp_mem_free(alloc, vtype);
    if (err != PROVEN_OK) {
        rp_buf_free(&f);
        return err;
    }
    return rp_buf_take(&f, out, len);
}
