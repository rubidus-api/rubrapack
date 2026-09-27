#ifndef RUBRAPACK_PKI_H
#define RUBRAPACK_PKI_H

// include/rubrapack/pki.h - certificates (X.509, RFC 5280) and key files (PKCS#8 RFC 5208/5958,
// PEM RFC 7468, PKCS#12 RFC 7292 with PBES2/PBKDF2 RFC 8018) for signing (RFC-0007 1, 5.2).
// Supported profile (RFC-0001 12.3.1): RSA keys; PBES2 with PBKDF2-HMAC-SHA256/384/512 and
// AES-128/192/256-CBC; PKCS#12 MAC with SHA-256/384/512 through the RFC 7292 appendix B KDF.
// Old algorithms (3DES, RC2, SHA-1 MACs) and EC keys (until RFC-0007 S1 enables them) are
// refused with a message that says so.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/crypto.h"
#include "rubrapack/der.h"

typedef struct {
    rp_der_span_t der;          // the whole certificate
    rp_der_span_t tbs;          // tbsCertificate, the signed part (whole element)
    rp_der_span_t serial;       // the serialNumber INTEGER element (whole)
    rp_der_span_t issuer;       // Name elements (whole)
    rp_der_span_t subject;
    rp_der_span_t spki;         // SubjectPublicKeyInfo (whole)
    bool          rsa;          // an RSA public key
    rp_der_span_t rsa_n, rsa_e; // its magnitude bytes
    rp_der_span_t sig_alg;      // signatureAlgorithm (whole)
    rp_der_span_t sig;          // signature bits
    int64_t       not_before, not_after;    // seconds since 1970 (UTC)
    bool          has_ku, ku_sign, ku_cert_sign;
    bool          has_eku, eku_code, eku_time, eku_any;
    bool          ca;
    bool          unknown_critical;         // a critical extension we do not understand
} rp_cert_t;

// Parses one DER certificate; `why` says what is wrong on failure. The spans point into `der`.
[[nodiscard]] bool rp_cert_parse(const uint8_t *der, size_t len, rp_cert_t *c, const char **why);

// A private key with its certificates, loaded from a PKCS#12 (.pfx/.p12), PEM or DER PKCS#8 file.
typedef struct {
    proven_allocator_t alloc;
    uint8_t           *key_der;     // the RSAPrivateKey (owned, wiped on free)
    size_t             key_len;
    rp_rsa_key_t       rsa;         // points into key_der
    uint8_t          **certs;       // DER certificates in file order (owned)
    size_t            *cert_len;
    size_t             cert_count;
} rp_keyfile_t;

// Loads a key file. `pass` may be NULL for an unencrypted file. Errors: PROVEN_ERR_PERMISSION for
// a wrong password (MAC or padding check failed), PROVEN_ERR_UNSUPPORTED for an algorithm outside
// the profile, PROVEN_ERR_INVALID_FORMAT for anything malformed; `why` explains.
[[nodiscard]] proven_err_t rp_keyfile_load(proven_allocator_t alloc, const uint8_t *data, size_t len, const uint8_t *pass,
                                           size_t pass_len, rp_keyfile_t *out, const char **why);
// Adds the certificates of a PEM or DER file (a chain given apart from the key).
[[nodiscard]] proven_err_t rp_keyfile_add_certs(rp_keyfile_t *kf, const uint8_t *data, size_t len, const char **why);
void rp_keyfile_free(rp_keyfile_t *kf);

// The certificate whose public key is the key's, or -1.
[[nodiscard]] int rp_keyfile_leaf(const rp_keyfile_t *kf);

#endif // RUBRAPACK_PKI_H
