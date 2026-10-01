// src/model/ir_ui.c - dialogs, languages, [ui] checks and NFC names (RFC-0005, RFC-0012).

#include "ir_int.h"

// ---- dialogs (RFC-0005) ------------------------------------------------------------------------

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k) {
        char a = s[n - m + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != suffix[k]) return false;
    }
    return true;
}

static char *ui_source(ctx_t *c, const char *shown, rp_pos_t pos);

char *ir_icon_source(ctx_t *c, const char *shown, rp_pos_t pos) {
    if (!ends_with_ci(shown, ".ico")) {
        ERR(c, pos, "RP1316", "icon must be an .ico file");
        return NULL;
    }
    return ui_source(c, shown, pos);
}

// A source file named by a key (license, banner): relative, existing, a regular file.
static char *ui_source(ctx_t *c, const char *shown, rp_pos_t pos) {
    if (!ir_source_path_ok(c, shown, pos)) return NULL;
    char *path = ir_join(c, c->opt->source_dir, shown);
    uint64_t size = 0;
    if (path && rp_pal_stat(c->alloc, path, &size) != RP_FS_FILE) {
        ERR(c, pos, "RP1509", "cannot read '%s'", shown);
        rp_mem_free(c->alloc, path);
        return NULL;
    }
    return path;
}

void ir_parse_ui_text(ctx_t *c, const rp_ttable_t *t, rp_ir_ui_text_t *x) {
    static const char *const keys[] = { "text", NULL }, *const lkeys[] = { "text", NULL };
    ir_check_keys_lang(c, t, keys, lkeys);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    ir_get_ltexts(c, t, "text", &x->by_lang, &x->by_lang_count);
    x->text = ir_get_str(c, t, "text", x->by_lang_count == 0, NULL);
    if (!rp_ui_text_known(t->id)) ERR(c, t->pos, "RP1201", "[ui-text.%s]: no dialog text has this ID", t->id);
}

