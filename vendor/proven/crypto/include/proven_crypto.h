#ifndef PROVEN_CRYPTO_UMBRELLA_H
#define PROVEN_CRYPTO_UMBRELLA_H

/**
 * @file proven_crypto.h
 * @brief Entry point for the cryptography library, proven_crypto (RFC-0045): SHA-2, HMAC and the
 *        legacy hashes, AES and AEAD ciphers, key exchange, signing keys, DER and X.509
 *        certificates. It stands on the base library's public headers alone and is built from
 *        the same tree, under one version.
 *
 * Its headers keep the names they always had (proven/sign.h, proven/cert.h, ...) and live in
 * crypto/include: add that directory to the include path, compile crypto/src and
 * crypto/platform with the base library, and proven.h includes this file too. Everything but
 * the system trust store (cert_system.c, hosted) is pure computation and part of the
 * freestanding profile. The random generators stay in the base library (random.h).
 */

#include "proven.h"
#include "proven/sha2.h"
#include "proven/hash_legacy.h"
#include "proven/hmac.h"
#include "proven/aes.h"
#include "proven/aead.h"
#include "proven/kex.h"
#include "proven/der.h"
#include "proven/cert.h"
#include "proven/sign.h"

#endif /* PROVEN_CRYPTO_UMBRELLA_H */
