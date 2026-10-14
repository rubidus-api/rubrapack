#ifndef RUBRAPACK_LANG_H
#define RUBRAPACK_LANG_H

// include/rubrapack/lang.h
//
// Languages. A language is one file, a language pack (gettext PO): its header names the language,
// an entry without msgctxt is a text of the command line (msgid: the English text as it is written
// in src/), an entry with msgctxt "installer" a text of the installers rubrapack builds (msgid: the
// text's ID). English is in the code, and what a text without an entry comes out as. The packs of
// resources/lang/ (ko, ja, zh, es, fr, de) are built in; a file found at run time (RUBRAPACK_LANG_DIR,
// lang/ beside the source, lang/ beside the program) comes first, so it adds a language or replaces
// a built-in one.
//
// A command-line translation is not a printf format: it names the English text's conversions by
// number, {1} for the first, and may put them in another order ("{2} 의 {1}"); {-2} says that the
// second has no place in this language (a plural "s"); {{ is a brace. So a wrong pack can leave a
// value out, but never read one that is not there.

#include <stdarg.h>
#include <stddef.h>

#include "rubrapack/diag.h"

// One entry of a pack.
typedef struct {
    const char *ctxt;           // NULL: a command-line text; "installer": a text of the installers
    const char *id, *str;
} rp_lang_row_t;

enum { RP_LANGPACK_LANGIDS = 8 };

typedef struct {
    char           code[16];
    const char    *name;        // X-Rubrapack-Name: the language's own name, or NULL
    const char    *font;        // X-Rubrapack-Font, or NULL
    unsigned       langids[RP_LANGPACK_LANGIDS];    // X-Rubrapack-LangIDs
    size_t         langid_count;
    rp_lang_row_t *rows;
    size_t         count;
} rp_langpack_t;

// The pack of `code` (letters, digits, '-' and '_'), or NULL when there is none: a built-in one,
// then <RUBRAPACK_LANG_DIR>/<code>.po, <near>/lang/<code>.po (`near`: the folder of the source
// being built, or NULL) and lang/<code>.po beside the program. A pack is read once and kept.
const rp_langpack_t *rp_langpack_get(const char *code, const char *near);

// The text of `id` in the pack (ctxt NULL or "installer"), or NULL when the pack has none.
const char *rp_langpack_text(const rp_langpack_t *pack, const char *ctxt, const char *id);

// The language of the command line: "en", or a code with a pack; false otherwise (it stays as it was).
bool rp_lang_set(const char *code);
const char *rp_lang(void);
const rp_langpack_t *rp_lang_pack(void);        // NULL for English

// The text in the chosen language, for one without conversions.
const char *rp_tr(const char *en);

// TR("text"): the text in the chosen language. TR_N("text") only marks a text of a table, which is
// looked up where it is printed (rp_tr): the checker finds both.
#define TR(s)   rp_tr(s)
#define TR_N(s) s

// As vsnprintf / snprintf with the English format, in the chosen language. Returns the length the
// whole text has (it is cut to `cap`, at a byte: the caller trims a partial character).
int rp_lang_vformat(char *out, size_t cap, const char *en_fmt, va_list ap);
[[gnu::format(RP_PRINTF_FORMAT, 3, 4)]]
int rp_lang_format(char *out, size_t cap, const char *en_fmt, ...);

// The packs built into the program (nob embeds resources/lang/*.po).
typedef struct {
    const char          *code;
    const unsigned char *data;
    size_t               len;
} rp_lang_embedded_t;
extern const rp_lang_embedded_t rp_lang_embedded[];
extern const size_t rp_lang_embedded_count;

#endif