static bool public_property(const char *s) {
    bool upper = s[0] != '\0' && strlen(s) <= 72 && !(s[0] >= '0' && s[0] <= '9');
    for (const char *p = s; *p; ++p) upper &= (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
    return upper && !ir_tool_property(s);
}

// Names the built-in frame gives every page (rubrapack/ui.h); an author's control cannot take them.
static bool frame_control(const char *s) {
    static const char *const names[] = { "Banner", "Title", "Description", "BannerLine", "BottomLine", "Back", "Next",
                                         "Cancel", NULL };
    return ir_in_list(s, names);
}

void ir_parse_dialog(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_t *x) {
    static const char *const keys[] = { "title", "description", "after", NULL }, *const lkeys[] = { "title", "description", NULL };
    ir_check_keys_lang(c, t, keys, lkeys);
    ir_get_ltexts(c, t, "title", &x->title_by_lang, &x->title_by_lang_count);
    ir_get_ltexts(c, t, "description", &x->description_by_lang, &x->description_by_lang_count);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    if (!ir_check_id(c, t, 72)) return;
    if (strncmp(t->id, "Rp", 2) == 0 || strcmp(t->id, "FilesInUse") == 0) {
        ERR(c, t->pos, "RP1302", "dialog IDs starting with 'Rp' (and FilesInUse) are rubrapack's own");
    }
    x->title = ir_get_str(c, t, "title", false, NULL);         // the banner heading; [ProductName] when omitted
    x->description = ir_get_str(c, t, "description", false, NULL);
    x->after = ir_get_str(c, t, "after", true, NULL);
}

void ir_parse_dialog_control(ctx_t *c, const rp_ttable_t *t, rp_ir_dialog_control_t *x) {
    static const char *const keys[] = { "dialog", "type", "x", "y", "width", "height", "text", "property", "values",
                                        "labels", NULL }, *const lkeys[] = { "text", "labels", NULL };
    ir_check_keys_lang(c, t, keys, lkeys);
    ir_get_ltexts(c, t, "text", &x->text_by_lang, &x->text_by_lang_count);
    x->id = ir_dup(c, t->id);
    x->pos = t->pos;
    if (!ir_check_id(c, t, 50)) return;
    if (frame_control(t->id)) ERR(c, t->pos, "RP1302", "'%s' is a control of the built-in frame; choose another ID", t->id);
    x->dialog = ir_get_str(c, t, "dialog", true, NULL);
    char *type = ir_get_str(c, t, "type", true, NULL);
    static const char *const types[] = { "text", "checkbox", "edit", "radio", "combo", NULL };
    x->type = -1;
    for (int k = 0; type && types[k]; ++k) {
        if (strcmp(type, types[k]) == 0) x->type = k;
    }
    if (type && x->type < 0) ERR(c, ir_key_pos(t, "type"), "RP1316", "type must be text, checkbox, edit, radio or combo (got '%s')", type);
    rp_mem_free(c->alloc, type);
    static const char *const req[] = { "x", "y", "width", "height" };
    for (int k = 0; k < 4; ++k) {
        if (!ir_find_key(t, req[k])) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs '%s'", t->id, req[k]);
    }
    // The body between the banner line (44) and the button line (234) of a 370 x 270 page.
    x->x = (int)ir_get_int(c, t, "x", 0, 0, 369);
    x->y = (int)ir_get_int(c, t, "y", 45, 45, 233);
    x->width = (int)ir_get_int(c, t, "width", 1, 1, 370);
    x->height = (int)ir_get_int(c, t, "height", 1, 1, 189);
    if (x->x + x->width > 370) ERR(c, ir_key_pos(t, "width"), "RP1308", "x + width must be at most 370 (the page width)");
    if (x->y + x->height > 234) ERR(c, ir_key_pos(t, "height"), "RP1308", "y + height must be at most 234 (the line above the buttons)");
    bool has_text = false;
    x->text = ir_get_str(c, t, "text", false, &has_text);
    x->property = ir_get_str(c, t, "property", false, NULL);
    if (x->type == RP_DC_TEXT || x->type == RP_DC_CHECKBOX) {
        if (!has_text && x->text_by_lang_count == 0) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'text'", t->id);
    } else if (x->type >= 0 && (has_text || x->text_by_lang_count)) {
        ERR(c, ir_key_pos(t, "text"), "RP1316", "a %s control has no text; put a text control beside it", x->type == RP_DC_EDIT ? "edit" : x->type == RP_DC_RADIO ? "radio" : "combo");
    }
    if (x->type == RP_DC_TEXT) {
        if (x->property) ERR(c, ir_key_pos(t, "property"), "RP1316", "a text control has no property");
    } else if (x->type >= 0) {
        if (x->property == NULL) ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'property'", t->id);
        else if (!public_property(x->property))
            ERR(c, ir_key_pos(t, "property"), "RP1310", "property '%s' must be a public name of your own (upper case)", x->property);
    }
    const rp_tkey_t *vk = ir_find_key(t, "values"), *lk = ir_find_key(t, "labels");
    bool list = x->type == RP_DC_RADIO || x->type == RP_DC_COMBO;
    if (!list) {
        if (vk) ERR(c, vk->pos, "RP1316", "only radio and combo controls take values");
        if (lk) ERR(c, lk->pos, "RP1316", "only radio and combo controls take labels");
        for (size_t k = 0; k < t->count; ++k) {
            if (ir_lang_of_key(t->keys[k].key, "labels")) ERR(c, t->keys[k].pos, "RP1316", "only radio and combo controls take labels");
        }
        return;
    }
    if (vk == NULL) {
        ERR(c, t->pos, "RP1202", "[dialog-control.%s] needs 'values'", t->id);
        return;
    }
    if (vk->val.kind != RP_TV_ARRAY || vk->val.count == 0 || vk->val.count > 32) {
        ERR(c, vk->pos, "RP1316", "values is an array of 1 to 32 strings");
        return;
    }
    if (lk && (lk->val.kind != RP_TV_ARRAY || lk->val.count != vk->val.count)) {
        ERR(c, lk->pos, "RP1316", "labels is an array of strings, one per value");
        lk = NULL;
    }
    x->values = rp_mem_alloc(c->alloc, vk->val.count, sizeof *x->values);
    x->labels = rp_mem_alloc(c->alloc, vk->val.count, sizeof *x->labels);
    if (x->values == NULL || x->labels == NULL) {
        c->nomem = true;
        return;
    }
    for (size_t k = 0; k < vk->val.count; ++k) {
        const rp_tval_t *v = &vk->val.items[k], *l = lk ? &lk->val.items[k] : NULL;
        if (v->kind != RP_TV_STRING || (l && l->kind != RP_TV_STRING)) {
            ERR(c, vk->pos, "RP1316", "values and labels are strings");
            break;
        }
        char *val = ir_subst(c, v);
        if (val && (val[0] == '\0' || strlen(val) > 64)) ERR(c, vk->pos, "RP1316", "a value is 1 to 64 bytes");
        for (size_t j = 0; val && j < x->value_count; ++j) {
            if (strcmp(x->values[j], val) == 0) ERR(c, vk->pos, "RP1316", "value '%s' is listed twice", val);
        }
        x->values[x->value_count] = val;
        x->labels[x->value_count] = l ? ir_subst(c, l) : ir_dup(c, val ? val : "");
        ++x->value_count;
    }
    // labels-xx: the labels in one language, one per value (RFC-0012).
    size_t nl = 0;
    for (size_t k = 0; k < t->count; ++k) nl += ir_lang_of_key(t->keys[k].key, "labels") != NULL;
    if (nl) {
        x->labels_by_lang = rp_mem_alloc(c->alloc, nl, sizeof *x->labels_by_lang);
        if (x->labels_by_lang == NULL) {
            c->nomem = true;
            return;
        }
    }
    for (size_t k = 0; k < t->count; ++k) {
        const char *code = ir_lang_of_key(t->keys[k].key, "labels");
        if (code == NULL) continue;
        const rp_tval_t *a = &t->keys[k].val;
        if (a->kind != RP_TV_ARRAY || a->count != x->value_count) {
            ERR(c, t->keys[k].pos, "RP1316", "%s is an array of strings, one per value", t->keys[k].key);
            continue;
        }
        char **ls = rp_mem_alloc(c->alloc, a->count, sizeof *ls);
        if (ls == NULL) {
            c->nomem = true;
            return;
        }
        for (size_t j = 0; j < a->count; ++j) {
            if (a->items[j].kind != RP_TV_STRING) {
                ERR(c, t->keys[k].pos, "RP1316", "%s holds strings", t->keys[k].key);
                ls[j] = ir_dup(c, "");
            } else {
                ls[j] = ir_subst(c, &a->items[j]);
            }
        }
        x->labels_by_lang[x->labels_by_lang_count++] = (rp_ir_llabels_t){ ir_dup(c, code), ls };
    }
    if (x->type == RP_DC_RADIO && x->height < 12 * (int)x->value_count)
        ERR(c, ir_key_pos(t, "height"), "RP1308", "a radio control stacks its buttons: height must be at least 12 per value (%d)", 12 * (int)x->value_count);
}

static const rp_ir_dialog_t *find_dialog(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        if (ir->dialogs[k].id && strcmp(ir->dialogs[k].id, id) == 0) return &ir->dialogs[k];
    }
    return NULL;
}

