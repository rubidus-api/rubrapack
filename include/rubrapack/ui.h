#ifndef RUBRAPACK_UI_H
#define RUBRAPACK_UI_H

// include/rubrapack/ui.h - the built-in dialog sets (RFC-0005): Dialog, Control, ControlEvent,
// ControlCondition, EventMapping, TextStyle, UIText, Binary rows, plus the Property and
// InstallUISequence rows they need. Layout and wording are rubrapack's own, designed from the
// Windows Installer table and control reference only.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/ir.h"
#include "rubrapack/msi.h"

enum { RP_UI_NONE, RP_UI_BASIC, RP_UI_MINIMAL, RP_UI_INSTALLDIR, RP_UI_FEATURES };

typedef struct {
    const char *name, *value;
} rp_ui_prop_t;

typedef struct {
    const char *action, *condition;
    int         sequence;
} rp_ui_seq_t;

typedef struct rp_ui rp_ui_t;

// A property-setting custom action (type 51) of the dialogs (RFC-0012: the chosen language's texts).
typedef struct {
    const char *action, *source, *target;
} rp_ui_ca_t;

struct rp_ui {
    rp_msi_wtable_t *tables;
    size_t           table_count;
    rp_ui_prop_t    *props;
    size_t           prop_count;
    rp_ui_seq_t     *seqs;          // InstallUISequence rows
    size_t           seq_count;
    rp_ui_ca_t      *cas;           // CustomAction rows (type 51), sequenced in seqs
    size_t           ca_count;
    void            *priv;
};

typedef struct {
    const char    *install_dir;     // Directory key the installdir/features sets let the user change
    const uint8_t *license_rtf;     // RTF text for the license dialog, or NULL
    size_t         license_len;
    const uint8_t *banner_bmp;      // user BMPs, or NULL (then a plain white banner)
    size_t         banner_len;
    // RFC-0012: a license per language of ir->ui_langs ([ui] license-xx), or NULL: license_rtf.
    const uint8_t *license_rtf_by_lang[RP_UI_LANG_MAX];
    size_t         license_len_by_lang[RP_UI_LANG_MAX];
} rp_ui_input_t;

// Builds the tables for ir->ui (RP_UI_BASIC..RP_UI_FEATURES) in English, with ir->ui_texts
// overrides; with more than one of ir->ui_langs, every text is a property that the chosen
// language fills (RFC-0012). Free with rp_ui_free.
[[nodiscard]] proven_err_t rp_ui_build(proven_allocator_t alloc, const rp_ir_t *ir, const rp_ui_input_t *in, rp_ui_t **out);
void rp_ui_free(proven_allocator_t alloc, rp_ui_t *ui);

// Whether `id` names a text that [ui-text.ID] may override.
bool rp_ui_text_known(const char *id);

// Text `id` in language `li` of ir->ui_langs: [ui-text.ID] text-xx, its text, or the built-in one.
const char *rp_ui_text_for(const rp_ir_t *ir, const char *id, size_t li);
// The same for a language code, whether or not the dialogs have that language.
const char *rp_ui_text_lang(const rp_ir_t *ir, const char *id, const char *code);

// The i-th of those IDs, or NULL past the last (RFC-0012: another language gives every one).
const char *rp_ui_text_id(size_t i);

// Whether a dialog text can be settled when the package is built (RFC-0012): its only [...] are
// [ProductName], [Manufacturer], [ProductVersion] and [\x] escapes. Other texts are formatted when
// the language is chosen, and must fit a 255-character column.
bool rp_ui_text_static(const char *text);

// A typeface name as the license text's RTF can name it: an ASCII name as it is, the local name of
// a common Korean, Japanese or Chinese face as its English one ("맑은 고딕" -> "Malgun Gothic"), else
// NULL (Windows' rich edit control drops a face name written with \uN escapes).
const char *rp_ui_face_ascii(const char *face);

// Plain UTF-8 text -> RTF for the license dialog (ScrollableText) in the typeface `face` (UTF-8):
// \uN? escapes, one \par per line.
[[nodiscard]] proven_err_t rp_ui_text_to_rtf(proven_allocator_t alloc, const uint8_t *text, size_t len, const char *face,
                                             uint8_t **out, size_t *out_len);

#endif // RUBRAPACK_UI_H
