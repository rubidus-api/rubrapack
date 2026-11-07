# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found, report it to proven_c_lib and resync from a fixed release.

- Snapshot: tag `v0.112.1`, commit `ea571ee8`, taken 2026-11-07 with `git archive v0.112.1`
  (`include/`, `src/`, `platform/`, `net/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`). From v0.111.0 the
  network library (proven_net: sockets, TLS, HTTP, ...) is in `net/`; rubrapack uses its TLS engine,
  so `net/` is vendored and `net/include` is on the include path.
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`), because `src/proven/random.c` and
  `src/proven/float_decimal.c` use `unsigned __int128`. Only the files `nob.c` names are compiled
  (the hashes, HMAC, certificates, signatures, ciphers and, from `net/src/`, the TLS engine);
  the rest of the snapshot (HTTP, QUIC, compression, ...) is here unbuilt. rubrapack's own sources keep `-pedantic`.
