// src/cli/keys.c - `rubrapack keys list` (RFC-0011 P9b): the private keys that can sign - in a PKCS#11
// token (--pkcs11) or, on Windows, the "My" certificate stores - with what `sign` needs to use them
// and, for MSIX, the publisher each certificate signs for. Nothing secret is printed.

#include "rubrapack/diag.h"
#include "rubrapack/inspect.h"
#include "rubrapack/keys.h"
#include "rubrapack/mem.h"
#include "rubrapack/msix.h"
#include "rubrapack/pal.h"

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"

typedef struct {
    size_t count;
    int    rc;
} list_t;

static void show(void *ctx, const rp_key_entry_t *e) {
    list_t *l = ctx;
    char line[3000], publisher[1200] = "-", until[64] = "-";
    rp_cert_t c;
    if (e->cert && rp_cert_parse(e->cert, e->cert_len, &c, NULL)) {
        rp_iso_time(c.not_after, until);
        if (!rp_msix_publisher_of(proven_heap_allocator(), e->cert, e->cert_len, publisher, sizeof publisher)) snprintf(publisher, sizeof publisher, "?");
    }
    if (e->label[0]) {
        snprintf(line, sizeof line, "%s\tlabel=%s\tid=%s\t%s\tpublisher=%s\tuntil=%s%s\n", e->where, e->label, e->id_hex, e->kind, publisher, until,
                 e->cert ? "" : "\t(no certificate on the token)");
    } else {
        snprintf(line, sizeof line, "%s\tthumbprint=%s\t%s\tpublisher=%s\tuntil=%s\n", e->where, e->id_hex, e->kind, publisher, until);
    }
    if (rp_pal_puts(RP_OUT_STDOUT, line) != PROVEN_OK) l->rc = RP_EXIT_IO;
    ++l->count;
}

int rp_cmd_keys(int argc, char **argv) {
    rp_sign_args_t a = { 0 };
    bool list = argc >= 3 && strcmp(argv[2], "list") == 0, bad = !list;
    for (int i = 3; list && i < argc && !bad; ++i) {
        int used = rp_sign_key_option(&a, argv[i], i + 1 < argc ? argv[i + 1] : NULL);
        if (used == 0) bad = true;
        else i += used - 1;
    }
    if (bad || a.key || a.key_store || a.cert || a.key_label || (a.pin_env && a.pin_file) || ((a.token_label || a.pin_env || a.pin_file) && !a.pkcs11)) {
        rp_diag_error(RP_DIAG_EXTRA_ARGUMENT, "usage: rubrapack keys list [--pkcs11 <module> [--token-label <label>] [--pin-env VAR | --pin-file FILE]]");
        return RP_EXIT_USAGE;
    }
    proven_allocator_t heap = proven_heap_allocator();
    list_t l = { 0, RP_EXIT_OK };
    const char *why = NULL;
    proven_err_t err;
    if (a.pkcs11) {
        uint8_t *pin = NULL;
        size_t pin_len = 0;
        if (a.pin_env) {
            char *v = rp_pal_getenv(heap, a.pin_env);
            if (v == NULL) {
                rp_diag_error(RP_DIAG_SIGN, "--pin-env %s: the variable is not set", a.pin_env);
                return RP_EXIT_USAGE;
            }
            pin = (uint8_t *)v;
            pin_len = strlen(v);
        } else if (a.pin_file) {
            if (rp_pal_read_file(heap, a.pin_file, 4096, &pin, &pin_len) != PROVEN_OK) {
                rp_diag_error(RP_DIAG_INPUT, "cannot read the PIN file '%s'", a.pin_file);
                return RP_EXIT_IO;
            }
            while (pin_len && (pin[pin_len - 1] == '\n' || pin[pin_len - 1] == '\r')) --pin_len;
        }
        err = rp_pkcs11_list(heap, a.pkcs11, a.token_label, pin, pin_len, show, &l, &why);
        if (pin) {
            rp_wipe(pin, pin_len);
            rp_mem_free(heap, pin);
        }
    } else {
        err = rp_ncrypt_list(heap, show, &l, &why);
    }
    if (err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_SIGN, "%s", why ? why : "cannot list the keys");
        return err == PROVEN_ERR_UNSUPPORTED ? RP_EXIT_USAGE : err == PROVEN_ERR_PERMISSION ? RP_EXIT_SIGN : RP_EXIT_IO;
    }
    if (l.count == 0 && l.rc == RP_EXIT_OK) l.rc = rp_pal_puts(RP_OUT_STDOUT, "no keys\n") == PROVEN_OK ? RP_EXIT_OK : RP_EXIT_IO;
    return l.rc;
}
