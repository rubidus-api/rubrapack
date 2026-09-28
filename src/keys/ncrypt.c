// src/keys/ncrypt.c - keys in the Windows certificate store (RFC-0011 P9b; RFC-0001 12): the
// certificate is found by its SHA-1 thumbprint in "My", its private key opened through
// CryptAcquireCertificatePrivateKey as an NCrypt key (a smart card, token or TPM key through its
// key storage provider, or a software key), and only NCryptSignHash runs there. The chain comes
// from CertGetCertificateChain, without the root.

#include "rubrapack/keys.h"
#include "rubrapack/buf.h"
#include "rubrapack/crypto.h"
#include "rubrapack/der.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)

proven_err_t rp_ncrypt_open(proven_allocator_t alloc, const char *thumbprint, bool machine, rp_keyfile_t *kf, const char **why) {
    (void)alloc, (void)thumbprint, (void)machine, (void)kf;
    *why = "--key-store is a Windows certificate store; elsewhere use a key file or --pkcs11";
    return PROVEN_ERR_UNSUPPORTED;
}

proven_err_t rp_ncrypt_list(proven_allocator_t alloc, void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx, const char **why) {
    (void)alloc, (void)visit, (void)ctx;
    *why = "without --pkcs11, `keys list` shows the Windows certificate store";
    return PROVEN_ERR_UNSUPPORTED;
}

#else

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

typedef struct {
    proven_allocator_t alloc;
    NCRYPT_KEY_HANDLE  key;
    bool               free_key, ec;
    size_t             ec_size;
} nk_t;

static char msg[300];

static void nk_close(void *ctx) {
    nk_t *k = ctx;
    if (k == NULL) return;
    if (k->key && k->free_key) NCryptFreeObject(k->key);
    rp_mem_free(k->alloc, k);
}

