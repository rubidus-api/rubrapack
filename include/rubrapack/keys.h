#ifndef RUBRAPACK_KEYS_H
#define RUBRAPACK_KEYS_H

// include/rubrapack/keys.h - keys that stay where they are (RFC-0011 P9b; RFC-0001 12, 4.3): a
// PKCS#11 token (rubrapack's own client for the module the user names; POSIX) and a key in the
// Windows certificate store (NCrypt). Hashes, CMS and timestamps stay rubrapack's; only the one
// signature operation goes to the token or store (rp_keyfile_t ext_sign).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/pki.h"

// Opens `module` (a PKCS#11 library), finds the token (by `token_label`, or the only one present),
// logs in with the PIN, finds the private key labelled `key_label` and the certificate with its
// CKA_ID, and fills *kf: certs[0] is that certificate; kf->ext_sign signs with the key (RSA
// PKCS#1 v1.5 or ECDSA). Extra certificates can be added with rp_keyfile_add_certs; when the token
// has no certificate for the key, kf holds none and the first one added is taken as the key's. Free with
// rp_keyfile_free (logs out and unloads the module). Errors: PROVEN_ERR_PERMISSION for a wrong or
// locked PIN, PROVEN_ERR_NOT_FOUND for a missing module, token, key or certificate,
// PROVEN_ERR_UNSUPPORTED on Windows (use rp_ncrypt_open) or for another key type.
[[nodiscard]] proven_err_t rp_pkcs11_open(proven_allocator_t alloc, const char *module, const char *token_label, const char *key_label,
                                          const uint8_t *pin, size_t pin_len, rp_keyfile_t *kf, const char **why);

// One key that can sign, as `keys list` shows it.
typedef struct {
    const char    *where;       // token label, or the store name
    const char    *label;       // key label (PKCS#11), or empty
    const char    *id_hex;      // CKA_ID in hex (PKCS#11), or the certificate's SHA-1 thumbprint (store)
    const char    *kind;        // "RSA", "ECDSA"
    const uint8_t *cert;        // its certificate (DER), or NULL when there is none
    size_t         cert_len;
} rp_key_entry_t;

// Lists the private keys of every token of `module` (after logging in with `pin` when given, else
// what a token shows without a login).
[[nodiscard]] proven_err_t rp_pkcs11_list(proven_allocator_t alloc, const char *module, const char *token_label, const uint8_t *pin,
                                          size_t pin_len, void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx,
                                          const char **why);

// Windows: the certificate with SHA-1 thumbprint `thumbprint` (40 hex digits) in the current user's
// (or, with `machine`, the local machine's) "My" store, whose private key NCrypt can use; its chain
// up to the root (left out) from the system. PROVEN_ERR_UNSUPPORTED elsewhere.
[[nodiscard]] proven_err_t rp_ncrypt_open(proven_allocator_t alloc, const char *thumbprint, bool machine, rp_keyfile_t *kf,
                                          const char **why);
// Windows: the certificates with a private key in the current user's and the local machine's "My" stores.
[[nodiscard]] proven_err_t rp_ncrypt_list(proven_allocator_t alloc, void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx,
                                          const char **why);

#endif // RUBRAPACK_KEYS_H
