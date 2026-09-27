// src/msix/msix.c - MSIX packages from the model (include/rubrapack/msix.h; RFC-0009, RFC-0001 13).
//
// The package root is the folder of the application's executable, walked up to the folder the
// source anchors in a known location (ProgramFiles/<name> and the like): every file of the package
// must be below it (files elsewhere need VFS, P8b). Names inside the package are three forms of one
// path (RFC-0001 13.1): the ZIP entry (OPC part name, '/', percent-encoded), the block map name
// ('\', as written) and the manifest's references ('\').

#include "rubrapack/msix.h"

#include "rubrapack/buf.h"
#include "rubrapack/crypto.h"
#include "rubrapack/deflate.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/regf.h"
#include "rubrapack/text.h"
#include "rubrapack/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 65536, LEVEL = 6 };

// The bundle manifest as the ZIP and the block map name it.
#define BUNDLE_MANIFEST_ZIP "AppxMetadata/AppxBundleManifest.xml"
#define BUNDLE_MANIFEST     "AppxMetadata\\AppxBundleManifest.xml"

static const char UNSIGNED_OID[] = "OID.2.25.311729368913984317654407730594956997722=1";

// ---- small helpers ------------------------------------------------------------------------------

static void xml_text(rp_buf_t *b, const char *s) {
    for (; *s; ++s) {
        switch (*s) {
        case '&': rp_buf_puts(b, "&amp;"); break;
        case '<': rp_buf_puts(b, "&lt;"); break;
        case '>': rp_buf_puts(b, "&gt;"); break;
        case '"': rp_buf_puts(b, "&quot;"); break;
        case '\'': rp_buf_puts(b, "&apos;"); break;
        default: rp_buf_byte(b, (uint8_t)*s);
        }
    }
}

static void attr(rp_buf_t *b, const char *name, const char *value) {
    rp_buf_byte(b, ' ');
    rp_buf_puts(b, name);
    rp_buf_puts(b, "=\"");
    xml_text(b, value);
    rp_buf_byte(b, '"');
}

static void attr_u64(rp_buf_t *b, const char *name, uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    attr(b, name, t);
}

static void base64(const uint8_t *p, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[o] = 0;
}

// An OPC part name from a package path ('\' separated, UTF-8): '/' between segments, every byte
// outside A-Z a-z 0-9 - . _ ~ percent-encoded (as Windows' writer does, parentheses included).
static void part_name(const char *path, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < cap; ++p) {
        unsigned ch = *p;
        if (ch == '\\') out[o++] = '/';
        else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~') out[o++] = (char)ch;
        else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = 0;
}

static const char *extension(const char *path) {
    const char *dot = strrchr(path, '.'), *sep = strrchr(path, '\\');
    return dot && (!sep || dot > sep) && dot[1] ? dot + 1 : NULL;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static const char *content_type(const char *ext) {
    static const struct { const char *ext, *type; } map[] = {
        { "exe", "application/x-msdownload" }, { "dll", "application/x-msdownload" }, { "sys", "application/x-msdownload" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "bmp", "image/bmp" },
        { "ico", "image/vnd.microsoft.icon" }, { "svg", "image/svg+xml" }, { "txt", "text/plain" }, { "htm", "text/html" },
        { "html", "text/html" }, { "css", "text/css" }, { "js", "application/javascript" }, { "json", "application/json" },
        { "xml", "application/xml" }, { "pdf", "application/pdf" }, { "zip", "application/x-zip-compressed" },
    };
    for (size_t i = 0; ext && i < sizeof map / sizeof map[0]; ++i) {
        if (ieq(ext, map[i].ext)) return map[i].type;
    }
    return "application/octet-stream";
}

// Data that deflate cannot shrink (RFC-0009 M3): stored.
static bool already_compressed(const char *ext) {
    static const char *const list[] = { "png", "jpg", "jpeg", "gif", "zip", "7z", "gz", "bz2", "xz", "zst", "cab", "msix", "appx",
                                        "docx", "xlsx", "pptx", "mp3", "mp4", "m4a", "ogg", "webm", "webp", "woff", "woff2", NULL };
    for (size_t i = 0; ext && list[i]; ++i) {
        if (ieq(ext, list[i])) return true;
    }
    return false;
}

// ---- PNG: the size of a given logo; a plain default logo (RFC-0009 M4) --------------------------

static bool png_size(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n < 24 || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) return false;
    *w = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
    *h = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
    return true;
}

static void be32(rp_buf_t *b, uint32_t v) {
    rp_buf_byte(b, (uint8_t)(v >> 24));
    rp_buf_byte(b, (uint8_t)(v >> 16));
    rp_buf_byte(b, (uint8_t)(v >> 8));
    rp_buf_byte(b, (uint8_t)v);
}

static void png_chunk(rp_buf_t *b, const char *type, const uint8_t *data, size_t n) {
    be32(b, (uint32_t)n);
    size_t at = b->len;
    rp_buf_put(b, type, 4);
    rp_buf_put(b, data, n);
    be32(b, b->err == PROVEN_OK ? rp_crc32(0, b->data + at, 4 + n) : 0);
}