static const rp_ir_property_t *find_property(const rp_ir_t *ir, const char *id) {
    for (size_t k = 0; k < ir->property_count; ++k) {
        if (ir->properties[k].id && strcmp(ir->properties[k].id, id) == 0) return &ir->properties[k];
    }
    return NULL;
}

// K4: every page hangs off a page of the chosen set; the values have defaults (RFC-0003 7).
void ir_dialog_checks(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        const rp_ir_dialog_t *d = &ir->dialogs[k];
        if (ir->ui < RP_UI_MINIMAL) {
            ERR(c, d->pos, "RP1316", "[dialog.%s] needs ui = \"minimal\", \"installdir\" or \"features\"", d->id);
            continue;
        }
        if (d->after == NULL) continue;
        bool builtin = strcmp(d->after, "RpWelcomeDlg") == 0 ||
                       (strcmp(d->after, "RpLicenseDlg") == 0 && ir->license_shown) ||
                       (strcmp(d->after, "RpInstallDirDlg") == 0 && ir->ui >= RP_UI_INSTALLDIR) ||
                       (strcmp(d->after, "RpCustomizeDlg") == 0 && ir->ui == RP_UI_FEATURES);
        if (builtin) continue;
        // Another author page: it must exist and the chain must reach a built-in page.
        const rp_ir_dialog_t *p = find_dialog(ir, d->after);
        size_t steps = 0;
        while (p && p->after && find_dialog(ir, p->after) && steps <= ir->dialog_count) {
            p = find_dialog(ir, p->after);
            ++steps;
        }
        if (find_dialog(ir, d->after) == NULL) {
            ERR(c, d->pos, "RP1315", "after = '%s' is not a page of this dialog set (RpWelcomeDlg%s%s%s) or a [dialog.*]", d->after,
                ir->license_shown ? ", RpLicenseDlg" : "", ir->ui >= RP_UI_INSTALLDIR ? ", RpInstallDirDlg" : "",
                ir->ui == RP_UI_FEATURES ? ", RpCustomizeDlg" : "");
        } else if (steps > ir->dialog_count) {
            ERR(c, d->pos, "RP1307", "[dialog.%s] is placed after itself through other dialogs", d->id);
        }
    }
    for (size_t k = 0; k < ir->dialog_control_count; ++k) {
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[k];
        if (x->dialog && find_dialog(ir, x->dialog) == NULL) ERR(c, x->pos, "RP1315", "dialog '%s' is not a [dialog.*]", x->dialog);
        if (x->property == NULL || x->type == RP_DC_TEXT) continue;
        const rp_ir_property_t *p = find_property(ir, x->property);
        if (x->type == RP_DC_RADIO || x->type == RP_DC_COMBO) {
            bool ok = false;
            for (size_t j = 0; p && p->value && j < x->value_count; ++j) ok |= x->values[j] && strcmp(x->values[j], p->value) == 0;
            if (!ok) ERR(c, x->pos, "RP1315", "a %s control needs [property.%s] with one of its values as the default (a silent installation uses it)",
                         x->type == RP_DC_RADIO ? "radio" : "combo", x->property);
        }
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_dialog_control_t *y = &ir->dialog_controls[j];
            if (y->property && strcmp(y->property, x->property) == 0 && y->type != RP_DC_TEXT)
                ERR(c, x->pos, "RP1301", "property '%s' already belongs to control '%s'", x->property, y->id);
        }
    }
    for (size_t k = 0; k < ir->dialog_count; ++k) {
        bool any = false;
        for (size_t j = 0; j < ir->dialog_control_count && !any; ++j) any = ir->dialog_controls[j].dialog && strcmp(ir->dialog_controls[j].dialog, ir->dialogs[k].id) == 0;
        size_t count = 0;
        for (size_t j = 0; j < ir->dialog_control_count; ++j) count += ir->dialog_controls[j].dialog && strcmp(ir->dialog_controls[j].dialog, ir->dialogs[k].id) == 0;
        if (!any) ERR(c, ir->dialogs[k].pos, "RP1202", "[dialog.%s] has no [dialog-control.*]", ir->dialogs[k].id);
        else if (count > 64) ERR(c, ir->dialogs[k].pos, "RP1313", "[dialog.%s] has %zu controls; at most 64", ir->dialogs[k].id, count);
    }
}

