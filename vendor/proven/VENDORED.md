# Vendored proven_c_lib

A copied snapshot of proven_c_lib (MIT, same author). Do not edit files here; if a proven
defect is found, report it to proven_c_lib and resync from a fixed release.

- Snapshot: tag `v0.1.1`, commit `22f964e`, taken 2026-09-26 with `git archive v0.1.1`
  (`include/`, `src/`, `platform/`, `LICENSE`, `THIRD_PARTY_NOTICES.md`).
- Build flags: `nob.c` compiles these files with proven's own flag set
  (`-std=c23 -Wall -Wextra -Werror`, no `-pedantic`), because `src/proven/random.c` and
  `src/proven/float_decimal.c` use `unsigned __int128`. rubrapack's own sources keep `-pedantic`.
