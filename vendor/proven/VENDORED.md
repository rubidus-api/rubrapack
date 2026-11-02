# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found, report it to proven_c_lib and resync from a fixed release.

- Snapshot: tag `v0.107.2`, commit `def42831`, taken 2026-11-02 with `git archive v0.107.2`
  (`include/`, `src/`, `platform/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`).
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`), because `src/proven/random.c` and
  `src/proven/float_decimal.c` use `unsigned __int128`. Only the files `nob.c` names are compiled;
  the rest of the snapshot (TLS, HTTP, compression, ...) is here unbuilt. rubrapack's own sources keep `-pedantic`.
