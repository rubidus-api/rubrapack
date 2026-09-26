#ifndef RUBRAPACK_IDENT_H
#define RUBRAPACK_IDENT_H

// include/rubrapack/ident.h - deterministic identities (DECISIONS 2026-09-26 "P2 identity rules").

#include <stddef.h>

#include "proven/types.h"

// UUIDv8 (RFC 9562 5.8) from SHA-256 over: u32 LE length + `tag`, u32 LE format version 1, then
// for each field u32 LE length + its UTF-8 bytes. Written "{XXXXXXXX-XXXX-8XXX-YXXX-XXXXXXXXXXXX}",
// upper case, into out[39].
void rp_uuid_derive(const char *tag, const char *const *fields, size_t count, char out[39]);

// A random UUIDv4 from the OS random source, same text form.
[[nodiscard]] proven_err_t rp_uuid_random(char out[39]);

// `prefix` + '_' + the first 20 lower-case hex digits of SHA-256(text): out[23].
void rp_key_derive(char prefix, const char *text, char out[23]);

#endif // RUBRAPACK_IDENT_H