// A size x size PNG of one colour (8-bit RGB): the logo when the source gives none.
static proven_err_t default_logo(proven_allocator_t alloc, uint32_t size, uint8_t **out, size_t *len) {
    size_t row = 1 + 3 * (size_t)size, raw_len = row * size;
    uint8_t *raw = rp_mem_alloc(alloc, raw_len, 1);
    if (raw == NULL) return PROVEN_ERR_NOMEM;
    for (size_t y = 0; y < size; ++y) {
        uint8_t *r = raw + y * row;
        r[0] = 0;                                          // filter: none
        for (size_t x = 0; x < size; ++x) r[1 + 3 * x] = 0x2A, r[2 + 3 * x] = 0x5D, r[3 + 3 * x] = 0x8F;
    }
    uint8_t *def = NULL;
    size_t def_len = 0;
    proven_err_t err = rp_deflate(alloc, raw, raw_len, 9, &def, &def_len);
    uint32_t a = 1, b2 = 0;                                // Adler-32 of the raw data (zlib, RFC 1950)
    for (size_t i = 0; i < raw_len; ++i) {
        a = (a + raw[i]) % 65521;
        b2 = (b2 + a) % 65521;
    }
    rp_mem_free(alloc, raw);
    if (err != PROVEN_OK) return err;
    rp_buf_t z = rp_buf_new(alloc, 1u << 20), p = rp_buf_new(alloc, 1u << 20);
    rp_buf_byte(&z, 0x78);
    rp_buf_byte(&z, 0x01);
    rp_buf_put(&z, def, def_len);
    be32(&z, b2 << 16 | a);
    rp_mem_free(alloc, def);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    rp_buf_put(&p, sig, 8);
    uint8_t ihdr[13] = { (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size,
                         (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size, 8, 2, 0, 0, 0 };
    png_chunk(&p, "IHDR", ihdr, sizeof ihdr);
    png_chunk(&p, "IDAT", z.data, z.len);
    png_chunk(&p, "IEND", NULL, 0);
    err = z.err != PROVEN_OK ? z.err : p.err;
    rp_buf_free(&z);
    if (err != PROVEN_OK) {
        rp_buf_free(&p);
        return err;
    }
    return rp_buf_take(&p, out, len);
}

// ---- the package root and the paths inside it ----------------------------------------------------

static const rp_ir_dir_t *find_dir(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; id && i < ir->dir_count; ++i) {
        if (strcmp(ir->dirs[i].id, id) == 0) return &ir->dirs[i];
    }
    return NULL;
}

// The top folder of `dir` (the one anchored in a known location) and the path below it
// (components joined with '\'); false when the chain breaks.
static bool below_root(const rp_ir_t *ir, const char *dir, const rp_ir_dir_t **root, char *path, size_t cap) {
    const rp_ir_dir_t *chain[64];
    size_t n = 0;
    for (const rp_ir_dir_t *d = find_dir(ir, dir); d; d = d->base ? NULL : find_dir(ir, d->parent)) {
        if (n == 64) return false;
        chain[n++] = d;
    }
    if (n == 0 || chain[n - 1]->base == NULL) return false;
    *root = chain[n - 1];
    size_t o = 0;
    path[0] = 0;
    for (size_t i = n - 1; i-- > 0;) {
        for (size_t k = 0; k < chain[i]->part_count; ++k) {
            int w = snprintf(path + o, cap - o, "%s%s", o ? "\\" : "", chain[i]->parts[k]);
            if (w < 0 || (size_t)w >= cap - o) return false;
            o += (size_t)w;
        }
    }
    return true;
}

// ---- building --------------------------------------------------------------------------------------

typedef struct {
    const char *file_id;        // the [file.*] it comes from, or NULL
    char        path[2200];     // package path, '\' separated
    const char *source;         // file to read, or NULL for `data`
    uint8_t    *data;           // generated content (logos), owned
    size_t      data_len;
    rp_pos_t    pos;
} item_t;

#define DERR(pos, code, ...) rp_srcdiag_add(d, (pos), (code), false, __VA_ARGS__)

static bool reserved(const char *path) {
    static const char *const names[] = { "AppxManifest.xml", "AppxBlockMap.xml", "[Content_Types].xml", "AppxSignature.p7x",
                                         "CodeIntegrity.cat", "resources.pri", "Registry.dat", "User.dat", "UserClasses.dat", NULL };
    for (size_t i = 0; names[i]; ++i) {
        if (ieq(path, names[i])) return true;
    }
    char top[16];
    size_t n = strcspn(path, "\\");
    snprintf(top, sizeof top, "%.*s", (int)(n < 15 ? n : 15), path);
    return n < 15 && path[n] == '\\' && (ieq(top, "AppxMetadata") || ieq(top, "VFS"));
}

// The VFS folder for files anchored in a known location (RFC-0010 N5, narrowed to what Windows
// does: docs/research/2026-09-27-p8b-regf-oracle.md), or NULL with *why.
static const char *vfs_folder(const char *base, rp_arch_t arch, const char **why) {
    bool x86 = arch == RP_ARCH_X86;
    if (strcmp(base, "ProgramFiles") == 0) return x86 ? "ProgramFilesX86" : "ProgramFilesX64";
    if (strcmp(base, "ProgramFiles32") == 0) return "ProgramFilesX86";
    if (strcmp(base, "CommonFiles") == 0) return x86 ? "ProgramFilesCommonX86" : "ProgramFilesCommonX64";
    if (strcmp(base, "System") == 0) return x86 ? "SystemX86" : "SystemX64";
    if (strcmp(base, "Windows") == 0) return "Windows";
    if (strcmp(base, "CommonAppData") == 0) return "Common AppData";
    if (strcmp(base, "AppData") == 0 || strcmp(base, "LocalAppData") == 0) {
        *why = "MSIX has no virtual folder for the user's AppData; the app creates what it needs there when it runs";
    } else if (strcmp(base, "Fonts") == 0) {
        *why = "fonts go into an MSIX through [font.*] (planned for P8b-3)";
    } else if (strcmp(base, "Temp") == 0) {
        *why = "an MSIX cannot place files in the temporary folder";
    } else {
        *why = "Start menu, Programs, Desktop and Startup entries come from shortcuts (planned for P8b-3)";
    }
    return NULL;
}

// ---- [registry] into Registry.dat (HKLM\Software) and User.dat (HKCU) ------------------------------
//
// Measured on Windows (docs/research/2026-09-27-p8b-regf-oracle.md): a package's Registry.dat maps its
// REGISTRY\MACHINE\SOFTWARE to HKLM\Software (WOW6432Node beneath it for the 32-bit view), User.dat
// maps its root to HKCU.

// A formatted string (RFC-0004) that holds no install-time part: [\[] and [\]] become [ and ];
// false for any other [...] (a property, a file's path...), which only Windows Installer knows.
static bool literal(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s; ++s) {
        if (o + 2 >= cap) return false;
        if (s[0] == '[') {
            if ((s[1] == '\\') && (s[2] == '[' || s[2] == ']') && s[3] == ']') {
                out[o++] = s[2];
                s += 3;
                continue;
            }
            return false;
        }
        out[o++] = *s;
    }
    out[o] = 0;
    return true;
}

// `s` as UTF-16LE with its terminating zero.
static bool utf16z(rp_buf_t *b, const char *s) {
    static uint16_t tmp[8192];
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)s, strlen(s), tmp, 8192);
    if (r.err != PROVEN_OK) return false;
    for (size_t i = 0; i < r.units; ++i) rp_buf_u16le(b, tmp[i]);
    rp_buf_u16le(b, 0);
    return b->err == PROVEN_OK;
}

// Adds one [registry.ID] to the hives; false with a diagnostic when it cannot go into an MSIX.
static bool add_registry(proven_allocator_t alloc, const rp_ir_registry_t *r, rp_regf_t *machine, rp_regf_t *user, bool *used_m, bool *used_u,
                         rp_srcdiags_t *d) {
    if (r->root == RP_ROOT_HKCR) {
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: HKCR does not reach outside an MSIX; use [assoc.*] or [protocol.*] (planned for P8b-3), or msi-only = true", r->id);
        return false;
    }
    if (r->remove || r->keep) {
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: an MSIX neither removes values nor leaves them behind (remove, keep); use msi-only = true", r->id);
        return false;
    }
    bool hkcu = r->root == RP_ROOT_HKCU;
    const char *key = r->key;
    if (!(strncmp(key, "Software", 8) == 0 || strncmp(key, "SOFTWARE", 8) == 0 || strncmp(key, "software", 8) == 0) || (key[8] != '\\' && key[8] != 0)) {
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: an MSIX carries values under %s\\Software only", r->id, hkcu ? "HKCU" : "HKLM");
        return false;
    }
    char path[2048];
    const char *below = key[8] ? key + 9 : "";
    if (hkcu) snprintf(path, sizeof path, "Software%s%s", below[0] ? "\\" : "", below);
    else snprintf(path, sizeof path, "REGISTRY\\MACHINE\\SOFTWARE%s%s%s", r->view32 ? "\\WOW6432Node" : "", below[0] ? "\\" : "", below);
    rp_buf_t data = rp_buf_new(alloc, 1u << 24);
    uint32_t type = 0;
    bool ok = true;
    char text[8192];
    switch (r->type) {
    case RP_REG_STRING:
    case RP_REG_EXPAND:
        type = r->type == RP_REG_STRING ? 1 : 2;
        ok = r->value == NULL || (literal(r->value, text, sizeof text) && utf16z(&data, text));
        if (r->value == NULL) ok = utf16z(&data, "");
        break;
    case RP_REG_MULTI:
        type = 7;
        for (size_t i = 0; ok && i < r->item_count; ++i) ok = literal(r->items[i], text, sizeof text) && utf16z(&data, text);
        rp_buf_u16le(&data, 0);
        break;
    case RP_REG_DWORD: {
        type = 4;
        long long v = strtoll(r->value, NULL, 10);
        rp_buf_u32le(&data, (uint32_t)v);
        break;
    }
    case RP_REG_QWORD: {
        type = 11;
        unsigned long long v = strncmp(r->value, "0x", 2) == 0 || strncmp(r->value, "0X", 2) == 0 ? strtoull(r->value + 2, NULL, 16)
                                                                                                 : (unsigned long long)strtoll(r->value, NULL, 10);
        rp_buf_u64le(&data, v);
        break;
    }
    case RP_REG_BINARY:
        type = 3;
        for (size_t i = 0; r->value[i] && r->value[i + 1]; i += 2) {
            unsigned hi = (unsigned)(r->value[i] <= '9' ? r->value[i] - '0' : (r->value[i] | 32) - 'a' + 10);
            unsigned lo = (unsigned)(r->value[i + 1] <= '9' ? r->value[i + 1] - '0' : (r->value[i + 1] | 32) - 'a' + 10);
            rp_buf_byte(&data, (uint8_t)(hi << 4 | lo));
        }
        break;
    }
    if (!ok) {
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: its value has a part Windows Installer fills in at install time ([...]); an MSIX holds fixed values only", r->id);
        rp_buf_free(&data);
        return false;
    }
    proven_err_t err = data.err != PROVEN_OK ? data.err
                                             : rp_regf_set(hkcu ? user : machine, path, r->name ? r->name : "", type, data.data, data.len);
    rp_buf_free(&data);
    if (err != PROVEN_OK) {
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: the key or value name cannot go into a registry hive", r->id);
        return false;
    }
    *(hkcu ? used_u : used_m) = true;
    return true;
}

