// src/core/diag.c - one-line diagnostics on stderr (include/rubrapack/diag.h).

#include "rubrapack/diag.h"
#include "rubrapack/pal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- `--json` (RFC-0025) ---------------------------------------------------------------------------

static struct {
    bool   on;
    char   items[1u << 18];     // the "diagnostics" array's elements, joined
    size_t len, errors, warnings, not_shown;
    bool   full;
} json;

static void put(const char *s, size_t n) {
    if (json.full || json.len + n + 1 >= sizeof json.items) {
        json.full = true;
        return;
    }
    memcpy(json.items + json.len, s, n);
    json.len += n;
}

static void putz(const char *s) { put(s, strlen(s)); }

static void put_str(const char *s) {
    put("\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
        char e[8];
        if (*p == '"' || *p == '\\') {
            e[0] = '\\';
            e[1] = (char)*p;
            put(e, 2);
        } else if (*p < 0x20 || *p == 0x7F) {
            snprintf(e, sizeof e, "\\u%04x", *p);
            put(e, 6);
        } else {
            put((const char *)p, 1);
        }
    }
    put("\"", 1);
}

void rp_diag_json_begin(void) { json.on = true; }

bool rp_diag_json_on(void) { return json.on; }

void rp_diag_json_add(const char *path, unsigned line, unsigned col, const char *code, bool warning, const char *msg) {
    if (warning) ++json.warnings;
    else ++json.errors;
    char num[48];
    putz(json.len ? ",\n  {\"file\": " : "\n  {\"file\": ");
    if (path) put_str(path);
    else putz("null");
    snprintf(num, sizeof num, ", \"line\": %u, \"column\": %u, ", line, col);
    putz(num);
    putz(warning ? "\"severity\": \"warning\", \"code\": " : "\"severity\": \"error\", \"code\": ");
    put_str(code);
    putz(", \"message\": ");
    put_str(msg);
    putz("}");
}

void rp_diag_json_not_shown(size_t n) { json.not_shown += n; }

void rp_diag_json_end(const char *path, int rc) {
    char head[256];
    json.on = false;
    (void)rp_pal_puts(RP_OUT_STDOUT, "{\"file\": ");
    json.items[json.len] = '\0';
    // The file name, escaped like the rest.
    size_t at = json.len;
    put_str(path);
    json.items[json.len] = '\0';
    (void)rp_pal_puts(RP_OUT_STDOUT, json.items + at);
    json.len = at;
    json.items[json.len] = '\0';
    snprintf(head, sizeof head, ", \"exit\": %d, \"errors\": %zu, \"warnings\": %zu, \"not_shown\": %zu, \"diagnostics\": [", rc, json.errors,
             json.warnings, json.not_shown + (json.full ? 1 : 0));
    (void)rp_pal_puts(RP_OUT_STDOUT, head);
    (void)rp_pal_puts(RP_OUT_STDOUT, json.items);
    (void)rp_pal_puts(RP_OUT_STDOUT, json.len ? "\n]}\n" : "]}\n");
}

static void emit(const char *kind, const char *code, const char *fmt, va_list ap) {
    char msg[1024];
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    if (n < 0) {
        msg[0] = '\0';
    } else if ((size_t)n >= sizeof msg) {
        // vsnprintf cut the text at a byte. If the last character lost bytes, drop it whole.
        size_t len = sizeof msg - 1;
        size_t p = len;
        while (p > 0 && ((unsigned char)msg[p - 1] & 0xC0u) == 0x80u) --p;
        if (p > 0) {
            unsigned char lead = (unsigned char)msg[p - 1];
            size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if ((p - 1) + need > len) msg[p - 1] = '\0';
        }
    }

    if (json.on) {
        rp_diag_json_add(NULL, 0, 0, code, strcmp(kind, "warning") == 0, msg);
        return;
    }
    char line[1100];
    int m = snprintf(line, sizeof line, "rubrapack: %s[%s]: %s\n", kind, code, msg);
    if (m < 0) return;
    size_t len = strlen(line);
    (void)rp_pal_write(RP_OUT_STDERR, (const uint8_t *)line, len);
}

void rp_diag_error(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("error", code, fmt, ap);
    va_end(ap);
}

void rp_diag_warning(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("warning", code, fmt, ap);
    va_end(ap);
}
