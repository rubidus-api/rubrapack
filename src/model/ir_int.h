// src/model/ir_int.h - what the files of the .rpk model (src/model/ir*.c) share; not a public header.

#ifndef RUBRAPACK_IR_INT_H
#define RUBRAPACK_IR_INT_H

#include "rubrapack/buf.h"
#include "rubrapack/ident.h"
#include "rubrapack/ir.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/pe.h"
#include "rubrapack/text.h"
#include "rubrapack/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    proven_allocator_t     alloc;
    const rp_tdoc_t       *doc;
    const rp_ir_options_t *opt;
    rp_srcdiags_t         *d;
    rp_ir_t               *ir;
    const rp_ttable_t     *define;
    bool                   nomem;
    int                    fmt;     // IR_FMT_*: what ir_subst may put in
    size_t                 dir_cap, file_cap;
} ctx_t;

enum { IR_FMT_NONE, IR_FMT_INSTALL, IR_FMT_RUNTIME };

#define ERR(c, pos, code, ...) rp_srcdiag_add((c)->d, (pos), (code), false, __VA_ARGS__)

// ir_common.c
char *ir_dup_n(ctx_t *c, const char *s, size_t n);
char *ir_dup(ctx_t *c, const char *s);
const char *ir_suggest(const char *word, const char *const *choices);
bool ir_valid_id(const char *s, size_t max);
bool ir_check_id(ctx_t *c, const rp_ttable_t *t, size_t max);
const char *ir_lang_of_key(const char *key, const char *base);
void ir_check_keys_lang(ctx_t *c, const rp_ttable_t *t, const char *const *allowed, const char *const *lang_bases);
void ir_check_keys(ctx_t *c, const rp_ttable_t *t, const char *const *allowed);
const rp_tkey_t *ir_find_key(const rp_ttable_t *t, const char *key);
bool ir_msix_output(const ctx_t *c);
char *ir_subst(ctx_t *c, const rp_tval_t *v);
char *ir_get_str(ctx_t *c, const rp_ttable_t *t, const char *key, bool required, bool *present);
char *ir_get_fmt(ctx_t *c, const rp_ttable_t *t, const char *key, bool required, bool *present, int fmt);
char *ir_get_path(ctx_t *c, const rp_ttable_t *t, const char *key, bool required);
bool ir_windows_name(const char *s);
void ir_check_format(ctx_t *c);
void ir_get_ltexts(ctx_t *c, const rp_ttable_t *t, const char *base, rp_ir_ltext_t **out, size_t *count);
bool ir_get_bool(ctx_t *c, const rp_ttable_t *t, const char *key, bool dflt);
int64_t ir_get_int(ctx_t *c, const rp_ttable_t *t, const char *key, int64_t dflt, int64_t lo, int64_t hi);
rp_pos_t ir_key_pos(const rp_ttable_t *t, const char *key);
bool ir_is_hex(char ch);
bool ir_guid_ok(char *s);
bool ir_ascii_only(const char *s);
bool ir_has_control(const char *s);
bool ir_target_name_ok(ctx_t *c, const char *name, rp_pos_t pos);
void ir_fold(const char *s, char *out, size_t cap);
bool ir_known_folder(const char *s);

// ir_files.c
rp_ir_dir_t *ir_push_dir(ctx_t *c);
rp_ir_file_t *ir_push_file(ctx_t *c);
bool ir_parse_version(const char *s, uint16_t parts[4], size_t *count);
void ir_parse_package(ctx_t *c, const rp_ttable_t *t);
void ir_parse_module(ctx_t *c, const rp_ttable_t *t);
void ir_parse_feature(ctx_t *c, const rp_ttable_t *t, rp_ir_feature_t *f);
void ir_parse_dir(ctx_t *c, const rp_ttable_t *t, rp_ir_dir_t *d);
bool ir_source_path_ok(ctx_t *c, const char *s, rp_pos_t pos);
void ir_parse_file(ctx_t *c, const rp_ttable_t *t, rp_ir_file_t *f);
char *ir_join(ctx_t *c, const char *a, const char *b);
void ir_expand_files(ctx_t *c, const rp_ttable_t *t);
void ir_parse_folder(ctx_t *c, const rp_ttable_t *t, rp_ir_folder_t *f);