static proven_err_t nk_sign(void *ctx, int alg, const uint8_t *digest, uint8_t *sig, size_t *sig_len, const char **why) {
    nk_t *k = ctx;
    DWORD hl = (DWORD)rp_hash_size((rp_hash_alg_t)alg), got = 0;
    uint8_t raw[1024];
    SECURITY_STATUS st;
    if (k->ec) {
        st = NCryptSignHash(k->key, NULL, (PBYTE)digest, hl, raw, sizeof raw, &got, 0);
    } else {
        BCRYPT_PKCS1_PADDING_INFO pad = { alg == RP_HASH_SHA384 ? BCRYPT_SHA384_ALGORITHM : alg == RP_HASH_SHA512 ? BCRYPT_SHA512_ALGORITHM
                                                                                                   : BCRYPT_SHA256_ALGORITHM };
        st = NCryptSignHash(k->key, &pad, (PBYTE)digest, hl, raw, sizeof raw, &got, BCRYPT_PAD_PKCS1);
    }
    if (st != ERROR_SUCCESS) {
        snprintf(msg, sizeof msg, "NCryptSignHash failed (0x%08lX)%s", (unsigned long)st,
                 st == (SECURITY_STATUS)0x8010006E ? ": the user cancelled the PIN prompt" : "");
        *why = msg;
        return PROVEN_ERR_IO;
    }
    if (!k->ec) {
        if (got > *sig_len) return PROVEN_ERR_OUT_OF_BOUNDS;
        memcpy(sig, raw, got);
        *sig_len = got;
        return PROVEN_OK;
    }
    // ECDSA: NCrypt gives r || s; the file holds DER SEQUENCE { r, s }.
    if (got != 2 * k->ec_size) {
        *why = "NCrypt returned an ECDSA signature of the wrong length";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    rp_buf_t rs = rp_buf_new(k->alloc, 256), seq = rp_buf_new(k->alloc, 256);
    rp_der_put_uint(&rs, raw, k->ec_size);
    rp_der_put_uint(&rs, raw + k->ec_size, k->ec_size);
    rp_der_wrap(&seq, RP_DER_SEQUENCE, &rs);
    proven_err_t err = seq.err;
    if (err == PROVEN_OK && seq.len > *sig_len) err = PROVEN_ERR_OUT_OF_BOUNDS;
    if (err == PROVEN_OK) {
        memcpy(sig, seq.data, seq.len);
        *sig_len = seq.len;
    }
    rp_buf_free(&seq);
    return err;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

proven_err_t rp_ncrypt_open(proven_allocator_t alloc, const char *thumbprint, bool machine, rp_keyfile_t *kf, const char **why) {
    memset(kf, 0, sizeof *kf);
    kf->alloc = alloc;
    uint8_t sha1[20];
    size_t tl = 0;
    for (const char *p = thumbprint; *p; ++p) {
        if (*p == ' ' || *p == ':') continue;
        int h = hexv(*p), l = p[1] ? hexv(p[1]) : -1;
        if (h < 0 || l < 0 || tl == 20) {
            *why = "--key-store takes the certificate's SHA-1 thumbprint (40 hex digits)";
            return PROVEN_ERR_INVALID_ARG;
        }
        sha1[tl++] = (uint8_t)(h << 4 | l);
        ++p;
    }
    if (tl != 20) {
        *why = "--key-store takes the certificate's SHA-1 thumbprint (40 hex digits)";
        return PROVEN_ERR_INVALID_ARG;
    }
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     (machine ? CERT_SYSTEM_STORE_LOCAL_MACHINE : CERT_SYSTEM_STORE_CURRENT_USER) | CERT_STORE_READONLY_FLAG, L"MY");
    if (store == NULL) {
        *why = "cannot open the certificate store";
        return PROVEN_ERR_IO;
    }
    CRYPT_HASH_BLOB hb = { 20, sha1 };
    PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &hb, NULL);
    proven_err_t err = PROVEN_OK;
    nk_t *k = NULL;
    if (cert == NULL) {
        *why = machine ? "no certificate with that thumbprint in the local machine's \"My\" store"
                       : "no certificate with that thumbprint in the current user's \"My\" store (--machine-store for the machine's)";
        err = PROVEN_ERR_NOT_FOUND;
    }
    if (err == PROVEN_OK) {
        k = rp_mem_alloc(alloc, 1, sizeof *k);
        if (k == NULL) err = PROVEN_ERR_NOMEM;
        else memset(k, 0, sizeof *k), k->alloc = alloc;
    }
    if (err == PROVEN_OK) {
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE h = 0;
        DWORD spec = 0;
        BOOL must_free = FALSE;
        if (!CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG, NULL, &h, &spec, &must_free) ||
            spec != CERT_NCRYPT_KEY_SPEC) {
            snprintf(msg, sizeof msg, "the certificate has no private key NCrypt can use (0x%08lX)", (unsigned long)GetLastError());
            *why = msg;
            err = PROVEN_ERR_NOT_FOUND;
        } else {
            k->key = (NCRYPT_KEY_HANDLE)h;
            k->free_key = must_free != FALSE;
        }
    }
    if (err == PROVEN_OK) err = rp_keyfile_add_certs(kf, cert->pbCertEncoded, cert->cbCertEncoded, why);
    if (err == PROVEN_OK) {
        rp_cert_t c;
        if (!rp_cert_parse(kf->certs[0], kf->cert_len[0], &c, why)) {
            err = PROVEN_ERR_INVALID_FORMAT;
        } else if (c.ec_curve >= 0) {
            k->ec = true;
            k->ec_size = rp_ec_size((rp_ec_curve_t)c.ec_curve);
            kf->ec = true;
            kf->ec_curve = c.ec_curve;
        }
    }
    // The chain the system builds, without the root (as Windows' signer includes it).
    if (err == PROVEN_OK) {
        CERT_CHAIN_PARA para;
        memset(&para, 0, sizeof para);
        para.cbSize = sizeof para;
        PCCERT_CHAIN_CONTEXT chain = NULL;
        if (CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore, &para, 0, NULL, &chain) && chain->cChain > 0) {
            PCERT_SIMPLE_CHAIN sc = chain->rgpChain[0];
            for (DWORD i = 1; err == PROVEN_OK && i < sc->cElement; ++i) {
                PCCERT_CONTEXT e = sc->rgpElement[i]->pCertContext;
                if (CertCompareCertificateName(X509_ASN_ENCODING, &e->pCertInfo->Subject, &e->pCertInfo->Issuer)) continue;
                err = rp_keyfile_add_certs(kf, e->pbCertEncoded, e->cbCertEncoded, why);
            }
        }
        if (chain) CertFreeCertificateChain(chain);
    }
    if (cert) CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    if (err != PROVEN_OK) {
        nk_close(k);
        rp_keyfile_free(kf);
        return err;
    }
    kf->ext_sign = nk_sign;
    kf->ext_free = nk_close;
    kf->ext_ctx = k;
    kf->ext_leaf = 0;
    return PROVEN_OK;
}

proven_err_t rp_ncrypt_list(proven_allocator_t alloc, void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx, const char **why) {
    (void)alloc;
    static const struct {
        DWORD       where;
        const char *name;
    } stores[] = { { CERT_SYSTEM_STORE_CURRENT_USER, "CurrentUser\\My" }, { CERT_SYSTEM_STORE_LOCAL_MACHINE, "LocalMachine\\My" } };
    for (int s = 0; s < 2; ++s) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, stores[s].where | CERT_STORE_READONLY_FLAG, L"MY");
        if (store == NULL) continue;
        PCCERT_CONTEXT c = NULL;
        while ((c = CertEnumCertificatesInStore(store, c)) != NULL) {
            DWORD sz = 0;
            if (!CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, NULL, &sz)) continue;      // no private key
            uint8_t sha1[20];
            DWORD hl = sizeof sha1;
            char hex[41] = "";
            if (CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, sha1, &hl)) {
                for (int i = 0; i < 20; ++i) snprintf(hex + 2 * i, 3, "%02x", sha1[i]);
            }
            rp_cert_t pc;
            const char *kind = "other";
            if (rp_cert_parse(c->pbCertEncoded, c->cbCertEncoded, &pc, NULL)) kind = pc.rsa ? "RSA" : pc.ec_curve >= 0 ? "ECDSA" : "other";
            rp_key_entry_t e = { stores[s].name, "", hex, kind, c->pbCertEncoded, c->cbCertEncoded };
            visit(ctx, &e);
        }
        CertCloseStore(store, 0);
    }
    *why = NULL;
    return PROVEN_OK;
}

#endif