// --nfc (RFC-0006 L3): the names the package gives to folders, files and shortcuts in NFC - a
// file from macOS often arrives decomposed. The source files keep their names; texts, registry
// values and the rest stay as written (lint warns about them). The case check that follows sees
// the new names, so two names that become one are refused.
static void nfc_one(ctx_t *c, char **s) {
    if (*s == NULL) return;
    uint8_t *o;
    size_t n;
    proven_err_t err = rp_nfc(c->alloc, (const uint8_t *)*s, strlen(*s), &o, &n);
    if (err == PROVEN_ERR_NOMEM) {
        c->nomem = true;
        return;
    }
    if (err != PROVEN_OK) return;       // not UTF-8: already refused where the name was read
    rp_mem_free(c->alloc, *s);
    *s = (char *)o;
}

void ir_nfc_names(ctx_t *c) {
    rp_ir_t *ir = c->ir;
    for (size_t k = 0; k < ir->dir_count; ++k) {
        for (size_t j = 0; j < ir->dirs[k].part_count; ++j) nfc_one(c, &ir->dirs[k].parts[j]);
    }
    for (size_t k = 0; k < ir->file_count; ++k) nfc_one(c, &ir->files[k].name);
    for (size_t k = 0; k < ir->folder_count; ++k) nfc_one(c, &ir->folders[k].name);
    for (size_t k = 0; k < ir->shortcut_count; ++k) nfc_one(c, &ir->shortcuts[k].name);
    for (size_t k = 0; k < ir->copy_count; ++k) nfc_one(c, &ir->copies[k].name);
}

