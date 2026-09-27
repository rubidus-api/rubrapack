#ifndef RUBRAPACK_SIGN_H
#define RUBRAPACK_SIGN_H

// include/rubrapack/sign.h - Authenticode (RFC-0007 1-3): CMS SignedData over an
// SpcIndirectDataContent, for PE files (SpcPeImageData) and, next, MSI (SpcSipInfo). The encoding
// follows what Windows' own signer writes (tests/fixtures/authenticode, made by
// Set-AuthenticodeSignature): SHA-256 digest, rsaEncryption signature, the signed attributes
// SpcSpOpusInfo, contentType, SpcStatementType (individual) and messageDigest, the certificates
// of the key file without self-signed roots, no signing time. A timestamp (RFC-0008) is an RFC 3161
// token in the unsigned attribute 1.3.6.1.4.1.311.3.3.1, over the signature value, as
// mssign32!SignerTimeStampEx2 writes it (tests/fixtures/timestamp).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/cfb.h"
#include "rubrapack/pki.h"

// Checks that the key file can sign code at time `now` (seconds since 1970): an RSA key with a
// matching certificate that has the code-signing EKU, is not a CA, allows digital signatures, is
// valid at `now` and has no critical extension we do not understand. *leaf gets its index.
[[nodiscard]] bool rp_sign_check_key(const rp_keyfile_t *kf, int64_t now, int *leaf, const char **why);

// Gets a timestamp for a signature: `stamp` returns a TimeStampToken (DER, allocated with `alloc`)
// whose imprint is over `sig`, the signature value. rp_tsa_stamp (below) asks a TSA over HTTP.
typedef struct {
    proven_err_t (*stamp)(void *ctx, proven_allocator_t alloc, const uint8_t *sig, size_t sig_len, uint8_t **token,
                          size_t *token_len, const char **why);
    void *ctx;
} rp_timestamper_t;

// Builds the SignedData ContentInfo (DER). `data` is the SpcAttributeTypeAndOptionalValue DER of
// the indirect data (the part that says what kind of file is signed); `digest` the file's digest.
// With `ts`, the token it returns is checked (imprint, signature, genTime inside the signing
// certificate's validity) and added as the unsigned attribute; NULL signs without a timestamp.
[[nodiscard]] proven_err_t rp_authenticode_build(proven_allocator_t alloc, const rp_keyfile_t *kf, int leaf, rp_hash_alg_t alg,
                                                 const uint8_t *data, size_t data_len, const uint8_t *digest,
                                                 const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why);

// What a signature check found; each part apart (RFC-0001 12.6).
typedef struct {
    bool          parsed;       // a SignedData we understand
    bool          digest_ok;    // the file's digest equals the signed one
    bool          attrs_ok;     // messageDigest and contentType agree with the content
    bool          signature_ok; // the signer's signature over the attributes verifies
    rp_hash_alg_t alg;
    rp_der_span_t data;         // the SpcAttributeTypeAndOptionalValue (whole element)
    rp_der_span_t signer_cert;  // the signer's certificate (whole), when found
    rp_der_span_t certs;        // the certificates SET contents
    rp_der_span_t sig;          // the signature value (OCTET STRING contents)
    rp_der_span_t ts_token;     // the RFC 3161 token (whole), when there is one
    bool          ts_legacy;    // an old-style Authenticode timestamp (PKCS#9 counterSignature)
    bool          ts_bad;       // the unsigned attributes are malformed or hold more than one timestamp
} rp_authenticode_check_t;

// Parses a SignedData and checks its attributes and signature; the caller compares the file digest
// (digest_ok is set when `digest` is given and equals the signed one).
void rp_authenticode_verify(const uint8_t *der, size_t len, const uint8_t *digest, rp_authenticode_check_t *r, const char **why);

// ---- PE ------------------------------------------------------------------------------------------

// The Authenticode digest of a PE file (Authenticode PE document): the headers without the
// checksum and the certificate table entry, the sections in file order, and the data after them,
// without the certificate table itself.
[[nodiscard]] bool rp_pe_digest(const uint8_t *pe, size_t len, rp_hash_alg_t alg, uint8_t *digest, const char **why);

// Signs a PE file: pads it to 8 bytes, adds the WIN_CERTIFICATE, points the certificate table at
// it and recomputes the checksum. A file that is signed already is refused.
[[nodiscard]] proven_err_t rp_pe_sign(proven_allocator_t alloc, const uint8_t *pe, size_t len, const rp_keyfile_t *kf, int64_t now,
                                      const rp_timestamper_t *ts, uint8_t **out, size_t *out_len, const char **why);

// Verifies a signed PE file.
void rp_pe_verify(const uint8_t *pe, size_t len, rp_authenticode_check_t *r, const char **why);

// ---- MSI ---------------------------------------------------------------------------------------

// The MSI digests, from the compound file (tests/fixtures/authenticode, worked out against
// Windows' signer - manual/formats/authenticode.md): `ex` = SHA-256 over the root's CLSID and
// state bits and, for every stream in UTF-16LE name order, its name, 8-byte size and two stored
// times; `digest` = hash over ex, the stream contents in the same order, and the root CLSID. The
// two signature streams are left out. A database with storages is refused.
[[nodiscard]] bool rp_msi_digest(const rp_cfb_t *cfb, rp_hash_alg_t alg, bool with_ex, uint8_t ex[32], uint8_t *digest, const char **why);

