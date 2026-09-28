// src/keys/pkcs11.c - rubrapack's own PKCS#11 client (RFC-0011 P9b; RFC-0001 F16). The declarations
// below are written from the OASIS PKCS#11 Base Specification v2.40 (section 3 for the types, 5 for
// the functions; constants as in its published pkcs11t.h) - no header is taken from anywhere.
// Structures use the platform's default alignment, as PKCS#11 modules on Unix are built (the
// specification's 1-byte packing is what Windows modules use; there rubrapack uses NCrypt).

#include "rubrapack/keys.h"
#include "rubrapack/buf.h"
#include "rubrapack/crypto.h"
#include "rubrapack/der.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

proven_err_t rp_pkcs11_open(proven_allocator_t alloc, const char *module, const char *token_label, const char *key_label, const uint8_t *pin,
                            size_t pin_len, rp_keyfile_t *kf, const char **why) {
    (void)alloc, (void)module, (void)token_label, (void)key_label, (void)pin, (void)pin_len, (void)kf;
    *why = "on Windows a token is used through the certificate store (--key-store <thumbprint>)";
    return PROVEN_ERR_UNSUPPORTED;
}

proven_err_t rp_pkcs11_list(proven_allocator_t alloc, const char *module, const char *token_label, const uint8_t *pin, size_t pin_len,
                            void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx, const char **why) {
    (void)alloc, (void)module, (void)token_label, (void)pin, (void)pin_len, (void)visit, (void)ctx;
    *why = "on Windows `keys list` shows the certificate store";
    return PROVEN_ERR_UNSUPPORTED;
}

#else

#include <dlfcn.h>

typedef unsigned long CK_ULONG;
typedef CK_ULONG CK_RV, CK_SLOT_ID, CK_SESSION_HANDLE, CK_OBJECT_HANDLE, CK_FLAGS, CK_ATTRIBUTE_TYPE, CK_MECHANISM_TYPE, CK_USER_TYPE;
typedef unsigned char CK_BBOOL;
typedef struct {
    unsigned char major, minor;
} CK_VERSION;
typedef struct {
    CK_ATTRIBUTE_TYPE type;
    void             *pValue;
    CK_ULONG          ulValueLen;
} CK_ATTRIBUTE;
typedef struct {
    CK_MECHANISM_TYPE mechanism;
    void             *pParameter;
    CK_ULONG          ulParameterLen;
} CK_MECHANISM;
typedef struct {
    void    *CreateMutex, *DestroyMutex, *LockMutex, *UnlockMutex;
    CK_FLAGS flags;
    void    *pReserved;
} CK_C_INITIALIZE_ARGS;
typedef struct {
    unsigned char label[32], manufacturerID[32], model[16], serialNumber[16];
    CK_FLAGS      flags;
    CK_ULONG      ulMaxSessionCount, ulSessionCount, ulMaxRwSessionCount, ulRwSessionCount, ulMaxPinLen, ulMinPinLen;
    CK_ULONG      ulTotalPublicMemory, ulFreePublicMemory, ulTotalPrivateMemory, ulFreePrivateMemory;
    CK_VERSION    hardwareVersion, firmwareVersion;
    unsigned char utcTime[16];
} CK_TOKEN_INFO;