// ir_tables.c
bool ir_tool_property(const char *s);
void ir_parse_arp(ctx_t *c, const rp_ttable_t *t);
void ir_parse_msix(ctx_t *c, const rp_ttable_t *t);
char *ir_logo_path(ctx_t *c, const rp_ttable_t *t, const char *key, char **shown);
void ir_parse_msix_app(ctx_t *c, const rp_ttable_t *t);
void ir_parse_msix_dep(ctx_t *c, const rp_ttable_t *t);
void ir_parse_property(ctx_t *c, const rp_ttable_t *t, rp_ir_property_t *p);
void ir_parse_action(ctx_t *c, const rp_ttable_t *t, rp_ir_action_t *a);
void ir_parse_registry(ctx_t *c, const rp_ttable_t *t, rp_ir_registry_t *r);
bool ir_shortcut_folder(const char *s);
void ir_parse_shortcut(ctx_t *c, const rp_ttable_t *t, rp_ir_shortcut_t *s);
void ir_parse_remove(ctx_t *c, const rp_ttable_t *t, rp_ir_remove_t *r);
void ir_parse_copy(ctx_t *c, const rp_ttable_t *t, rp_ir_copy_t *cp);
void ir_parse_ini(ctx_t *c, const rp_ttable_t *t, rp_ir_ini_t *x);
char *ir_get_when(ctx_t *c, const rp_ttable_t *t);
char *ir_get_cond(ctx_t *c, const rp_ttable_t *t, const char *key);
void ir_parse_require(ctx_t *c, const rp_ttable_t *t, rp_ir_require_t *r);
void ir_parse_search(ctx_t *c, const rp_ttable_t *t, rp_ir_search_t *x);
int ir_ascii_casecmp(const char *a, const char *b);
void ir_parse_service(ctx_t *c, const rp_ttable_t *t, rp_ir_service_t *x);
void ir_parse_permission(ctx_t *c, const rp_ttable_t *t, rp_ir_permission_t *x);
void ir_parse_font(ctx_t *c, const rp_ttable_t *t, rp_ir_font_t *x);
void ir_parse_merge(ctx_t *c, const rp_ttable_t *t, rp_ir_merge_t *x);
void ir_parse_assoc(ctx_t *c, const rp_ttable_t *t, rp_ir_assoc_t *x);
void ir_parse_protocol(ctx_t *c, const rp_ttable_t *t, rp_ir_protocol_t *x);
void ir_parse_com(ctx_t *c, const rp_ttable_t *t, rp_ir_com_t *x);
void ir_parse_handler(ctx_t *c, const rp_ttable_t *t, rp_ir_handler_t *x);
void ir_parse_menu(ctx_t *c, const rp_ttable_t *t, rp_ir_menu_t *x);
void ir_menu_files(ctx_t *c);
void ir_parse_msix_ext(ctx_t *c, const rp_ttable_t *t, rp_ir_msix_ext_t *x);
void ir_parse_env(ctx_t *c, const rp_ttable_t *t, rp_ir_env_t *e);

// ir_ui.c
char *ir_icon_source(ctx_t *c, const char *shown, rp_pos_t pos);
void ir_parse_ui_text(ctx_t *c, const rp_ttable_t *t, rp_ir_ui_text_t *x);
void ir_parse_dialog(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_t *x);
void ir_parse_dialog_control(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_control_t *x);
void ir_dialog_checks(ctx_t *c);
void ir_nfc_names(ctx_t *c);
void ir_ui_checks(ctx_t *c, const rp_ttable_t *uit, const rp_ttable_t *pkg);

// ir_check.c
const rp_ir_dir_t *ir_find_dir(const rp_ir_t *ir, const char *id);
int ir_cmp_str(const char *a, const char *b);
void ir_class_checks(ctx_t *c);
void ir_cross_checks(ctx_t *c);

// ir.c
bool ir_in_list(const char *s, const char *const *list);

#endif // RUBRAPACK_IR_INT_H