// Languages the dialogs know without help (RFC-0012): their LANGIDs for the automatic choice, the
// name on the language page and the face. Built-in texts exist for en and ko only; any other
// language gives every text itself ([ui-text.ID] text-xx).
static const struct {
    const char *code, *name, *font;
    uint16_t    ids[6];
} known_langs[] = {
    { "en", "English", "Segoe UI", { 1033, 2057, 3081, 4105, 5129, 6153 } },
    { "ko", "한국어", "맑은 고딕", { 1042 } },
    { "ja", "日本語", "Yu Gothic UI", { 1041 } },
    { "zh", "中文", "Microsoft YaHei UI", { 2052, 1028, 3076, 4100, 5124 } },
    { "de", "Deutsch", "Segoe UI", { 1031, 2055, 3079, 4103, 5127 } },
    { "fr", "Français", "Segoe UI", { 1036, 2060, 3084, 4108, 5132, 6156 } },
    { "es", "Español", "Segoe UI", { 1034, 3082, 2058, 11274, 9226, 13322 } },
    { "it", "Italiano", "Segoe UI", { 1040, 2064 } },
    { "pt", "Português", "Segoe UI", { 1046, 2070 } },
    { "nl", "Nederlands", "Segoe UI", { 1043, 2067 } },
    { "pl", "Polski", "Segoe UI", { 1045 } },
    { "ru", "Русский", "Segoe UI", { 1049 } },
    { "uk", "Українська", "Segoe UI", { 1058 } },
    { "tr", "Türkçe", "Segoe UI", { 1055 } },
    { "vi", "Tiếng Việt", "Segoe UI", { 1066 } },
    { "th", "ไทย", "Leelawadee UI", { 1054 } },
};

static int known_lang(const char *code) {
    for (size_t i = 0; i < sizeof known_langs / sizeof known_langs[0]; ++i) {
        if (strcmp(known_langs[i].code, code) == 0) return (int)i;
    }
    return -1;
}

static int ui_lang_index(const rp_ir_t *ir, const char *code) {
    for (size_t i = 0; i < ir->ui_lang_count; ++i) {
        if (strcmp(ir->ui_langs[i].code, code) == 0) return (int)i;
    }
    return -1;
}

// A license file of the dialogs: .txt/.md shown as text, .rtf as it is.
static bool license_kind_ok(const char *s) { return ends_with_ci(s, ".txt") || ends_with_ci(s, ".rtf") || ends_with_ci(s, ".md"); }