typedef CK_RV (*fn_void)(void);
// CK_FUNCTION_LIST (section 3.6): the version, then the functions in this order.
typedef struct {
    CK_VERSION version;
    CK_RV (*C_Initialize)(void *);
    CK_RV (*C_Finalize)(void *);
    fn_void C_GetInfo, C_GetFunctionList;
    CK_RV (*C_GetSlotList)(CK_BBOOL, CK_SLOT_ID *, CK_ULONG *);
    fn_void C_GetSlotInfo;
    CK_RV (*C_GetTokenInfo)(CK_SLOT_ID, CK_TOKEN_INFO *);
    fn_void C_GetMechanismList, C_GetMechanismInfo, C_InitToken, C_InitPIN, C_SetPIN;
    CK_RV (*C_OpenSession)(CK_SLOT_ID, CK_FLAGS, void *, void *, CK_SESSION_HANDLE *);
    CK_RV (*C_CloseSession)(CK_SESSION_HANDLE);
    fn_void C_CloseAllSessions, C_GetSessionInfo, C_GetOperationState, C_SetOperationState;
    CK_RV (*C_Login)(CK_SESSION_HANDLE, CK_USER_TYPE, const unsigned char *, CK_ULONG);
    CK_RV (*C_Logout)(CK_SESSION_HANDLE);
    fn_void C_CreateObject, C_CopyObject, C_DestroyObject, C_GetObjectSize;
    CK_RV (*C_GetAttributeValue)(CK_SESSION_HANDLE, CK_OBJECT_HANDLE, CK_ATTRIBUTE *, CK_ULONG);
    fn_void C_SetAttributeValue;
    CK_RV (*C_FindObjectsInit)(CK_SESSION_HANDLE, CK_ATTRIBUTE *, CK_ULONG);
    CK_RV (*C_FindObjects)(CK_SESSION_HANDLE, CK_OBJECT_HANDLE *, CK_ULONG, CK_ULONG *);
    CK_RV (*C_FindObjectsFinal)(CK_SESSION_HANDLE);
    fn_void C_EncryptInit, C_Encrypt, C_EncryptUpdate, C_EncryptFinal, C_DecryptInit, C_Decrypt, C_DecryptUpdate, C_DecryptFinal;
    fn_void C_DigestInit, C_Digest, C_DigestUpdate, C_DigestKey, C_DigestFinal;
    CK_RV (*C_SignInit)(CK_SESSION_HANDLE, CK_MECHANISM *, CK_OBJECT_HANDLE);
    CK_RV (*C_Sign)(CK_SESSION_HANDLE, const unsigned char *, CK_ULONG, unsigned char *, CK_ULONG *);
    // The rest of the list (SignUpdate ... WaitForSlotEvent) is not used.
} CK_FUNCTION_LIST;

enum {
    CKR_OK = 0x0, CKR_PIN_INCORRECT = 0xA0, CKR_PIN_INVALID = 0xA1, CKR_PIN_LEN_RANGE = 0xA2, CKR_PIN_EXPIRED = 0xA3,
    CKR_PIN_LOCKED = 0xA4, CKR_TOKEN_NOT_PRESENT = 0xE0, CKR_USER_ALREADY_LOGGED_IN = 0x100, CKR_BUFFER_TOO_SMALL = 0x150,
    CKR_CRYPTOKI_ALREADY_INITIALIZED = 0x191,
    CKF_TOKEN_PRESENT = 0x1, CKF_SERIAL_SESSION = 0x4, CKF_OS_LOCKING_OK = 0x2, CKF_USER_PIN_LOCKED = 0x40000, CKF_TOKEN_INITIALIZED = 0x400,
    CKU_USER = 1,
    CKO_CERTIFICATE = 1, CKO_PRIVATE_KEY = 3,
    CKA_CLASS = 0x0, CKA_LABEL = 0x3, CKA_VALUE = 0x11, CKA_KEY_TYPE = 0x100, CKA_ID = 0x102,
    CKK_RSA = 0, CKK_EC = 3,
    CKM_RSA_PKCS = 0x1, CKM_ECDSA = 0x1041,
};

typedef struct {
    proven_allocator_t alloc;
    void              *lib;
    CK_FUNCTION_LIST  *f;
    CK_SESSION_HANDLE  session;
    CK_OBJECT_HANDLE   key;
    bool               ec, logged_in, initialized, session_open;
    size_t             ec_size;
} p11_t;

static char msg[400];

static const char *rv_text(CK_RV rv) {
    switch (rv) {
    case CKR_PIN_INCORRECT: return "wrong PIN";
    case CKR_PIN_INVALID: case CKR_PIN_LEN_RANGE: return "the PIN has characters or a length the token does not take";
    case CKR_PIN_EXPIRED: return "the PIN has expired: change it on the token first";
    case CKR_PIN_LOCKED: return "the PIN is locked (too many wrong tries)";
    case CKR_TOKEN_NOT_PRESENT: return "the token is not present";
    default: return NULL;
    }
}

