#ifndef RUBRAPACK_LANG_H
#define RUBRAPACK_LANG_H

// include/rubrapack/lang.h
//
// The language of what the command line prints for a person to read: diagnostics, help, results.
// The texts are written in English where they are used; another language has one table of
// { English, translation } pairs (src/core/lang_ko.c), looked up by the English text. English is
// the default, and what a text without a row comes out as.
//
// A translation is not a printf format: it names the English text's conversions by number, {1} for
// the first, and may put them in another order ("{2} 의 {1}"); {-2} says that the second has no place
// in this language (a plural "s"); {{ is a brace. So a wrong row can
// leave a value out, but never read one that is not there.

#include <stdarg.h>
#include <stddef.h>

#include "rubrapack/diag.h"

// "en" or "ko"; false for any other code (the language stays as it was).
bool rp_lang_set(const char *code);
const char *rp_lang(void);

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

// The table of one language (src/core/lang_ko.c).
typedef struct {
    const char *en, *tr;
} rp_lang_row_t;
extern const rp_lang_row_t rp_lang_ko[];
extern const size_t rp_lang_ko_count;

#endif