// [ui] languages and its name-xx / font-xx / langid-xx / license-xx (RFC-0012). English is always
// there, first: the dialogs' default.
static void ui_languages(ctx_t *c, const rp_ttable_t *uit) {
    rp_ir_t *ir = c->ir;
    ir->ui_lang_count = 1;
    memcpy(ir->ui_langs[0].code, "en", 3);
    const rp_tkey_t *lk = uit ? ir_find_key(uit, "languages") : NULL;
    if (lk) {
        if (lk->val.kind != RP_TV_ARRAY) ERR(c, lk->pos, "RP1306", "languages is an array of language codes, like [\"ko\"]");
        for (size_t k = 0; lk->val.kind == RP_TV_ARRAY && k < lk->val.count; ++k) {
            const rp_tval_t *v = &lk->val.items[k];
            char *code = v->kind == RP_TV_STRING ? ir_subst(c, v) : NULL;
            size_t n = code ? strlen(code) : 0;
            bool ok = n >= 2 && n <= 3;
            for (size_t i = 0; ok && i < n; ++i) ok = code[i] >= 'a' && code[i] <= 'z';
            if (!ok) {
                ERR(c, lk->pos, "RP1316", "languages holds codes of 2 or 3 lower-case letters, like \"ko\" (got '%s')", code ? code : "?");
            } else if (strcmp(code, "en") == 0) {
                // English is always there
            } else if (ui_lang_index(ir, code) >= 0) {
                ERR(c, lk->pos, "RP1301", "language '%s' is listed twice", code);
            } else if (ir->ui_lang_count == RP_UI_LANG_MAX) {
                ERR(c, lk->pos, "RP1308", "at most %d languages besides English", RP_UI_LANG_MAX - 1);
            } else {
                memcpy(ir->ui_langs[ir->ui_lang_count++].code, code, n + 1);
            }
            rp_mem_free(c->alloc, code);
        }
        if (ir->ui_lang_count > 1 && ir->ui == 0) ERR(c, lk->pos, "RP1316", "languages needs a dialog set (ui = \"basic\" or another)");
    }
    // The per-language keys of [ui].
    for (size_t k = 0; uit && k < uit->count; ++k) {
        const rp_tkey_t *key = &uit->keys[k];
        static const char *const bases[] = { "name", "font", "langid", "license" };
        for (int b = 0; b < 4; ++b) {
            const char *code = ir_lang_of_key(key->key, bases[b]);
            if (code == NULL) continue;
            int li = ui_lang_index(ir, code);
            if (li < 0) {
                ERR(c, key->pos, "RP1316", "'%s': '%s' is not in [ui] languages", key->key, code);
                continue;
            }
            rp_ir_ui_lang_t *L = &ir->ui_langs[li];
            L->pos = key->pos;
            if (b == 2) {           // langid-xx: one LANGID or an array of them
                const rp_tval_t *one = &key->val;
                size_t cnt = one->kind == RP_TV_ARRAY ? one->count : 1;
                for (size_t j = 0; j < cnt; ++j) {
                    const rp_tval_t *v = one->kind == RP_TV_ARRAY ? &one->items[j] : one;
                    if (v->kind != RP_TV_INT || v->i < 1 || v->i > 65535) {
                        ERR(c, key->pos, "RP1308", "%s holds LANGIDs (1..65535), like 1041", key->key);
                        break;
                    }
                    if (L->langid_count < RP_UI_LANGID_MAX) L->langids[L->langid_count++] = (uint16_t)v->i;
                }
                continue;
            }
            if (key->val.kind != RP_TV_STRING) {
                ERR(c, key->pos, "RP1306", "'%s' must be a string", key->key);
                continue;
            }
            char *v = ir_subst(c, &key->val);
            if (b == 0) L->name = v;
            else if (b == 1) {
                // TextStyle.FaceName holds a face name as GDI takes it: at most 31 UTF-16 units.
                size_t units = 0;
                for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; ++p) {
                    if ((*p & 0xC0) != 0x80) units += *p >= 0xF0 ? 2 : 1;
                }
                if (v && (units == 0 || units > 31 || ir_has_control(v))) {
                    ERR(c, key->pos, "RP1308", "%s is a typeface name of 1 to 31 characters, like \"Segoe UI\" (got %zu)", key->key, units);
                }
                L->font = v;
            }
            else {
                if (ir->ui < 2) ERR(c, key->pos, "RP1316", "a license needs ui = \"minimal\", \"installdir\" or \"features\"");
                else if (!license_kind_ok(v)) ERR(c, key->pos, "RP1316", "license must be a .txt, .md (shown as plain text) or .rtf file");
                else L->license_source = ui_source(c, v, key->pos);
                L->license_shown = v;
            }
        }
    }
    // What each language still needs.
    for (size_t i = 0; i < ir->ui_lang_count; ++i) {
        rp_ir_ui_lang_t *L = &ir->ui_langs[i];
        int kl = known_lang(L->code);
        rp_pos_t pos = lk ? lk->pos : (uit ? uit->pos : (rp_pos_t){ 0 });
        if (L->langid_count == 0 && kl >= 0) {
            for (size_t j = 0; j < 6 && known_langs[kl].ids[j]; ++j) L->langids[L->langid_count++] = known_langs[kl].ids[j];
        }
        if (L->langid_count == 0) ERR(c, pos, "RP1202", "language '%s' needs [ui] langid-%s (its LANGIDs, for the automatic choice)", L->code, L->code);
        if (L->name == NULL && kl < 0) ERR(c, pos, "RP1202", "language '%s' needs [ui] name-%s (its name on the language page)", L->code, L->code);
        if (L->name == NULL && kl >= 0) L->name = ir_dup(c, known_langs[kl].name);
        if (L->font == NULL) L->font = ir_dup(c, kl >= 0 ? known_langs[kl].font : "Segoe UI");
        // A license shown as text names its face in RTF, which takes no face name it cannot spell in ASCII.
        bool text_license = L->license_source && !ends_with_ci(L->license_source, ".rtf");
        if (text_license && L->font && rp_ui_face_ascii(L->font) == NULL) {
            char key[16];
            snprintf(key, sizeof key, "font-%s", L->code);
            rp_srcdiag_add(c->d, uit ? ir_key_pos(uit, key) : pos, "RP1319", true,
                           "%s '%s': the license text needs the face's English name, so it shows in the language's own face; "
                           "write the English name (like \"Malgun Gothic\") to use this face there too", key, L->font);
        }
        // Built-in texts are English and Korean; another language writes every one of them.
        if (strcmp(L->code, "en") != 0 && strcmp(L->code, "ko") != 0) {
            size_t missing = 0;
            const char *first = NULL;
            for (size_t t = 0; rp_ui_text_id(t); ++t) {
                const char *id = rp_ui_text_id(t);
                bool have = false;
                for (size_t u = 0; u < ir->ui_text_count && !have; ++u) {
                    const rp_ir_ui_text_t *x = &ir->ui_texts[u];
                    if (strcmp(x->id, id) != 0) continue;
                    have = x->text != NULL;
                    for (size_t w = 0; w < x->by_lang_count && !have; ++w) have = strcmp(x->by_lang[w].lang, L->code) == 0;
                }
                if (!have && missing++ == 0) first = id;
            }
            if (missing) ERR(c, pos, "RP1202", "language '%s' has no built-in texts: give [ui-text.ID] text-%s for all of them (%zu missing, the first is %s)",
                             L->code, L->code, missing, first);
        }
    }
    // text-xx / title-xx / labels-xx name languages of the dialogs.