static const char *fail_rv(const char *what, CK_RV rv) {
    const char *t = rv_text(rv);
    if (t) snprintf(msg, sizeof msg, "%s: %s", what, t);
    else snprintf(msg, sizeof msg, "%s failed (PKCS#11 error 0x%lX)", what, (unsigned long)rv);
    return msg;
}

static void p11_close(void *ctx) {
    p11_t *p = ctx;
    if (p == NULL) return;
    if (p->logged_in) (void)p->f->C_Logout(p->session);
    if (p->session_open) (void)p->f->C_CloseSession(p->session);
    if (p->initialized) (void)p->f->C_Finalize(NULL);
    if (p->lib) dlclose(p->lib);
    proven_allocator_t a = p->alloc;
    rp_mem_free(a, p);
}

// The DigestInfo that RSA PKCS#1 v1.5 signs (RFC 8017 9.2), for CKM_RSA_PKCS.
static bool digest_info(int alg, const uint8_t *digest, uint8_t *out, size_t *len) {
    static const uint8_t p256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static const uint8_t p384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static const uint8_t p512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    const uint8_t *pre = alg == RP_HASH_SHA256 ? p256 : alg == RP_HASH_SHA384 ? p384 : alg == RP_HASH_SHA512 ? p512 : NULL;
    if (pre == NULL) return false;
    size_t hl = rp_hash_size((rp_hash_alg_t)alg);
    memcpy(out, pre, 19);
    memcpy(out + 19, digest, hl);
    *len = 19 + hl;
    return true;
}

