#ifndef RUBRAPACK_INSPECT_H
#define RUBRAPACK_INSPECT_H

#include <stddef.h>
#include <stdint.h>

#include "rubrapack/cfb.h"
#include "rubrapack/msi.h"

// include/rubrapack/inspect.h - the `inspect` and `build` commands.

// argv as rp_main receives it (argv[1] == "inspect"); returns the exit code.
[[nodiscard]] int rp_cmd_inspect(int argc, char **argv);

// `rubrapack build` (src/cli/build.c).
[[nodiscard]] int rp_cmd_build(int argc, char **argv);

// `rubrapack lint <src.rpk|file.msi> [--strict]` (RFC-0006 1); the source half lives in build.c.
[[nodiscard]] int rp_cmd_lint(int argc, char **argv);
[[nodiscard]] int rp_cmd_lint_source(int argc, char **argv);

// A package read as writer tables (RFC-0006 1), for `lint` and `transform` (src/cli/lint.c).
typedef struct {
    uint8_t      *data, *sum;
    rp_cfb_t      cfb;
    rp_msi_t      msi;
    rp_msi_view_t view;
    int           stage;        // what is open: 1 data, 2 cfb, 3 msi, 4 view
} rp_pkg_t;

// Opens a package; prints why not and returns an exit code. Close it even after a failure.
[[nodiscard]] int rp_pkg_open(proven_allocator_t heap, const char *path, const rp_limits_t *limits, rp_pkg_t *p);
void rp_pkg_close(proven_allocator_t heap, rp_pkg_t *p);

// `rubrapack transform <base.msi> <target.msi> -o <out.mst>` and `inspect <file.mst>` (RFC-0016 3).
[[nodiscard]] int rp_cmd_transform(int argc, char **argv);
[[nodiscard]] int rp_mst_inspect(const char *path, const char *what, const char *base);

// `rubrapack extract <file.msi|file.cab> -d <new dir>` (RFC-0006 2).
[[nodiscard]] int rp_cmd_extract(int argc, char **argv);

// `rubrapack new [msi] <name>` and `rubrapack guid [--from <text>]` (RFC-0006 5).
[[nodiscard]] int rp_cmd_new(int argc, char **argv);
[[nodiscard]] int rp_cmd_edit(int argc, char **argv);
[[nodiscard]] int rp_cmd_guid(int argc, char **argv);

// Signing options shared by `sign` and `build --key` (one code path, RFC-0007 S4).
typedef struct {
    const char *key, *cert, *pass_env, *pass_file;
    const char *timestamp, *tsa_trust;  // --timestamp <URL>, --tsa-trust <certificates> (RFC-0008)
    const char *tls_trust;              // --tls-trust <certificates>: an https timestamp server's roots
    const char *proxy;                  // --proxy http://host:port (else https_proxy/http_proxy, no_proxy)
    bool        system_roots;           // --system-roots: the operating system's roots for it too
    bool        allow_unsigned_cabs;
    // A key that stays where it is (RFC-0011 P9b): a PKCS#11 token, or the Windows store.
    const char *pkcs11, *key_label, *token_label;   // --pkcs11 <module> --key-label <label> [--token-label <label>]
    const char *pin_env, *pin_file;                 // --pin-env VAR | --pin-file FILE (never on the command line)
    const char *key_store;                          // --key-store <SHA-1 thumbprint> (Windows)
    bool        machine_store;                      // --machine-store: LocalMachine\My instead of CurrentUser\My
} rp_sign_args_t;

// "2026-09-27T07:44:31Z" from seconds since 1970.
void rp_iso_time(int64_t t, char out[64]);

// `rubrapack keys list` (RFC-0011 P9b): the keys that can sign, in a PKCS#11 token or the Windows store.
[[nodiscard]] int rp_cmd_keys(int argc, char **argv);

// Whether a signing key was given (--key, --pkcs11 or --key-store).
static inline bool rp_sign_wanted(const rp_sign_args_t *a) { return a->key || a->pkcs11 || a->key_store; }

// Takes one of the key options (--key, --cert, --pass-env, --pass-file, --pkcs11, --key-label,
// --token-label, --pin-env, --pin-file, --key-store, --machine-store): 1 or 2 arguments used, 0 when
// `arg` is none of them.
[[nodiscard]] int rp_sign_key_option(rp_sign_args_t *a, const char *arg, const char *next);

// Signs a PE file or an MSI package held in memory; prints its own diagnostics (`label` names the
// file) and returns an exit code.
[[nodiscard]] int rp_sign_bytes(const rp_sign_args_t *a, const char *label, const uint8_t *in, size_t len, uint8_t **out, size_t *out_len);

// `rubrapack sign` and `rubrapack verify` (RFC-0007 S4).
[[nodiscard]] int rp_cmd_sign(int argc, char **argv);
[[nodiscard]] int rp_cmd_verify(int argc, char **argv);

#endif // RUBRAPACK_INSPECT_H