#define LANG_OK(code, pos)                                                                                             \
    do {                                                                                                               \
        if (ui_lang_index(ir, (code)) < 0) ERR(c, (pos), "RP1316", "language '%s' is not in [ui] languages", (code));  \
    } while (0)
    for (size_t u = 0; u < ir->ui_text_count; ++u) {
        for (size_t w = 0; w < ir->ui_texts[u].by_lang_count; ++w) LANG_OK(ir->ui_texts[u].by_lang[w].lang, ir->ui_texts[u].pos);
    }
    for (size_t u = 0; u < ir->dialog_count; ++u) {
        for (size_t w = 0; w < ir->dialogs[u].title_by_lang_count; ++w) LANG_OK(ir->dialogs[u].title_by_lang[w].lang, ir->dialogs[u].pos);
        for (size_t w = 0; w < ir->dialogs[u].description_by_lang_count; ++w) LANG_OK(ir->dialogs[u].description_by_lang[w].lang, ir->dialogs[u].pos);
    }
    for (size_t u = 0; u < ir->dialog_control_count; ++u) {
        const rp_ir_dialog_control_t *x = &ir->dialog_controls[u];
        for (size_t w = 0; w < x->text_by_lang_count; ++w) LANG_OK(x->text_by_lang[w].lang, x->pos);
        for (size_t w = 0; w < x->labels_by_lang_count; ++w) LANG_OK(x->labels_by_lang[w].lang, x->pos);
    }