static proven_err_t p11_sign(void *ctx, int alg, const uint8_t *digest, uint8_t *sig, size_t *sig_len, const char **why) {
    p11_t *p = ctx;
    uint8_t in[128], raw[512];
    size_t il = 0;
    CK_MECHANISM m = { p->ec ? CKM_ECDSA : CKM_RSA_PKCS, NULL, 0 };
    if (p->ec) {
        il = rp_hash_size((rp_hash_alg_t)alg);
        memcpy(in, digest, il);
    } else if (!digest_info(alg, digest, in, &il)) {
        *why = "a hash the token signing does not know";
        return PROVEN_ERR_UNSUPPORTED;
    }
    CK_RV rv = p->f->C_SignInit(p->session, &m, p->key);
    CK_ULONG rl = sizeof raw;
    if (rv == CKR_OK) rv = p->f->C_Sign(p->session, in, (CK_ULONG)il, raw, &rl);
    if (rv != CKR_OK) {
        *why = fail_rv("signing on the token", rv);
        return PROVEN_ERR_IO;
    }
    if (!p->ec) {
        if (rl > *sig_len) return PROVEN_ERR_OUT_OF_BOUNDS;
        memcpy(sig, raw, rl);
        *sig_len = rl;
        return PROVEN_OK;
    }
    // ECDSA: r || s from the token, DER SEQUENCE { r, s } in the file (the one conversion, RFC-0001 12).
    if (p->ec_size == 0 && (rl == 64 || rl == 96)) p->ec_size = rl / 2;         // the certificate came from --cert
    if (rl != 2 * p->ec_size) {
        *why = "the token returned an ECDSA signature of the wrong length";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    rp_buf_t rs = rp_buf_new(p->alloc, 256), seq = rp_buf_new(p->alloc, 256);
    rp_der_put_uint(&rs, raw, p->ec_size);
    rp_der_put_uint(&rs, raw + p->ec_size, p->ec_size);
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

// Loads the module and initializes it (a module another part of the process initialized is fine).
static proven_err_t p11_load(proven_allocator_t alloc, const char *module, p11_t **out, const char **why) {
    p11_t *p = rp_mem_alloc(alloc, 1, sizeof *p);
    if (p == NULL) return PROVEN_ERR_NOMEM;
    memset(p, 0, sizeof *p);
    p->alloc = alloc;
    p->lib = dlopen(module, RTLD_NOW | RTLD_LOCAL);
    if (p->lib == NULL) {
        snprintf(msg, sizeof msg, "cannot load the PKCS#11 module '%.300s'", module);
        *why = msg;
        rp_mem_free(alloc, p);
        return PROVEN_ERR_NOT_FOUND;
    }
    CK_RV (*get)(CK_FUNCTION_LIST **) = NULL;
    void *sym = dlsym(p->lib, "C_GetFunctionList");
    memcpy(&get, &sym, sizeof get);
    CK_RV rv = get ? get(&p->f) : 1;
    if (rv != CKR_OK || p->f == NULL || p->f->version.major < 2) {
        *why = "the library is not a PKCS#11 module (no C_GetFunctionList, or version 1)";
        p11_close(p);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    CK_C_INITIALIZE_ARGS args = { NULL, NULL, NULL, NULL, CKF_OS_LOCKING_OK, NULL };
    rv = p->f->C_Initialize(&args);
    if (rv != CKR_OK && rv != CKR_CRYPTOKI_ALREADY_INITIALIZED) {
        *why = fail_rv("C_Initialize", rv);
        p11_close(p);
        return PROVEN_ERR_IO;
    }
    p->initialized = rv == CKR_OK;
    *out = p;
    return PROVEN_OK;
}

static void token_label(const CK_TOKEN_INFO *t, char out[33]) {
    memcpy(out, t->label, 32);
    out[32] = 0;
    for (int i = 31; i >= 0 && out[i] == ' '; --i) out[i] = 0;
}

// The slots with a token (whose label is `want`, when given).
static proven_err_t find_slots(p11_t *p, const char *want, CK_SLOT_ID *slots, CK_ULONG *n, const char **why) {
    CK_SLOT_ID all[64];
    CK_ULONG na = 64;
    CK_RV rv = p->f->C_GetSlotList(1, all, &na);
    if (rv != CKR_OK) {
        *why = fail_rv("C_GetSlotList", rv);
        return PROVEN_ERR_IO;
    }
    *n = 0;
    for (CK_ULONG i = 0; i < na && i < 64; ++i) {
        CK_TOKEN_INFO ti;
        char label[33];
        if (p->f->C_GetTokenInfo(all[i], &ti) != CKR_OK || !(ti.flags & CKF_TOKEN_INITIALIZED)) continue;   // an empty slot to set up
        token_label(&ti, label);
        if (want == NULL || strcmp(label, want) == 0) slots[(*n)++] = all[i];
    }
    if (*n == 0) {
        snprintf(msg, sizeof msg, want ? "no token labelled '%.100s' is present" : "no token is present", want);
        *why = msg;
        return PROVEN_ERR_NOT_FOUND;
    }
    return PROVEN_OK;
}

// Up to `max` objects matching the template.
static CK_ULONG find(p11_t *p, CK_ATTRIBUTE *t, CK_ULONG nt, CK_OBJECT_HANDLE *out, CK_ULONG max) {
    CK_ULONG got = 0;
    if (p->f->C_FindObjectsInit(p->session, t, nt) != CKR_OK) return 0;
    if (p->f->C_FindObjects(p->session, out, max, &got) != CKR_OK) got = 0;
    (void)p->f->C_FindObjectsFinal(p->session);
    return got;
}

// An attribute's value, allocated (NULL when the object has none).
static uint8_t *attr(p11_t *p, CK_OBJECT_HANDLE o, CK_ATTRIBUTE_TYPE type, size_t *len) {
    CK_ATTRIBUTE a = { type, NULL, 0 };
    if (p->f->C_GetAttributeValue(p->session, o, &a, 1) != CKR_OK || a.ulValueLen == (CK_ULONG)-1 || a.ulValueLen > (1u << 20)) return NULL;
    uint8_t *v = rp_mem_alloc(p->alloc, a.ulValueLen + 1, 1);
    if (v == NULL) return NULL;
    a.pValue = v;
    if (p->f->C_GetAttributeValue(p->session, o, &a, 1) != CKR_OK) {
        rp_mem_free(p->alloc, v);
        return NULL;
    }
    v[a.ulValueLen] = 0;
    *len = a.ulValueLen;
    return v;
}

static proven_err_t login(p11_t *p, CK_SLOT_ID slot, const uint8_t *pin, size_t pin_len, const char **why) {
    CK_RV rv = p->f->C_OpenSession(slot, CKF_SERIAL_SESSION, NULL, NULL, &p->session);
    if (rv != CKR_OK) {
        *why = fail_rv("opening a session on the token", rv);
        return PROVEN_ERR_IO;
    }
    p->session_open = true;
    if (pin == NULL) return PROVEN_OK;
    rv = p->f->C_Login(p->session, CKU_USER, pin, (CK_ULONG)pin_len);
    if (rv != CKR_OK && rv != CKR_USER_ALREADY_LOGGED_IN) {
        *why = fail_rv("logging in to the token", rv);
        return rv == CKR_PIN_INCORRECT || rv == CKR_PIN_LOCKED || rv == CKR_PIN_EXPIRED || rv == CKR_PIN_INVALID || rv == CKR_PIN_LEN_RANGE
                   ? PROVEN_ERR_PERMISSION
                   : PROVEN_ERR_IO;
    }
    p->logged_in = rv == CKR_OK;
    return PROVEN_OK;
}

proven_err_t rp_pkcs11_open(proven_allocator_t alloc, const char *module, const char *tlabel, const char *key_label, const uint8_t *pin,
                            size_t pin_len, rp_keyfile_t *kf, const char **why) {
    memset(kf, 0, sizeof *kf);
    kf->alloc = alloc;
    p11_t *p = NULL;
    proven_err_t err = p11_load(alloc, module, &p, why);
    if (err != PROVEN_OK) return err;
    CK_SLOT_ID slots[64];
    CK_ULONG ns = 0;
    err = find_slots(p, tlabel, slots, &ns, why);
    if (err == PROVEN_OK && ns > 1) {
        *why = "more than one token is present: name one with --token-label";
        err = PROVEN_ERR_INVALID_ARG;
    }
    if (err == PROVEN_OK) err = login(p, slots[0], pin, pin_len, why);
    // The private key by label, then its certificate by CKA_ID.
    uint8_t *id = NULL, *type = NULL, *cert = NULL;
    size_t idl = 0, tl = 0, cl = 0;
    if (err == PROVEN_OK) {
        CK_ULONG cls = CKO_PRIVATE_KEY;
        CK_ATTRIBUTE t[2] = { { CKA_CLASS, &cls, sizeof cls }, { CKA_LABEL, (void *)key_label, (CK_ULONG)strlen(key_label) } };
        CK_OBJECT_HANDLE o[2];
        CK_ULONG n = find(p, t, 2, o, 2);
        if (n != 1) {
            snprintf(msg, sizeof msg, n ? "more than one private key is labelled '%.100s'" : "no private key labelled '%.100s' on the token%s", key_label,
                     pin ? "" : " (give the PIN: --pin-env or --pin-file)");
            *why = msg;
            err = PROVEN_ERR_NOT_FOUND;
        } else {
            p->key = o[0];
            id = attr(p, o[0], CKA_ID, &idl);
            type = attr(p, o[0], CKA_KEY_TYPE, &tl);
            if (type == NULL || tl != sizeof(CK_ULONG)) {
                *why = "the token does not say what kind of key it is";
                err = PROVEN_ERR_INVALID_FORMAT;
            }
        }
    }
    if (err == PROVEN_OK) {
        CK_ULONG kt;
        memcpy(&kt, type, sizeof kt);
        if (kt != CKK_RSA && kt != CKK_EC) {
            *why = "the key is neither RSA nor EC";
            err = PROVEN_ERR_UNSUPPORTED;
        }
        p->ec = kt == CKK_EC;
    }
    if (err == PROVEN_OK && id) {
        CK_ULONG cls = CKO_CERTIFICATE;
        CK_ATTRIBUTE t[2] = { { CKA_CLASS, &cls, sizeof cls }, { CKA_ID, id, (CK_ULONG)idl } };
        CK_OBJECT_HANDLE o[1];
        if (find(p, t, 2, o, 1) == 1) cert = attr(p, o[0], CKA_VALUE, &cl);
    }
    if (err == PROVEN_OK) {
        kf->ext_sign = p11_sign;
        kf->ext_free = p11_close;
        kf->ext_ctx = p;
        kf->ext_leaf = 0;
        kf->ec = p->ec;
        if (cert) {
            err = rp_keyfile_add_certs(kf, cert, cl, why);
            rp_cert_t c;
            if (err == PROVEN_OK && rp_cert_parse(kf->certs[0], kf->cert_len[0], &c, why)) {
                if (p->ec && c.ec_curve < 0) {
                    *why = "the token's EC key has a certificate that is not an EC one";
                    err = PROVEN_ERR_INVALID_FORMAT;
                } else if (p->ec) {
                    kf->ec_curve = c.ec_curve;
                    p->ec_size = rp_ec_size((rp_ec_curve_t)c.ec_curve);
                }
            }
        }
        // No certificate on the token: the caller's --cert supplies it (its first certificate).
        
    }
    rp_mem_free(alloc, id);
    rp_mem_free(alloc, type);
    rp_mem_free(alloc, cert);
    if (err != PROVEN_OK) {
        if (kf->ext_ctx == NULL) p11_close(p);
        rp_keyfile_free(kf);
    }
    return err;
}

proven_err_t rp_pkcs11_list(proven_allocator_t alloc, const char *module, const char *tlabel, const uint8_t *pin, size_t pin_len,
                            void (*visit)(void *ctx, const rp_key_entry_t *e), void *ctx, const char **why) {
    p11_t *p = NULL;
    proven_err_t err = p11_load(alloc, module, &p, why);
    if (err != PROVEN_OK) return err;
    CK_SLOT_ID slots[64];
    CK_ULONG ns = 0;
    err = find_slots(p, tlabel, slots, &ns, why);
    for (CK_ULONG s = 0; err == PROVEN_OK && s < ns; ++s) {
        CK_TOKEN_INFO ti;
        char label[33] = "";
        if (p->f->C_GetTokenInfo(slots[s], &ti) == CKR_OK) token_label(&ti, label);
        err = login(p, slots[s], pin, pin_len, why);
        if (err != PROVEN_OK) break;
        CK_ULONG cls = CKO_PRIVATE_KEY;
        CK_ATTRIBUTE t[1] = { { CKA_CLASS, &cls, sizeof cls } };
        CK_OBJECT_HANDLE o[64];
        CK_ULONG n = find(p, t, 1, o, 64);
        for (CK_ULONG k = 0; k < n; ++k) {
            size_t ll = 0, il = 0, tl = 0, cl = 0;
            uint8_t *kl = attr(p, o[k], CKA_LABEL, &ll), *id = attr(p, o[k], CKA_ID, &il), *type = attr(p, o[k], CKA_KEY_TYPE, &tl), *cert = NULL;
            CK_ULONG kt = 99;
            if (type && tl == sizeof kt) memcpy(&kt, type, sizeof kt);
            char hex[130] = "";
            for (size_t i = 0; id && i < il && i < 64; ++i) snprintf(hex + 2 * i, 3, "%02x", id[i]);
            if (id) {
                CK_ULONG cc = CKO_CERTIFICATE;
                CK_ATTRIBUTE ct[2] = { { CKA_CLASS, &cc, sizeof cc }, { CKA_ID, id, (CK_ULONG)il } };
                CK_OBJECT_HANDLE co[1];
                if (find(p, ct, 2, co, 1) == 1) cert = attr(p, co[0], CKA_VALUE, &cl);
            }
            rp_key_entry_t e = { label, kl ? (const char *)kl : "", hex, kt == CKK_RSA ? "RSA" : kt == CKK_EC ? "ECDSA" : "other", cert, cl };
            visit(ctx, &e);
            rp_mem_free(alloc, kl);
            rp_mem_free(alloc, id);
            rp_mem_free(alloc, type);
            rp_mem_free(alloc, cert);
        }
        if (p->logged_in) (void)p->f->C_Logout(p->session);
        (void)p->f->C_CloseSession(p->session);
        p->logged_in = p->session_open = false;
    }
    p11_close(p);
    return err;
}

#endif
