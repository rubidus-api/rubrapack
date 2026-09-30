// src/cli/new_int.h - what `rubrapack new` (new.c) and `rubrapack edit` (edit.c) share (RFC-0014,
// RFC-0015): the program folder scan, the questions and their checks, IDs and TOML strings.
// Not a public header.

#ifndef RUBRAPACK_NEW_INT_H
#define RUBRAPACK_NEW_INT_H

#include "rubrapack/buf.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **v;
    size_t n;
} names_t;

typedef struct {
    names_t files, dirs;    // top-level files; sub folders with files below them (sorted by name)
} scan_t;

typedef struct {
    char stem[128], ext[8], name[256], manufacturer[256], version[32], arch[8], dist[512], main[256], install_dir[256],
        scope[8], ui[16], license[512], languages[8], optional[1024], shortcuts[32];
    scan_t scan;
    bool scanned;
} ans_t;

typedef struct {
    char   v[4096][48];
    size_t n;
} ids_t;

const char *rpn_name_problem(const char *s);
// The length of a source extension at the end of s: ".toml" (the default) or ".rpk" (read too); 0 for none.
size_t rpn_source_ext(const char *s);
char *rpn_join(const char *a, const char *b);
int rpn_cmp_name(const void *a, const void *b);
void rpn_names_free(names_t *x);
proven_err_t rpn_scan_dist(const char *dist, scan_t *s);
void rpn_scan_free(scan_t *s);
bool rpn_in_names(const names_t *x, const char *s);
bool rpn_ends_ci(const char *s, const char *suffix);
const char *rpn_pe_arch(const char *path);
void rpn_find_license(const char *dist, char *out, size_t cap);

// Checks of one answer (NULL = fine; may normalise it in place).
const char *rpn_check_product(ans_t *a, char *v);
const char *rpn_check_version(ans_t *a, char *v);
const char *rpn_check_arch(ans_t *a, char *v);
const char *rpn_check_scope(ans_t *a, char *v);
const char *rpn_check_ui(ans_t *a, char *v);
const char *rpn_check_dist(ans_t *a, char *v);
const char *rpn_check_main(ans_t *a, char *v);
const char *rpn_check_folder(ans_t *a, char *v);
const char *rpn_check_optional(ans_t *a, char *v);
const char *rpn_check_license(ans_t *a, char *v);
const char *rpn_check_languages(ans_t *a, char *v);

// One question on stderr, the answer from stdin (Enter = dflt); 0 when answered.
int rpn_ask(ans_t *a, const char *question, const char *dflt, char *out, size_t cap, const char *(*check)(ans_t *, char *));
int rpn_ask_yes(ans_t *a, const char *question, bool dflt, bool *yes);

bool rpn_id_used(const ids_t *ids, const char *s);
const char *rpn_make_id(ids_t *ids, const char *name, const char *suffix, char *out, size_t cap);
void rpn_toml_str(rp_buf_t *b, const char *s);

#endif // RUBRAPACK_NEW_INT_H
