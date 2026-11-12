# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found, report it to proven_c_lib and resync from a fixed release.

- Snapshot: tag `v0.114.0`, commit `5236974e`, taken 2026-11-13 with `git archive v0.114.0`
  (`include/`, `src/`, `platform/`, `crypto/`, `codec/`, `net/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`).
  From v0.113.0 the cryptography and certificates are proven_crypto (`crypto/`: SHA-2, HMAC, the
  legacy hashes, AES, AEAD, key exchange, signing, DER, X.509) and compression is proven_codec
  (`codec/`); the network library (proven_net: sockets, TLS, HTTP, ...) is in `net/` and needs both.
  rubrapack uses proven_crypto, the TLS engine of `net/` and, of `codec/`, only the CPU probe; the
  `include` folders of `crypto/`, `codec/` and `net/` are on the include path.
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`), because `src/proven/random.c` and
  `src/proven/float_decimal.c` use `unsigned __int128`. Only the files `nob.c` names are compiled
  (from `src/proven/` the base, from `crypto/` the hashes, HMAC, certificates, signatures and
  ciphers, from `codec/platform/` the CPU probe and, from `net/src/`, the TLS engine);
  the rest of the snapshot (HTTP, QUIC, compression, ...) is here unbuilt. rubrapack's own sources keep `-pedantic`.