// Adds one payload file: its data stored or deflated block by block, its block map entry.
// `zip_name` overrides the ZIP entry's name (the footprint file [Content_Types].xml keeps its own).
static proven_err_t add_payload(proven_allocator_t alloc, rp_zip_writer_t *z, rp_buf_t *bm, const char *path, const char *zip_name,
                                const uint8_t *data, size_t n, bool deflate) {
    char part[3200];
    if (zip_name) snprintf(part, sizeof part, "%s", zip_name);
    else part_name(path, part, sizeof part);
    rp_buf_t comp = rp_buf_new(alloc, (size_t)1 << 40);
    rp_buf_t blocks = rp_buf_new(alloc, (size_t)1 << 30);
    proven_err_t err = PROVEN_OK;
    size_t nblocks = (n + BLOCK - 1) / BLOCK;
    for (size_t i = 0; i < nblocks && err == PROVEN_OK; ++i) {
        size_t take = n - i * BLOCK < BLOCK ? n - i * BLOCK : BLOCK;
        uint8_t h[32];
        char b64[48];
        rp_hash(RP_HASH_SHA256, data + i * BLOCK, take, h);
        base64(h, 32, b64);
        rp_buf_puts(&blocks, "<Block");
        attr(&blocks, "Hash", b64);
        if (deflate) {
            uint8_t *seg = NULL;
            size_t sl = 0;
            err = rp_deflate_segment(alloc, data + i * BLOCK, take, LEVEL, &seg, &sl);
            if (err == PROVEN_OK) {
                rp_buf_put(&comp, seg, sl);
                attr_u64(&blocks, "Size", sl);
                rp_mem_free(alloc, seg);
            }
        }
        rp_buf_puts(&blocks, "/>");
    }
    if (nblocks > 1) {                                     // the whole file's hash (2021 block map)
        uint8_t h[32];
        char b64[48];
        rp_hash(RP_HASH_SHA256, data, n, h);
        base64(h, 32, b64);
        rp_buf_puts(&blocks, "<b4:FileHash");
        attr(&blocks, "Hash", b64);
        rp_buf_puts(&blocks, "/>");
    }
    if (deflate) rp_buf_put(&comp, RP_DEFLATE_END, 2);
    size_t lfh = 0;
    if (err == PROVEN_OK && comp.err == PROVEN_OK && blocks.err == PROVEN_OK) {
        rp_zip_add(z, part, deflate ? 8 : 0, deflate ? comp.data : data, deflate ? comp.len : n, rp_crc32(0, data, n), n, &lfh);
        if (bm) {
            rp_buf_puts(bm, "<File");
            attr(bm, "Name", path);
            attr_u64(bm, "Size", n);
            attr_u64(bm, "LfhSize", lfh);
            if (blocks.len) {
                rp_buf_byte(bm, '>');
                rp_buf_put(bm, blocks.data, blocks.len);
                rp_buf_puts(bm, "</File>");
            } else {
                rp_buf_puts(bm, "/>");
            }
        }
    }
    if (err == PROVEN_OK) err = comp.err != PROVEN_OK ? comp.err : blocks.err;
    rp_buf_free(&comp);
    rp_buf_free(&blocks);
    return err;
}

typedef struct {
    char exe[2200];             // the executable's package path
    char logo[3][2200];         // Square150x150, Square44x44, StoreLogo
} app_paths_t;

static void manifest(rp_buf_t *m, const rp_ir_t *ir, const rp_msix_options_t *opt, const app_paths_t *ap) {
    static const char *const arch[] = { "x64", "arm64", "x86" };
    char version[32], publisher[8400];
    unsigned v[4] = { 0 };
    for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
    snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    snprintf(publisher, sizeof publisher, "%s%s%s", ir->msix_publisher, opt->unsigned_test ? ", " : "", opt->unsigned_test ? UNSIGNED_OID : "");
    rp_buf_puts(m, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                   "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\"\r\n"
                   "         xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\"\r\n"
                   "         xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\"\r\n"
                   "         IgnorableNamespaces=\"uap rescap\">\r\n  <Identity");
    attr(m, "Name", ir->msix_identity_name);
    attr(m, "Publisher", publisher);
    attr(m, "Version", version);
    attr(m, "ProcessorArchitecture", arch[ir->arch]);
    rp_buf_puts(m, " />\r\n  <Properties>\r\n    <DisplayName>");
    xml_text(m, ir->name);
    rp_buf_puts(m, "</DisplayName>\r\n    <PublisherDisplayName>");
    xml_text(m, ir->msix_publisher_display ? ir->msix_publisher_display : ir->manufacturer);
    rp_buf_puts(m, "</PublisherDisplayName>\r\n    <Logo>");
    xml_text(m, ap[0].logo[2]);
    rp_buf_puts(m, "</Logo>\r\n  </Properties>\r\n  <Dependencies>\r\n    <TargetDeviceFamily Name=\"Windows.Desktop\"");
    attr(m, "MinVersion", ir->msix_min_version ? ir->msix_min_version : "10.0.17763.0");
    rp_buf_puts(m, " MaxVersionTested=\"10.0.26100.0\" />\r\n  </Dependencies>\r\n  <Resources>\r\n    <Resource");
    attr(m, "Language", ir->language == 1042 ? "ko-KR" : "en-US");
    rp_buf_puts(m, " />\r\n  </Resources>\r\n  <Applications>\r\n");
    for (size_t i = 0; i < ir->msix_app_count; ++i) {
        const rp_ir_msix_app_t *a = &ir->msix_apps[i];
        const char *display = a->display ? a->display : ir->name;
        rp_buf_puts(m, "    <Application");
        attr(m, "Id", a->id);
        attr(m, "Executable", ap[i].exe);
        rp_buf_puts(m, " EntryPoint=\"Windows.FullTrustApplication\">\r\n      <uap:VisualElements");
        attr(m, "DisplayName", display);
        attr(m, "Description", a->description ? a->description : display);
        rp_buf_puts(m, " BackgroundColor=\"transparent\"");
        attr(m, "Square150x150Logo", ap[i].logo[0]);
        attr(m, "Square44x44Logo", ap[i].logo[1]);
        rp_buf_puts(m, " />\r\n    </Application>\r\n");
    }
    rp_buf_puts(m, "  </Applications>\r\n  <Capabilities>\r\n"
                   "    <rescap:Capability Name=\"runFullTrust\" />\r\n  </Capabilities>\r\n</Package>\r\n");
}

// [Content_Types].xml: one Default per extension in order of first use, the manifest's type for
// "xml" unless a payload file took it, Overrides for files without an extension and the block map.
static void content_types(rp_buf_t *t, const item_t *items, size_t n) {
    rp_buf_puts(t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">");
    const char *seen[256];
    size_t ns = 0;
    bool xml_taken = false;
    for (size_t i = 0; i < n; ++i) {
        const char *ext = extension(items[i].path);
        if (ext == NULL) continue;
        bool dup = false;
        for (size_t k = 0; k < ns && !dup; ++k) dup = ieq(seen[k], ext);
        if (dup || ns == 256) continue;
        seen[ns++] = ext;
        char low[64];
        size_t l = 0;
        for (; ext[l] && l < 63; ++l) low[l] = ext[l] >= 'A' && ext[l] <= 'Z' ? (char)(ext[l] + 32) : ext[l];
        low[l] = 0;
        xml_taken |= strcmp(low, "xml") == 0;
        rp_buf_puts(t, "<Default");
        attr(t, "Extension", low);
        attr(t, "ContentType", content_type(low));
        rp_buf_puts(t, " />");
    }
    if (!xml_taken) rp_buf_puts(t, "<Default Extension=\"xml\" ContentType=\"application/vnd.ms-appx.manifest+xml\" />");
    for (size_t i = 0; i < n; ++i) {
        if (extension(items[i].path)) continue;
        char part[3200] = "/";
        part_name(items[i].path, part + 1, sizeof part - 1);
        rp_buf_puts(t, "<Override");
        attr(t, "PartName", part);
        attr(t, "ContentType", "application/octet-stream");
        rp_buf_puts(t, " />");
    }
    if (xml_taken) rp_buf_puts(t, "<Override PartName=\"/AppxManifest.xml\" ContentType=\"application/vnd.ms-appx.manifest+xml\" />");
    rp_buf_puts(t, "<Override PartName=\"/AppxBlockMap.xml\" ContentType=\"application/vnd.ms-appx.blockmap+xml\" /></Types>");
}

