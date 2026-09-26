#ifndef RUBRAPACK_TOML_H
#define RUBRAPACK_TOML_H

// include/rubrapack/toml.h - the strict TOML 1.0 subset that `.rpk` files are written in
// (RFC-0002 section 1). The parser builds a small AST - tables, keys, values, positions - and
// reports everything outside the subset with the RFC-0002 codes. Meaning (which tables and keys
// exist, types, substitution) is checked later, on the AST.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/srcdiag.h"

typedef enum {
    RP_TV_STRING,
    RP_TV_INT,
    RP_TV_BOOL,
    RP_TV_ARRAY,
} rp_tkind_t;

typedef struct rp_tval {
    rp_tkind_t      kind;
    rp_pos_t        pos;
    char           *str;        // RP_TV_STRING: UTF-8 after TOML unescaping, NUL-terminated
    size_t          len;
    bool            literal;    // written as '...'
    int64_t         i;          // RP_TV_INT
    bool            b;          // RP_TV_BOOL
    struct rp_tval *items;      // RP_TV_ARRAY
    size_t          count;
} rp_tval_t;

typedef struct {
    char     *key;              // bare key, NUL-terminated
    rp_pos_t  pos;
    rp_tval_t val;
} rp_tkey_t;

typedef struct {
    char      *kind;            // first header segment
    char      *id;              // second segment, or NULL for a one-segment header
    rp_pos_t   pos;
    rp_tkey_t *keys;            // in file order
    size_t     count;
    size_t     cap;
} rp_ttable_t;

typedef struct {
    proven_allocator_t alloc;
    rp_ttable_t       *tables;  // in file order
    size_t             count;
    size_t             cap;
} rp_tdoc_t;

// Parses `data` (the raw file bytes: UTF-8 with or without BOM, or UTF-16LE with BOM). On any
// error the diagnostics say why and the function returns PROVEN_ERR_INVALID_FORMAT; the document
// is then empty. Allocation failure returns PROVEN_ERR_NOMEM.
[[nodiscard]] proven_err_t rp_toml_parse(proven_allocator_t alloc, const uint8_t *data, size_t len, rp_tdoc_t *doc,
                                         rp_srcdiags_t *diags);
void rp_toml_free(rp_tdoc_t *doc);

// The document as JSON, byte for byte what Python's
// `json.dumps(tomllib.loads(text), sort_keys=True, ensure_ascii=False)` prints: the test oracle.
[[nodiscard]] proven_err_t rp_toml_dump_json(const rp_tdoc_t *doc, proven_allocator_t alloc, uint8_t **out, size_t *len);

#endif // RUBRAPACK_TOML_H
