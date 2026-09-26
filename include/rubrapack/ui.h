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

struct rp_ui {
    rp_msi_wtable_t *tables;
    size_t           table_count;
    rp_ui_prop_t    *props;
    size_t           prop_count;
    rp_ui_seq_t     *seqs;          // InstallUISequence rows
    size_t           seq_count;
    void            *priv;
};

typedef struct {
    const char    *install_dir;     // Directory key the installdir/features sets let the user change
    const uint8_t *license_rtf;     // RTF text for the license dialog, or NULL
    size_t         license_len;
    const uint8_t *banner_bmp;      // user BMPs, or NULL (then a plain white banner)
    size_t         banner_len;
} rp_ui_input_t;

// Builds the tables for ir->ui (RP_UI_BASIC..RP_UI_FEATURES) in ir->language, with ir->ui_texts
// overrides. Free with rp_ui_free.
[[nodiscard]] proven_err_t rp_ui_build(proven_allocator_t alloc, const rp_ir_t *ir, const rp_ui_input_t *in, rp_ui_t **out);
void rp_ui_free(proven_allocator_t alloc, rp_ui_t *ui);

// Whether `id` names a text that [ui-text.ID] may override.
bool rp_ui_text_known(const char *id);

// Plain UTF-8 text -> RTF for the license dialog (ScrollableText): \uN? escapes, one \par per line.
[[nodiscard]] proven_err_t rp_ui_text_to_rtf(proven_allocator_t alloc, const uint8_t *text, size_t len, bool korean,
                                             uint8_t **out, size_t *out_len);

#endif // RUBRAPACK_UI_H