// Signs an MSI package: DigitalSignature and MsiDigitalSignatureEx streams are added. A package
// that uses cabinets outside itself is refused unless `allow_external_cabs` (RFC-0007 S2: those
// cabinets are then not covered, *external_cabs says so).
[[nodiscard]] proven_err_t rp_msi_sign(proven_allocator_t alloc, const uint8_t *msi, size_t len, const rp_keyfile_t *kf, int64_t now,
                                       const rp_timestamper_t *ts, bool allow_external_cabs, bool *external_cabs, uint8_t **out,
                                       size_t *out_len, const char **why);

// Verifies a signed MSI package (with or without MsiDigitalSignatureEx). The spans in *r point into
// *sig, the signature stream, which the caller frees with rp_mem_free (NULL when there is none).
void rp_msi_verify(proven_allocator_t alloc, const uint8_t *msi, size_t len, rp_authenticode_check_t *r, uint8_t **sig,
                   const char **why);

// ---- trust -------------------------------------------------------------------------------------

// Whether `child` is signed by `issuer`'s key (RSA or ECDSA with SHA-256/384/512) and names it as issuer.
[[nodiscard]] bool rp_cert_signed_by(const rp_cert_t *child, const rp_cert_t *issuer);

// Verifies a signature over `digest` with the certificate's public key: RSA PKCS#1 v1.5, or ECDSA
// with the signature as DER SEQUENCE { r, s }.
[[nodiscard]] bool rp_cert_verify_sig(const rp_cert_t *c, rp_hash_alg_t alg, const uint8_t *digest, const uint8_t *sig, size_t len);

enum { RP_PURPOSE_CODE, RP_PURPOSE_TIMESTAMP };

// Builds a path from the signer's certificate through the signature's certificates to one of the
// trust anchors (DER certificates) and checks it at time `now`: every signature, validity, CA and
// key-cert-sign on issuers, the code-signing EKU on the leaf, no unknown critical extension. A path
// is at most 8 certificates long.
[[nodiscard]] bool rp_chain_trusted(rp_der_span_t signer, rp_der_span_t certs, const rp_der_span_t *anchors, size_t anchor_count,
                                    int64_t now, const char **why);
// The same for another purpose: RP_PURPOSE_TIMESTAMP asks for the time-stamping EKU on the leaf.
[[nodiscard]] bool rp_chain_trusted_for(rp_der_span_t signer, rp_der_span_t certs, const rp_der_span_t *anchors, size_t anchor_count,
                                        int64_t now, int purpose, const char **why);

// ---- RFC 3161 timestamps (src/sign/tsp.c; RFC-0008) -------------------------------------------

typedef struct {
    rp_der_span_t token;        // the TimeStampToken ContentInfo (whole)
    int64_t       gen_time;     // seconds since 1970 (UTC), fraction dropped
    rp_der_span_t tsa_cert;     // the TSA's certificate (whole), from the token
    rp_der_span_t certs;        // the token's certificates SET contents
    rp_hash_alg_t imprint_alg;  // the hash of the message imprint
} rp_tsp_token_t;

// A TimeStampReq (DER) for `digest`: version 1, the imprint, the 8-byte nonce (its top bit is
// cleared so the INTEGER stays positive - pass the same bytes to the checks), certReq TRUE.
[[nodiscard]] proven_err_t rp_tsp_request(proven_allocator_t alloc, rp_hash_alg_t alg, const uint8_t *digest, uint8_t nonce[8],
                                          uint8_t **out, size_t *len);

// Checks a TimeStampResp: status granted, then the token as rp_tsp_token does (nonce required).
[[nodiscard]] bool rp_tsp_response(const uint8_t *resp, size_t len, const uint8_t *data, size_t data_len, const uint8_t nonce[8],
                                   rp_tsp_token_t *t, const char **why);

// Checks a TimeStampToken: the TSTInfo imprint is the hash of `data` (with the imprint's own
// algorithm, *t gets it), the nonce (when given) matches, the
// token's SignedData signature verifies with the TSA certificate it carries, whose extended key
// usage is time stamping and which was valid at genTime. Trust in that certificate is a separate
// question (rp_chain_trusted_for with RP_PURPOSE_TIMESTAMP).
[[nodiscard]] bool rp_tsp_token(const uint8_t *tok, size_t len, const uint8_t *data, size_t data_len, const uint8_t *nonce,
                                rp_tsp_token_t *t, const char **why);

// A timestamp server for rp_timestamper_t: rp_tsa_stamp posts a SHA-256 request with a random
// nonce to `url` (RFC-0008 2 limits), checks the answer with rp_tsp_response and, with anchors
// (--tsa-trust), the TSA's chain for time stamping at genTime. `failed` tells a timestamp failure
// (exit 6) from a signing one; `gen_time` is the token's time.
typedef struct {
    const char          *url, *proxy;
    const rp_der_span_t *anchors;
    size_t               anchor_count;
    bool                 failed;
    int64_t              gen_time;
} rp_tsa_t;
[[nodiscard]] proven_err_t rp_tsa_stamp(void *tsa, proven_allocator_t alloc, const uint8_t *sig, size_t sig_len, uint8_t **token,
                                        size_t *token_len, const char **why);

#endif // RUBRAPACK_SIGN_H