proven_err_t rp_msix_from_ir(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, uint8_t **out, size_t *len,
                             rp_srcdiags_t *d) {
    *out = NULL;
    *len = 0;
    rp_pos_t top = { 1, 1 };
    size_t errors = d->errors;
    // RFC-0009 M6: what an MSIX cannot carry.
    for (size_t i = 0; i < ir->msix_block_count; ++i) {
        const rp_ir_msix_block_t *b = &ir->msix_blocks[i];
        bool later = strcmp(b->kind, "shortcut") == 0 || strcmp(b->kind, "font") == 0;
        DERR(b->pos, "RP1605", "[%s.%s] cannot go into an MSIX%s; add msi-only = true to build the MSIX without it", b->kind, b->id,
             later ? " yet (planned for P8b)" : "");
    }
    if (!ir->has_msix) DERR(top, "RP1604", "a .msix output needs an [msix] table (identity-name, publisher)");
    if (ir->msix_app_count == 0) DERR(top, "RP1606", "a .msix output needs an [msix-app.ID] table naming the executable");
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;

    // The first application's executable decides the package root.
    const rp_ir_file_t *exe = NULL;
    const rp_ir_msix_app_t *first = &ir->msix_apps[0];
    for (size_t i = 0; i < ir->file_count && first->exe; ++i) {
        if (strcmp(ir->files[i].id, first->exe) == 0) exe = &ir->files[i];
    }
    const rp_ir_dir_t *root = NULL;
    char exe_dir[1024] = "";
    if (exe == NULL || exe->msi_only) {
        DERR(first->pos, "RP1607", "executable = \"%s\" must name a [file.*] that goes into the package", first->exe ? first->exe : "");
    } else if (!below_root(ir, exe->dir, &root, exe_dir, sizeof exe_dir)) {
        DERR(first->pos, "RP1609", "the executable's folder does not lead to a known location");
    }
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;

    // The payload.
    size_t cap = ir->file_count + 3 * ir->msix_app_count + 5, n = 0;
    item_t *items = rp_mem_alloc(alloc, cap, sizeof *items);
    if (items == NULL) return PROVEN_ERR_NOMEM;
    memset(items, 0, cap * sizeof *items);
    for (size_t i = 0; i < ir->file_count; ++i) {
        const rp_ir_file_t *f = &ir->files[i];
        if (f->msi_only) continue;
        const rp_ir_dir_t *r = NULL;
        char dir[1024];
        if (!below_root(ir, f->dir, &r, dir, sizeof dir)) {
            DERR(f->pos, "RP1609", "'%s': its folder does not lead to a known location", f->name);
            continue;
        }
        item_t *it = &items[n];
        if (r == root) {
            snprintf(it->path, sizeof it->path, "%s%s%s", dir, dir[0] ? "\\" : "", f->name);
            if (reserved(it->path)) DERR(f->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
        } else {
            // Elsewhere: the package's virtual file system, under the folder's own path.
            const char *why = NULL, *vfs = vfs_folder(r->base, ir->arch, &why);
            if (vfs == NULL) {
                DERR(f->pos, "RP1609", "'%s' goes to %s: %s; or msi-only = true", f->name, r->base, why);
                continue;
            }
            char top[1024] = "";
            size_t o = 0;
            for (size_t k = 0; k < r->part_count && o < sizeof top; ++k) {
                int w = snprintf(top + o, sizeof top - o, "%s%s", o ? "\\" : "", r->parts[k]);
                if (w < 0) break;
                o += (size_t)w;
            }
            snprintf(it->path, sizeof it->path, "VFS\\%s%s%s%s%s\\%s", vfs, top[0] ? "\\" : "", top, dir[0] ? "\\" : "", dir, f->name);
        }
        ++n;
        it->file_id = f->id;
        it->source = f->source_path;
        it->pos = f->pos;
    }
    // Each application: its executable's package path, and its logos - the three given (checked),
    // or plain ones made here once for all applications.
    app_paths_t *ap = rp_mem_alloc(alloc, ir->msix_app_count, sizeof *ap);
    if (ap == NULL) {
        rp_mem_free(alloc, items);
        return PROVEN_ERR_NOMEM;
    }
    memset(ap, 0, ir->msix_app_count * sizeof *ap);
    static const uint32_t logo_px[3] = { 150, 44, 50 };
    static const char *const logo_default[3] = { "Assets\\DefaultSquare150x150Logo.png", "Assets\\DefaultSquare44x44Logo.png",
                                                 "Assets\\DefaultStoreLogo.png" };
    bool defaults_made = false;
    for (size_t a = 0; a < ir->msix_app_count; ++a) {
        const rp_ir_msix_app_t *app = &ir->msix_apps[a];
        const item_t *x = NULL;
        for (size_t i = 0; i < n && app->exe && !x; ++i) {
            if (items[i].file_id && strcmp(items[i].file_id, app->exe) == 0) x = &items[i];
        }
        if (x == NULL) {
            DERR(app->pos, "RP1607", "executable = \"%s\" must name a [file.*] that goes into the package", app->exe ? app->exe : "");
        } else if (!ieq(extension(x->path) ? extension(x->path) : "", "exe")) {
            DERR(app->pos, "RP1607", "the executable '%s' must be an .exe", x->path);
        } else {
            snprintf(ap[a].exe, sizeof ap[a].exe, "%s", x->path);
        }
        for (int k = 0; k < 3; ++k) {
            if (app->logo_path[k]) {
                const item_t *found = NULL;
                for (size_t i = 0; i < n && !found; ++i) {
                    if (items[i].source && strcmp(items[i].source, app->logo_path[k]) == 0) found = &items[i];
                }
                uint8_t *png = NULL;
                size_t pl = 0;
                uint32_t w = 0, h = 0;
                if (rp_pal_read_file(alloc, app->logo_path[k], 16u << 20, &png, &pl) != PROVEN_OK || !png_size(png, pl, &w, &h)) {
                    DERR(app->pos, "RP1608", "logo '%s' is not a PNG file", app->logo[k]);
                } else if (w != logo_px[k] || h != logo_px[k]) {
                    DERR(app->pos, "RP1608", "logo '%s' is %ux%u pixels; it must be %ux%u", app->logo[k], (unsigned)w, (unsigned)h,
                         (unsigned)logo_px[k], (unsigned)logo_px[k]);
                }
                rp_mem_free(alloc, png);
                if (found) {
                    snprintf(ap[a].logo[k], sizeof ap[a].logo[k], "%s", found->path);
                } else {
                    // Not otherwise installed: it goes in under Assets\ with its own name.
                    const char *b = strrchr(app->logo[k], '/');
                    item_t *it = &items[n++];
                    snprintf(it->path, sizeof it->path, "Assets\\%s", b ? b + 1 : app->logo[k]);
                    it->source = app->logo_path[k];
                    it->pos = app->pos;
                    snprintf(ap[a].logo[k], sizeof ap[a].logo[k], "%s", it->path);
                }
            } else {
                if (!defaults_made) {
                    for (int q = 0; q < 3; ++q) {
                        item_t *it = &items[n++];
                        snprintf(it->path, sizeof it->path, "%s", logo_default[q]);
                        it->pos = app->pos;
                        if (default_logo(alloc, logo_px[q], &it->data, &it->data_len) != PROVEN_OK) DERR(top, "RP1608", "cannot make a default logo");
                    }
                    defaults_made = true;
                }
                snprintf(ap[a].logo[k], sizeof ap[a].logo[k], "%s", logo_default[k]);
            }
        }
    }
    // [registry] values: Registry.dat and User.dat.
    rp_regf_t *machine = rp_regf_new(alloc), *user = rp_regf_new(alloc);
    bool used_m = false, used_u = false;
    if (machine == NULL || user == NULL) DERR(top, "RP1612", "out of memory");
    for (size_t i = 0; machine && user && i < ir->registry_count; ++i) {
        if (!ir->registries[i].msi_only) (void)add_registry(alloc, &ir->registries[i], machine, user, &used_m, &used_u, d);
    }
    for (int h = 0; h < 2; ++h) {
        rp_regf_t *hive = h ? user : machine;
        if (!(h ? used_u : used_m) || d->errors != errors) continue;
        item_t *it = &items[n++];
        snprintf(it->path, sizeof it->path, "%s", h ? "User.dat" : "Registry.dat");
        it->pos = top;
        if (rp_regf_write(hive, &it->data, &it->data_len) != PROVEN_OK) DERR(top, "RP1612", "cannot write %s", it->path);
    }
    rp_regf_free(machine);
    rp_regf_free(user);
    // Two paths that Windows would take for one (letters compared without case).
    for (size_t i = 0; i < n; ++i) {
        for (size_t k = i + 1; k < n; ++k) {
            if (ieq(items[i].path, items[k].path)) DERR(items[k].pos, "RP1611", "'%s' and '%s' are the same path in the package", items[i].path, items[k].path);
        }
    }
    proven_err_t err = d->errors != errors ? PROVEN_ERR_INVALID_FORMAT : PROVEN_OK;

    // The archive: payload, manifest, block map, content types (Windows' order).
    rp_zip_writer_t z;
    rp_zip_begin(&z, alloc, (size_t)1 << 40);
    rp_buf_t bm = rp_buf_new(alloc, (size_t)1 << 30), man = rp_buf_new(alloc, 1u << 20), ct = rp_buf_new(alloc, 1u << 20);
    rp_buf_puts(&bm, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?><BlockMap "
                     "xmlns=\"http://schemas.microsoft.com/appx/2010/blockmap\" xmlns:b4=\"http://schemas.microsoft.com/appx/2021/blockmap\" "
                     "IgnorableNamespaces=\"b4\" HashMethod=\"http://www.w3.org/2001/04/xmlenc#sha256\">");
    for (size_t i = 0; i < n && err == PROVEN_OK; ++i) {
        uint8_t *data = items[i].data;
        size_t dl = items[i].data_len;
        if (items[i].source) {
            err = rp_pal_read_file(alloc, items[i].source, (size_t)1 << 32, &data, &dl);
            if (err != PROVEN_OK) {
                DERR(items[i].pos, "RP1507", "cannot read '%s'", items[i].source);
                break;
            }
        }
        const char *ext = extension(items[i].path);
        err = add_payload(alloc, &z, &bm, items[i].path, NULL, data, dl, !opt->store && !already_compressed(ext));
        if (items[i].source) rp_mem_free(alloc, data);
    }
    if (err == PROVEN_OK) {
        manifest(&man, ir, opt, ap);
        err = man.err;
    }
    if (err == PROVEN_OK) err = add_payload(alloc, &z, &bm, "AppxManifest.xml", NULL, man.data, man.len, true);
    rp_buf_puts(&bm, "</BlockMap>");
    if (err == PROVEN_OK) err = bm.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "AppxBlockMap.xml", NULL, bm.data, bm.len, true);
    content_types(&ct, items, n);
    if (err == PROVEN_OK) err = ct.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "[Content_Types].xml", "[Content_Types].xml", ct.data, ct.len, true);
    if (err == PROVEN_OK) err = z.out.err;
    if (err == PROVEN_OK) err = rp_zip_finish(&z, out, len);
    else rp_zip_abort(&z);
    rp_buf_free(&bm);
    rp_buf_free(&man);
    rp_buf_free(&ct);
    for (size_t i = 0; i < n; ++i) rp_mem_free(alloc, items[i].data);
    rp_mem_free(alloc, items);
    rp_mem_free(alloc, ap);
    return err;
}