#undef LANG_OK
    // 0.1 made Korean dialogs from language = "ko-KR"; the dialogs are English unless Korean is added.
    if (ir->language == 1042 && ir->ui && ui_lang_index(ir, "ko") < 0) {
        rp_srcdiag_add(c->d, uit ? uit->pos : (rp_pos_t){ 0 }, "RP1317", true,
                       "language = \"ko-KR\" no longer makes the dialogs Korean: they are English unless [ui] languages = [\"ko\"] adds Korean");
    }
}

void ir_ui_checks(ctx_t *c, const rp_ttable_t *uit, const rp_ttable_t *pkg) {
    rp_ir_t *ir = c->ir;
    if (ir->license_shown) {
        rp_pos_t pos = ir_key_pos(pkg, "license");
        if (ir->ui == 0 || ir->ui == 1) ERR(c, pos, "RP1316", "a license needs ui = \"minimal\", \"installdir\" or \"features\"");
        else if (!ends_with_ci(ir->license_shown, ".txt") && !ends_with_ci(ir->license_shown, ".rtf") && !ends_with_ci(ir->license_shown, ".md")) {
            ERR(c, pos, "RP1316", "license must be a .txt, .md (shown as plain text) or .rtf file");
        } else {
            ir->license_source = ui_source(c, ir->license_shown, pos);
        }
    }
    ui_languages(c, uit);
    if (uit == NULL) return;
    static const char *const keys[] = { "banner", "install-dir", "languages", "launch", "launch-args", "launch-checked", NULL },
                             *const lkeys[] = { "name", "font", "langid", "license", NULL };
    ir_check_keys_lang(c, uit, keys, lkeys);
    if (ir->ui == 0) ERR(c, uit->pos, "RP1316", "[ui] needs ui = \"basic\" or another dialog set in [package]");
    char *banner = ir_get_str(c, uit, "banner", false, NULL);
    if (banner) {
        if (!ends_with_ci(banner, ".bmp")) ERR(c, ir_key_pos(uit, "banner"), "RP1316", "banner must be a .bmp file (Windows Installer shows BMP only)");
        else ir->banner_source = ui_source(c, banner, ir_key_pos(uit, "banner"));
        rp_mem_free(c->alloc, banner);
    }
    ir->ui_install_dir = ir_get_str(c, uit, "install-dir", false, NULL);
    // RFC-0013 A5: a program started from the finished page, as the user who runs the setup.
    char *launch = ir_get_str(c, uit, "launch", false, NULL);
    if (launch) {
        if (strncmp(launch, "file:", 5) != 0 || launch[5] == '\0') ERR(c, ir_key_pos(uit, "launch"), "RP1315", "launch must be \"file:<ID>\" naming a [file.*] of this package");
        else ir->ui_launch_file = ir_dup(c, launch + 5);
        rp_mem_free(c->alloc, launch);
    }
    ir->ui_launch_args = ir_get_fmt(c, uit, "launch-args", false, NULL, IR_FMT_INSTALL);
    ir->ui_launch_default = ir_get_bool(c, uit, "launch-checked", true);
    if (ir->ui_launch_args && ir->ui_launch_file == NULL) ERR(c, ir_key_pos(uit, "launch-args"), "RP1316", "launch-args needs launch");
}