// ---- reading --------------------------------------------------------------------------------------
//
// A minimal reader for the block map (RFC-0001 F11: rubrapack's own, for this one schema): elements
// and attributes in double quotes, the five XML entities and numeric character references.

typedef struct {
    const char *p, *end;
} xr_t;

// The next start tag named `name` (with or without a prefix match on the local name), from x->p.
// On success x->p is just after the name; *self_closing tells whether it ends in "/>".
static bool next_tag(xr_t *x, const char *name, const char **tag_end) {
    size_t nl = strlen(name);
    for (const char *q = x->p; q + 1 + nl < x->end; ++q) {
        if (q[0] == '<' && memcmp(q + 1, name, nl) == 0 && (q[1 + nl] == ' ' || q[1 + nl] == '/' || q[1 + nl] == '>' || q[1 + nl] == '\r' || q[1 + nl] == '\n' || q[1 + nl] == '\t')) {
            const char *e = memchr(q, '>', (size_t)(x->end - q));
            if (e == NULL) return false;
            x->p = q + 1 + nl;
            *tag_end = e;
            return true;
        }
    }
    return false;
}

// The value of attribute `name` between x->p and tag_end, decoded into out (NUL-terminated).
static bool attr_value(const char *from, const char *tag_end, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *q = from; q + nl + 2 < tag_end; ++q) {
        if ((q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r' || q[-1] == '\n') && memcmp(q, name, nl) == 0 && q[nl] == '=' && q[nl + 1] == '"') {
            const char *v = q + nl + 2, *ve = memchr(v, '"', (size_t)(tag_end - v));
            if (ve == NULL) return false;
            size_t o = 0;
            for (const char *c = v; c < ve;) {
                if (o + 5 >= cap) return false;
                if (*c != '&') {
                    out[o++] = *c++;
                    continue;
                }
                const char *semi = memchr(c, ';', (size_t)(ve - c));
                if (semi == NULL || semi - c > 10) return false;
                size_t el = (size_t)(semi - c - 1);
                const char *ent = c + 1;
                unsigned long cp = 0;
                if (el == 3 && memcmp(ent, "amp", 3) == 0) cp = '&';
                else if (el == 2 && memcmp(ent, "lt", 2) == 0) cp = '<';
                else if (el == 2 && memcmp(ent, "gt", 2) == 0) cp = '>';
                else if (el == 4 && memcmp(ent, "quot", 4) == 0) cp = '"';
                else if (el == 4 && memcmp(ent, "apos", 4) == 0) cp = '\'';
                else if (el >= 2 && ent[0] == '#') {
                    bool hexa = ent[1] == 'x';
                    for (const char *d = ent + 1 + hexa; d < semi; ++d) {
                        int dv = *d >= '0' && *d <= '9' ? *d - '0' : hexa && (*d | 32) >= 'a' && (*d | 32) <= 'f' ? (*d | 32) - 'a' + 10 : -1;
                        if (dv < 0 || cp > 0x10FFFF) return false;
                        cp = cp * (hexa ? 16 : 10) + (unsigned long)dv;
                    }
                } else {
                    return false;
                }
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
                if (cp < 0x80) out[o++] = (char)cp;
                else if (cp < 0x800) out[o++] = (char)(0xC0 | cp >> 6), out[o++] = (char)(0x80 | (cp & 63));
                else if (cp < 0x10000) out[o++] = (char)(0xE0 | cp >> 12), out[o++] = (char)(0x80 | (cp >> 6 & 63)), out[o++] = (char)(0x80 | (cp & 63));
                else out[o++] = (char)(0xF0 | cp >> 18), out[o++] = (char)(0x80 | (cp >> 12 & 63)), out[o++] = (char)(0x80 | (cp >> 6 & 63)), out[o++] = (char)(0x80 | (cp & 63));
                c = semi + 1;
            }
            out[o] = 0;
            return true;
        }
    }
    return false;
}

static bool parse_u64(const char *s, uint64_t *v) {
    *v = 0;
    if (*s == 0) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || *v > (UINT64_MAX - 9) / 10) return false;
        *v = *v * 10 + (uint64_t)(*s - '0');
    }
    return true;
}

// Percent-decoded ZIP name with '\' for '/', compared with a block map name.
static bool zip_name_is(const char *zip, const char *bm) {
    for (; *zip; ++bm) {
        unsigned ch;
        if (zip[0] == '%') {
            int h = zip[1] >= '0' && zip[1] <= '9' ? zip[1] - '0' : (zip[1] | 32) >= 'a' && (zip[1] | 32) <= 'f' ? (zip[1] | 32) - 'a' + 10 : -1;
            int l = h < 0 ? -1 : zip[2] >= '0' && zip[2] <= '9' ? zip[2] - '0' : (zip[2] | 32) >= 'a' && (zip[2] | 32) <= 'f' ? (zip[2] | 32) - 'a' + 10 : -1;
            if (l < 0) return false;
            ch = (unsigned)(h * 16 + l);
            zip += 3;
        } else {
            ch = *zip == '/' ? '\\' : (unsigned char)*zip;
            ++zip;
        }
        if ((unsigned char)*bm != ch) return false;
    }
    return *bm == 0;
}

static int b64v(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
}

// Decodes base64 of exactly `n` bytes.
static bool unbase64(const char *s, uint8_t *out, size_t n) {
    size_t sl = strlen(s), o = 0;
    if (sl != (n + 2) / 3 * 4) return false;
    for (size_t i = 0; i < sl; i += 4) {
        int a = b64v(s[i]), b = b64v(s[i + 1]), c = s[i + 2] == '=' ? 0 : b64v(s[i + 2]), d = s[i + 3] == '=' ? 0 : b64v(s[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        uint8_t t[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        for (int k = 0; k < 3 && o < n; ++k) out[o++] = t[k];
    }
    return o == n;
}

void rp_msix_files_free(proven_allocator_t alloc, rp_msix_file_t *files, size_t count) {
    for (size_t i = 0; files && i < count; ++i) {
        rp_mem_free(alloc, files[i].name);
        rp_mem_free(alloc, files[i].data);
    }
    rp_mem_free(alloc, files);
}

#define BAD(msg) do { *why = (msg); err = PROVEN_ERR_INVALID_FORMAT; goto out; } while (0)

static proven_err_t open_archive(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_limits_t *lim, bool allow_bundle,
                                 rp_msix_file_t **files, size_t *count, uint8_t **manifest, size_t *manifest_len, const char **why);

// What a package's manifest says of it (the first Identity; every Resource and TargetDeviceFamily).
typedef struct {
    char name[256], publisher[8400], version[64], arch[16];
} identity_t;

static bool read_identity(const char *m, size_t ml, identity_t *id) {
    return rp_xml_attr(m, ml, "Identity", "Name", id->name, sizeof id->name) &&
           rp_xml_attr(m, ml, "Identity", "Publisher", id->publisher, sizeof id->publisher) &&
           rp_xml_attr(m, ml, "Identity", "Version", id->version, sizeof id->version);
}

static proven_err_t bundle_packages(proven_allocator_t alloc, const uint8_t *pkg, const rp_limits_t *lim, const rp_zip_entry_t *e,
                                    size_t ne, bool *used, const char *man, size_t ml, rp_msix_file_t *f, size_t *nf, const char **why) {
    proven_err_t err = PROVEN_OK;
    identity_t bid;
    char archs[64][16];
    size_t narch = 0;
    if (!read_identity(man, ml, &bid)) {
        *why = "the bundle manifest has no Identity Name, Publisher and Version";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    xr_t x = { man, man + ml };
    const char *te;
    while (next_tag(&x, "Package", &te)) {
        char type[32], ver[64], arch[16], file[1024], off_s[32], size_s[32];
        uint64_t off, size;
        const char *from = x.p - 1;
        x.p = te;
        if (!attr_value(from, te, "FileName", file, sizeof file) || !attr_value(from, te, "Offset", off_s, sizeof off_s) ||
            !attr_value(from, te, "Size", size_s, sizeof size_s) || !parse_u64(off_s, &off) || !parse_u64(size_s, &size)) {
            *why = "a Package of the bundle manifest without FileName, Offset or Size";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (!attr_value(from, te, "Type", type, sizeof type)) snprintf(type, sizeof type, "application");
        if (strcmp(type, "application") != 0) {
            *why = "a resource package in the bundle (rubrapack reads application packages only)";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (!attr_value(from, te, "Version", ver, sizeof ver) || !attr_value(from, te, "Architecture", arch, sizeof arch)) {
            *why = "a Package of the bundle manifest without Version or Architecture";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        size_t k = SIZE_MAX;
        for (size_t i = 0; i < ne && k == SIZE_MAX; ++i) {
            if (!used[i] && zip_name_is(e[i].name, file)) k = i;
        }
        if (k == SIZE_MAX) {
            *why = "a package named in the bundle manifest is not in the bundle";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (e[k].method != 0 || e[k].data_off != off || e[k].size != size) {
            *why = "a package's Offset or Size in the bundle manifest does not match where it lies (or it is compressed)";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        for (size_t i = 0; i < narch; ++i) {
            if (strcmp(archs[i], arch) == 0) {
                *why = "two packages of one architecture in the bundle";
                return PROVEN_ERR_INVALID_FORMAT;
            }
        }
        if (narch == sizeof archs / sizeof archs[0]) {
            *why = "too many packages in the bundle";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        snprintf(archs[narch++], sizeof archs[0], "%s", arch);
        used[k] = true;
        // The package itself, as a package; its identity must be the one the bundle states.
        rp_msix_file_t *inner = NULL;
        size_t ni = 0, iml = 0;
        uint8_t *im = NULL;
        const char *iwhy = NULL;
        err = open_archive(alloc, pkg + off, (size_t)size, lim, false, &inner, &ni, &im, &iml, &iwhy);
        identity_t pid;
        bool bad_id = false;
        if (err == PROVEN_OK) {
            bad_id = !read_identity((const char *)im, iml, &pid) ||
                     !rp_xml_attr((const char *)im, iml, "Identity", "ProcessorArchitecture", pid.arch, sizeof pid.arch) ||
                     strcmp(pid.name, bid.name) != 0 || strcmp(pid.publisher, bid.publisher) != 0 || strcmp(pid.version, ver) != 0 ||
                     strcmp(pid.arch, arch) != 0;
        }
        rp_msix_files_free(alloc, inner, ni);
        rp_mem_free(alloc, im);
        if (err != PROVEN_OK) {
            *why = err == PROVEN_ERR_NOMEM ? "out of memory" : iwhy && strcmp(iwhy, "a bundle inside a bundle") == 0 ? iwhy : "a package in the bundle is not a valid MSIX package";
            return err;
        }
        if (bad_id) {
            *why = "a package's identity differs from what the bundle manifest says of it";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        rp_msix_file_t *o = &f[(*nf)++];
        o->name = rp_mem_alloc(alloc, strlen(file) + 1, 1);
        o->data = rp_mem_alloc(alloc, (size_t)size ? (size_t)size : 1, 1);
        if (o->name == NULL || o->data == NULL) return PROVEN_ERR_NOMEM;
        strcpy(o->name, file);
        memcpy(o->data, pkg + off, (size_t)size);
        o->size = size;
        o->deflated = false;
    }
    if (narch == 0) {
        *why = "a bundle without packages";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return PROVEN_OK;
}

static proven_err_t open_archive(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_limits_t *lim, bool allow_bundle,
                                 rp_msix_file_t **files, size_t *count, uint8_t **manifest, size_t *manifest_len, const char **why) {
    *files = NULL;
    *count = 0;
    *manifest = NULL;
    *manifest_len = 0;
    rp_zip_entry_t *e = NULL;
    size_t ne = 0, nf = 0;
    uint8_t *bm = NULL;
    rp_msix_file_t *f = NULL;
    bool *used = NULL;
    proven_err_t err = rp_zip_read(alloc, pkg, len, lim, &e, &ne, why);
    if (err != PROVEN_OK) return err;
    size_t ibm = SIZE_MAX, ict = SIZE_MAX, iman = SIZE_MAX;
    bool bundle = false;
    for (size_t i = 0; i < ne; ++i) {
        if (strcmp(e[i].name, "AppxBlockMap.xml") == 0) ibm = i;
        else if (strcmp(e[i].name, "[Content_Types].xml") == 0) ict = i;
        else if (strcmp(e[i].name, "AppxManifest.xml") == 0) iman = i;
    }
    for (size_t i = 0; i < ne && iman == SIZE_MAX; ++i) {
        if (strcmp(e[i].name, BUNDLE_MANIFEST_ZIP) == 0) iman = i, bundle = true;
    }
    if (bundle && !allow_bundle) BAD("a bundle inside a bundle");
    if (ibm == SIZE_MAX || ict == SIZE_MAX || iman == SIZE_MAX) {
        BAD("not an MSIX package or bundle (AppxBlockMap.xml, the manifest or [Content_Types].xml missing)");
    }
    err = rp_zip_data(alloc, pkg, len, &e[ibm], 64u << 20, &bm, why);
    if (err != PROVEN_OK) goto out;
    xr_t x = { (const char *)bm, (const char *)bm + e[ibm].size };
    const char *te;
    char val[4200];
    if (!next_tag(&x, "BlockMap", &te) || !attr_value(x.p - 1, te, "HashMethod", val, sizeof val)) BAD("the block map has no HashMethod");
    rp_hash_alg_t alg;
    if (strcmp(val, "http://www.w3.org/2001/04/xmlenc#sha256") == 0) alg = RP_HASH_SHA256;
    else if (strcmp(val, "http://www.w3.org/2001/04/xmldsig-more#sha384") == 0) alg = RP_HASH_SHA384;
    else if (strcmp(val, "http://www.w3.org/2001/04/xmlenc#sha512") == 0) alg = RP_HASH_SHA512;
    else BAD("the block map uses a hash rubrapack does not know");
    size_t hl = rp_hash_size(alg);
    f = rp_mem_alloc(alloc, ne ? ne : 1, sizeof *f);
    used = rp_mem_alloc(alloc, ne ? ne : 1, sizeof *used);
    if (f == NULL || used == NULL) {
        err = PROVEN_ERR_NOMEM;
        goto out;
    }
    memset(f, 0, (ne ? ne : 1) * sizeof *f);
    memset(used, 0, (ne ? ne : 1) * sizeof *used);
    for (;;) {
        xr_t fx = x;
        if (!next_tag(&fx, "File", &te)) break;
        x = fx;
        uint64_t size, lfh;
        char sz[32], lf[32];
        if (!attr_value(x.p - 1, te, "Name", val, sizeof val) || !attr_value(x.p - 1, te, "Size", sz, sizeof sz) ||
            !attr_value(x.p - 1, te, "LfhSize", lf, sizeof lf) || !parse_u64(sz, &size) || !parse_u64(lf, &lfh)) {
            BAD("a malformed File in the block map");
        }
        if (nf == ne) BAD("the block map lists more files than the package holds");
        size_t k = SIZE_MAX;
        for (size_t i = 0; i < ne && k == SIZE_MAX; ++i) {
            if (!used[i] && i != ibm && i != ict && zip_name_is(e[i].name, val)) k = i;
        }
        if (k == SIZE_MAX) BAD("a file of the block map is not in the package");
        used[k] = true;
        if (e[k].size != size || e[k].lfh_size != lfh) BAD("a file's size or local header size differs from the block map");
        rp_msix_file_t *out_f = &f[nf++];
        out_f->name = rp_mem_alloc(alloc, strlen(val) + 1, 1);
        if (out_f->name == NULL) {
            err = PROVEN_ERR_NOMEM;
            goto out;
        }
        strcpy(out_f->name, val);
        out_f->size = size;
        out_f->deflated = e[k].method == 8;
        if (size > lim->max_output) {
            *why = "a file larger than allowed";
            err = PROVEN_ERR_OUT_OF_BOUNDS;
            goto out;
        }
        err = rp_zip_data(alloc, pkg, len, &e[k], lim->max_output, &out_f->data, why);
        if (err != PROVEN_OK) goto out;
        // The blocks, up to </File> (or none for a self-closing File).
        const char *file_end = te[-1] == '/' ? te : NULL;
        if (file_end == NULL) {
            for (const char *q = te; q + 7 <= x.end; ++q) {
                if (memcmp(q, "</File>", 7) == 0) {
                    file_end = q;
                    break;
                }
            }
            if (file_end == NULL) BAD("a File without its end in the block map");
        }
        uint64_t nblocks = (size + BLOCK - 1) / BLOCK, csum = 0, seen = 0;
        xr_t bx = { te, file_end };
        const char *be;
        while (next_tag(&bx, "Block", &be)) {
            uint8_t want[RP_HASH_MAX], got[RP_HASH_MAX];
            if (seen == nblocks || !attr_value(bx.p - 1, be, "Hash", val, sizeof val) || !unbase64(val, want, hl)) BAD("a malformed Block in the block map");
            uint64_t off = seen * BLOCK, take = size - off < BLOCK ? size - off : BLOCK;
            rp_hash(alg, out_f->data + off, (size_t)take, got);
            if (memcmp(want, got, hl) != 0) BAD("a block's hash does not match the block map (the package was changed)");
            if (out_f->deflated) {
                uint64_t bs;
                if (!attr_value(bx.p - 1, be, "Size", sz, sizeof sz) || !parse_u64(sz, &bs)) BAD("a deflated file's Block has no Size");
                csum += bs;
            }
            ++seen;
            bx.p = be;
        }
        if (seen != nblocks) BAD("the block map lists the wrong number of blocks for a file");
        if (out_f->deflated && csum + 2 != e[k].csize) BAD("a deflated file's block sizes do not add up to its compressed size");
        x.p = file_end;
    }
    if (!used[iman]) BAD("the manifest is not in the block map");
    for (size_t i = 0; i < nf; ++i) {
        if (strcmp(f[i].name, bundle ? BUNDLE_MANIFEST : "AppxManifest.xml") == 0) {
            *manifest = rp_mem_alloc(alloc, (size_t)f[i].size + 1, 1);
            if (*manifest == NULL) {
                err = PROVEN_ERR_NOMEM;
                goto out;
            }
            memcpy(*manifest, f[i].data, (size_t)f[i].size);
            (*manifest)[f[i].size] = 0;
            *manifest_len = (size_t)f[i].size;
        }
    }
    // A bundle's packages are outside its block map: its manifest names each one, where it lies
    // and how large it is, and each is a package of its own.
    if (bundle) {
        err = bundle_packages(alloc, pkg, lim, e, ne, used, (const char *)*manifest, *manifest_len, f, &nf, why);
        if (err != PROVEN_OK) goto out;
    }
    // Everything but the footprint files must be in the block map (or, in a bundle, be a package).
    for (size_t i = 0; i < ne; ++i) {
        if (used[i] || i == ibm || i == ict || strcmp(e[i].name, "AppxSignature.p7x") == 0 || strncmp(e[i].name, "AppxMetadata/", 13) == 0) continue;
        BAD(bundle ? "a file of the bundle is neither in its block map nor one of its packages" : "a file of the package is not in the block map");
    }
out:
    rp_mem_free(alloc, bm);
    rp_mem_free(alloc, used);
    rp_zip_entries_free(alloc, e, ne);
    if (err != PROVEN_OK) {
        rp_msix_files_free(alloc, f, nf);
        rp_mem_free(alloc, *manifest);
        *manifest = NULL;
        *manifest_len = 0;
        return err;
    }
    *files = f;
    *count = nf;
    return PROVEN_OK;
}

proven_err_t rp_msix_open(proven_allocator_t alloc, const uint8_t *pkg, size_t len, const rp_limits_t *lim, rp_msix_file_t **files,
                          size_t *count, uint8_t **manifest, size_t *manifest_len, const char **why) {
    return open_archive(alloc, pkg, len, lim, true, files, count, manifest, manifest_len, why);
}

bool rp_xml_attr(const char *xml, size_t len, const char *element, const char *attribute, char *out, size_t cap) {
    xr_t x = { xml, xml + len };
    const char *te;
    return next_tag(&x, element, &te) && attr_value(x.p - 1, te, attribute, out, cap);
}

// ---- bundles (RFC-0010 P8b-2) -------------------------------------------------------------------
//
// As Windows' bundle writer (IAppxBundleWriter) lays one out (docs/research/2026-09-27-p8b2-bundle-oracle.md):
// the packages stored in the order given, AppxMetadata/AppxBundleManifest.xml (each package's
// identity, languages and device families, and where its bytes lie), a block map of the manifest
// alone, [Content_Types].xml. The bundle's version is its packages' version.

// Every `element` of a manifest with its attributes `names`, written as `<prefix... a="v".../>` lines.
static void copy_elements(rp_buf_t *m, const char *man, size_t ml, const char *element, const char *out_name, const char *const *names) {
    xr_t x = { man, man + ml };
    const char *te;
    while (next_tag(&x, element, &te)) {
        const char *from = x.p - 1;
        rp_buf_puts(m, "\t\t\t\t<");
        rp_buf_puts(m, out_name);
        for (size_t i = 0; names[i]; ++i) {
            char v[1024];
            if (attr_value(from, te, names[i], v, sizeof v)) attr(m, names[i], v);
        }
        rp_buf_puts(m, "/>\r\n");
        x.p = te;
    }
}

proven_err_t rp_msix_bundle(proven_allocator_t alloc, const rp_msix_part_t *parts, size_t n, const rp_limits_t *lim, uint8_t **out,
                            size_t *len, const char **why) {
    enum { MAX_PARTS = 64 };
    *out = NULL;
    *len = 0;
    if (n == 0 || n > MAX_PARTS) {
        *why = n ? "at most 64 packages in a bundle" : "a bundle needs a package";
        return PROVEN_ERR_INVALID_ARG;
    }
    uint8_t *mans[MAX_PARTS] = { 0 };
    size_t mlens[MAX_PARTS] = { 0 };
    identity_t ids[MAX_PARTS];
    char zip_names[MAX_PARTS][1024];
    proven_err_t err = PROVEN_OK;
    for (size_t i = 0; i < n && err == PROVEN_OK; ++i) {
        rp_msix_file_t *files = NULL;
        size_t nfiles = 0;
        const char *iwhy = NULL;
        err = open_archive(alloc, parts[i].data, parts[i].len, lim, false, &files, &nfiles, &mans[i], &mlens[i], &iwhy);
        rp_msix_files_free(alloc, files, nfiles);
        if (err != PROVEN_OK) {
            *why = err == PROVEN_ERR_NOMEM ? "out of memory" : "a package for the bundle is not a valid MSIX package";
            break;
        }
        const char *m = (const char *)mans[i];
        const char *ext = extension(parts[i].file_name);
        if (!read_identity(m, mlens[i], &ids[i]) || !rp_xml_attr(m, mlens[i], "Identity", "ProcessorArchitecture", ids[i].arch, sizeof ids[i].arch)) {
            *why = "a package without Identity Name, Publisher, Version and ProcessorArchitecture";
        } else if (i && (strcmp(ids[i].name, ids[0].name) != 0 || strcmp(ids[i].publisher, ids[0].publisher) != 0 || strcmp(ids[i].version, ids[0].version) != 0)) {
            *why = "the packages of a bundle need the same Name, Publisher and Version";
        } else if (strlen(parts[i].file_name) > 255 || !ext || (!ieq(ext, "msix") && !ieq(ext, "appx")) || strpbrk(parts[i].file_name, "/\\")) {
            *why = "a package's name in the bundle must be a file name ending in .msix or .appx";
        } else {
            part_name(parts[i].file_name, zip_names[i], sizeof zip_names[i]);
            for (size_t k = 0; k < i && !*why; ++k) {
                if (strcmp(ids[k].arch, ids[i].arch) == 0) *why = "two packages of one architecture in a bundle";
                else if (ieq(parts[k].file_name, parts[i].file_name)) *why = "two packages of one name in a bundle";
            }
        }
        if (*why) err = PROVEN_ERR_INVALID_ARG;
    }
    rp_zip_writer_t z;
    rp_zip_begin(&z, alloc, (size_t)1 << 40);
    rp_buf_t man = rp_buf_new(alloc, 1u << 24), bm = rp_buf_new(alloc, 1u << 20);
    if (err == PROVEN_OK) {
        rp_buf_puts(&man, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n"
                          "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\" SchemaVersion=\"5.0\" "
                          "xmlns:b4=\"http://schemas.microsoft.com/appx/2018/bundle\" xmlns:b5=\"http://schemas.microsoft.com/appx/2019/bundle\" "
                          "IgnorableNamespaces=\"b4 b5\">\r\n\t<Identity");
        attr(&man, "Name", ids[0].name);
        attr(&man, "Publisher", ids[0].publisher);
        attr(&man, "Version", ids[0].version);
        rp_buf_puts(&man, "/>\r\n\t<Packages>\r\n");
        static const char *const res[] = { "Language", NULL }, *const tdf[] = { "Name", "MinVersion", "MaxVersionTested", NULL };
        for (size_t i = 0; i < n; ++i) {
            // Stored entries: a local header of 30 bytes and the name, the bytes, a 24-byte ZIP64 descriptor.
            size_t lfh = 0, before = z.out.len;
            rp_zip_add(&z, zip_names[i], 0, parts[i].data, parts[i].len, rp_crc32(0, parts[i].data, parts[i].len), parts[i].len, &lfh);
            rp_buf_puts(&man, "\t\t<Package Type=\"application\"");
            attr(&man, "Version", ids[i].version);
            attr(&man, "Architecture", ids[i].arch);
            attr(&man, "FileName", parts[i].file_name);
            attr_u64(&man, "Offset", before + lfh);
            attr_u64(&man, "Size", parts[i].len);
            rp_buf_puts(&man, ">\r\n\t\t\t<Resources>\r\n");
            copy_elements(&man, (const char *)mans[i], mlens[i], "Resource", "Resource", res);
            rp_buf_puts(&man, "\t\t\t</Resources>\r\n\t\t\t<b4:Dependencies>\r\n");
            copy_elements(&man, (const char *)mans[i], mlens[i], "TargetDeviceFamily", "b4:TargetDeviceFamily", tdf);
            rp_buf_puts(&man, "\t\t\t</b4:Dependencies>\r\n\t\t</Package>\r\n");
        }
        rp_buf_puts(&man, "\t</Packages>\r\n</Bundle>");
        err = man.err != PROVEN_OK ? man.err : z.out.err;
    }
    if (err == PROVEN_OK) {
        rp_buf_puts(&bm, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n<BlockMap "
                         "xmlns=\"http://schemas.microsoft.com/appx/2010/blockmap\" xmlns:b4=\"http://schemas.microsoft.com/appx/2021/blockmap\" "
                         "IgnorableNamespaces=\"b4\" HashMethod=\"http://www.w3.org/2001/04/xmlenc#sha256\">");
        err = add_payload(alloc, &z, &bm, BUNDLE_MANIFEST, NULL, man.data, man.len, true);
    }
    rp_buf_puts(&bm, "</BlockMap>");
    if (err == PROVEN_OK) err = bm.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "AppxBlockMap.xml", NULL, bm.data, bm.len, true);
    static const char ct[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                             "<Default Extension=\"msix\" ContentType=\"application/vnd.ms-appx\" />"
                             "<Default Extension=\"xml\" ContentType=\"application/vnd.ms-appx.bundlemanifest+xml\" />"
                             "<Override PartName=\"/AppxBlockMap.xml\" ContentType=\"application/vnd.ms-appx.blockmap+xml\" /></Types>";
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "[Content_Types].xml", "[Content_Types].xml", (const uint8_t *)ct, sizeof ct - 1, true);
    if (err == PROVEN_OK) err = z.out.err;
    if (err == PROVEN_OK) err = rp_zip_finish(&z, out, len);
    else rp_zip_abort(&z);
    rp_buf_free(&man);
    rp_buf_free(&bm);
    for (size_t i = 0; i < n; ++i) rp_mem_free(alloc, mans[i]);
    return err;
}
