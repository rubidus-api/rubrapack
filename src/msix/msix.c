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
#include "rubrapack/num.h"
#include "rubrapack/pal.h"
#include "rubrapack/parts.h"
#include "rubrapack/pe.h"
#include "rubrapack/pri.h"
#include "rubrapack/regf.h"
#include "rubrapack/text.h"
#include "rubrapack/zip.h"

#include "proven/encode.h"
#include "proven/hash.h"

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

// Standard padded base64 of n bytes, NUL-terminated; out holds (n + 2) / 3 * 4 + 1 characters.
static void base64(const uint8_t *p, size_t n, char *out) {
    proven_size_t w = 0;
    if (proven_base64_encode((proven_mem_view_t){ p, n }, (proven_byte_t *)out, proven_base64_encoded_size(n), &w) != PROVEN_OK) w = 0;
    out[w] = 0;
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
    be32(b, b->err == PROVEN_OK ? proven_crc32((proven_mem_view_t){ (const proven_byte_t *)(b->data + at), 4 + n }) : 0);
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
        *why = "a font goes into an MSIX through a [font.*] that names it";
    } else if (strcmp(base, "Temp") == 0) {
        *why = "an MSIX cannot place files in the temporary folder";
    } else {
        *why = "Start menu, Programs, Desktop and Startup entries come from [shortcut.*] and [msix-extension.*]";
    }
    return NULL;
}

static bool is_font(const rp_ir_t *ir, const char *file_id) {
    for (size_t k = 0; k < ir->font_count; ++k) {
        if (ir->fonts[k].file && strcmp(ir->fonts[k].file, file_id) == 0) return true;
    }
    return false;
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
        rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: HKCR does not reach outside an MSIX; use [assoc.*], [protocol.*], [com.*] or [handler.*], or msi-only = true", r->id);
        return false;
    }
    if (r->when) rp_srcdiag_add(d, r->pos, "RP1612", false, "[registry.%s]: an MSIX writes all its values (when); use msi-only = true", r->id);
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
        int64_t v = rp_int_or_zero(r->value);
        rp_buf_u32le(&data, (uint32_t)v);
        break;
    }
    case RP_REG_QWORD: {
        type = 11;
        uint64_t v = 0;
        int64_t sv = 0;
        if (strncmp(r->value, "0x", 2) == 0 || strncmp(r->value, "0X", 2) == 0) (void)rp_read_hex64(r->value, NULL, &v);
        else if (rp_read_i64(r->value, NULL, &sv)) v = (uint64_t)sv;
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
        rp_zip_add(z, part, deflate ? 8 : 0, deflate ? comp.data : data, deflate ? comp.len : n, proven_crc32((proven_mem_view_t){ (const proven_byte_t *)(data), n }), n, &lfh);
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
    char start[2200];           // what the manifest starts: exe, or rubrapack's launcher (RFC-0019)
    char logo[3][2200];         // Square150x150, Square44x44, StoreLogo
    bool loc_display, loc_description;      // written as ms-resource: (RFC-0016 3)
} app_paths_t;

// RFC-0016 3: what resources.pri holds - texts in several languages, logos in several scales.
enum { LOC_MAX = 512, LOC_LANGS = 32 };
typedef struct {
    proven_allocator_t alloc;
    rp_pri_candidate_t c[LOC_MAX];
    size_t             n;
    char              *own[3 * LOC_MAX];
    size_t             nown;
    const char        *langs[LOC_LANGS];    // languages besides the package's, in the order first seen
    size_t             nlangs;
    const char        *default_lang;
    bool               pkg_display, pub_display;
    bool               full;
} loc_t;

static const char *loc_dup(loc_t *l, const char *fmt, const char *a, const char *b) {
    char buf[2400];
    snprintf(buf, sizeof buf, fmt, a, b);
    size_t n = strlen(buf);
    if (l->nown == sizeof l->own / sizeof l->own[0]) {
        l->full = true;
        return "";
    }
    char *s = rp_mem_alloc(l->alloc, n + 1, 1);
    if (s == NULL) {
        l->full = true;
        return "";
    }
    memcpy(s, buf, n + 1);
    l->own[l->nown++] = s;
    return s;
}

static void loc_add(loc_t *l, const char *name, int qualifier, const char *qvalue, bool path, const char *value) {
    for (size_t i = 0; i < l->n; ++i) {        // the same logo for two applications: once
        if (strcmp(l->c[i].name, name) == 0 && l->c[i].qualifier == qualifier &&
            (qvalue == NULL ? l->c[i].qvalue == NULL : l->c[i].qvalue && strcmp(l->c[i].qvalue, qvalue) == 0)) {
            return;
        }
    }
    if (l->n == LOC_MAX) {
        l->full = true;
        return;
    }
    l->c[l->n++] = (rp_pri_candidate_t){ name, qualifier, qvalue, path, value };
}

// A text with -xx variants becomes Resources/<name> in each language; false when it has none.
static bool loc_text(loc_t *l, const char *name, const char *text, const rp_ir_ltext_t *by_lang, size_t count, rp_pos_t pos, rp_srcdiags_t *d) {
    if (count == 0) return false;
    const char *res = loc_dup(l, "Resources/%s%s", name, "");
    loc_add(l, res, RP_PRI_LANGUAGE, l->default_lang, false, text);
    for (size_t k = 0; k < count; ++k) {
        if (strncmp(by_lang[k].lang, l->default_lang, 2) == 0) {
            rp_srcdiag_add(d, pos, "RP1606", false, "'-%s' is the package's own language (%s); give that text without the suffix", by_lang[k].lang,
                           l->default_lang);
            continue;
        }
        loc_add(l, res, RP_PRI_LANGUAGE, by_lang[k].lang, false, by_lang[k].text);
        bool seen = false;
        for (size_t j = 0; j < l->nlangs; ++j) seen |= strcmp(l->langs[j], by_lang[k].lang) == 0;
        if (!seen && l->nlangs < LOC_LANGS) l->langs[l->nlangs++] = by_lang[k].lang;
    }
    return true;
}

static void loc_free(loc_t *l) {
    for (size_t i = 0; i < l->nown; ++i) rp_mem_free(l->alloc, l->own[i]);
    l->nown = 0;
}

// The namespaces the extensions use (RFC-0010 N4), declared only when used so that a package without
// extensions keeps the P8a manifest.
enum { NS_UAP3 = 1, NS_UAP4 = 2, NS_DESKTOP = 4, NS_DESKTOP7 = 8, NS_DESKTOP2 = 16, NS_DESKTOP6 = 32, NS_COM = 64,
       NS_DESKTOP4 = 128, NS_UAP6 = 256, NS_UAP7 = 512, NS_RESCAP6 = 1024, NS_COUNT = 11 };
// Capabilities the extensions need, in the same flag word above the namespaces (RFC-0016 2).
enum { CAP_SERVICES = 1 << 16, CAP_SYSTEM_SERVICES = 1 << 17, CAP_UNVIRTUALIZED = 1 << 18 };

// [msix] capabilities (RFC-0018): the names Microsoft Learn's "App capability declarations" lists
// for packaged desktop apps, each with the element that declares it. The manifest lists the
// Capability elements first, then the DeviceCapability ones, as its schema asks.
enum { C_FOUNDATION, C_UAP, C_UAP3, C_UAP6, C_UAP7, C_RESCAP, C_DEVICE };
static const struct {
    const char *name;
    int         kind;
} capability_names[] = {
    { "internetClient", C_FOUNDATION }, { "internetClientServer", C_FOUNDATION }, { "privateNetworkClientServer", C_FOUNDATION },
    { "allJoyn", C_FOUNDATION }, { "codeGeneration", C_FOUNDATION },
    { "documentsLibrary", C_UAP }, { "picturesLibrary", C_UAP }, { "videosLibrary", C_UAP }, { "musicLibrary", C_UAP },
    { "removableStorage", C_UAP }, { "enterpriseAuthentication", C_UAP }, { "sharedUserCertificates", C_UAP },
    { "appointments", C_UAP }, { "contacts", C_UAP }, { "userAccountInformation", C_UAP }, { "objects3D", C_UAP },
    { "phoneCall", C_UAP }, { "voipCall", C_UAP }, { "chat", C_UAP }, { "blockedChatMessages", C_UAP },
    { "backgroundMediaPlayback", C_UAP3 }, { "remoteSystem", C_UAP3 }, { "userNotificationListener", C_UAP3 },
    { "graphicsCapture", C_UAP6 }, { "globalMediaControl", C_UAP7 },
    { "allowElevation", C_RESCAP }, { "unvirtualizedResources", C_RESCAP }, { "broadFileSystemAccess", C_RESCAP },
    { "packageManagement", C_RESCAP }, { "packageQuery", C_RESCAP }, { "confirmAppClose", C_RESCAP },
    { "appDiagnostics", C_RESCAP }, { "appLicensing", C_RESCAP }, { "localSystemServices", C_RESCAP },
    { "packagedServices", C_RESCAP }, { "extendedExecutionUnconstrained", C_RESCAP }, { "extendedBackgroundTaskTime", C_RESCAP },
    { "inputForegroundObservation", C_RESCAP }, { "inputObservation", C_RESCAP }, { "inputSuppression", C_RESCAP },
    { "inputInjectionBrokered", C_RESCAP }, { "uiAccess", C_RESCAP }, { "interopServices", C_RESCAP },
    { "customInstallActions", C_RESCAP }, { "modifiableApp", C_RESCAP }, { "appCaptureSettings", C_RESCAP },
    { "webcam", C_DEVICE }, { "microphone", C_DEVICE }, { "location", C_DEVICE }, { "bluetooth", C_DEVICE },
    { "proximity", C_DEVICE }, { "radios", C_DEVICE }, { "wiFiControl", C_DEVICE }, { "lowLevel", C_DEVICE },
    { "gazeInput", C_DEVICE },
};

static int capability_kind(const char *name) {
    for (size_t k = 0; k < sizeof capability_names / sizeof capability_names[0]; ++k) {
        if (strcmp(capability_names[k].name, name) == 0) return capability_names[k].kind;
    }
    return -1;
}

// Where a file of `name` in the dir `dir_id` goes in the package: below the package's own folder,
// in its Fonts folder (a [font.*]), or in its virtual file system under the folder's own path.
// 1 when placed, 0 when the folder has no place in an MSIX (*why says why, with the folder's
// base), -1 when the dir does not lead to a known location; 2 for the package's own folder, where
// names the MSIX format keeps for itself must be refused (reserved).
static int place(const rp_ir_t *ir, const rp_ir_dir_t *root, const char *dir_id, const char *name, bool font, char *out, size_t cap,
                 const char **why) {
    const rp_ir_dir_t *r = NULL;
    char dir[1024];
    if (!below_root(ir, dir_id, &r, dir, sizeof dir)) return -1;
    if (r == root) {
        snprintf(out, cap, "%s%s%s", dir, dir[0] ? "\\" : "", name);
        return 2;
    }
    if (strcmp(r->base, "Fonts") == 0 && font) {
        // A [font.*] (RFC-0010 N4): in the package's Fonts folder, shared through uap4:SharedFonts.
        snprintf(out, cap, "Fonts\\%s", name);
        return 1;
    }
    // Elsewhere: the package's virtual file system, under the folder's own path.
    static char because[256];
    const char *vfs = vfs_folder(r->base, ir->arch, why);
    if (vfs == NULL) {
        snprintf(because, sizeof because, "%s: %s", r->base, *why ? *why : "no place in an MSIX");
        *why = because;
        return 0;
    }
    char top[1024] = "";
    size_t o = 0;
    for (size_t k = 0; k < r->part_count && o < sizeof top; ++k) {
        int w = snprintf(top + o, sizeof top - o, "%s%s", o ? "\\" : "", r->parts[k]);
        if (w < 0) break;
        o += (size_t)w;
    }
    snprintf(out, cap, "VFS\\%s%s%s%s%s\\%s", vfs, top[0] ? "\\" : "", top, dir[0] ? "\\" : "", dir, name);
    return 1;
}

// An [ini.*] value in an MSIX (RFC-0019): written when the package is built, so it may hold no
// part Windows Installer would fill in; [\[] and [\]] stand for the brackets. NULL when it has one.
static char *ini_literal(proven_allocator_t alloc, const char *v) {
    size_t n = strlen(v);
    char *o = rp_mem_alloc(alloc, n + 1, 1);
    if (o == NULL) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        if (v[i] == '[' && i + 3 < n + 1 && v[i + 1] == '\\' && v[i + 3] == ']') {
            o[w++] = v[i + 2];
            i += 4;
        } else if (v[i] == '[') {
            rp_mem_free(alloc, o);
            return NULL;
        } else {
            o[w++] = v[i++];
        }
    }
    o[w] = '\0';
    return o;
}

// The INI files the [ini.*] tables of one file make, as Windows reads them: ASCII, or UTF-16LE with
// a BOM when a text is not ASCII; CRLF lines; sections and keys in the order the source first
// names them; `add` joins the values with commas; `remove` (for what an older version left) has
// nothing to remove in a new package.
static proven_err_t ini_file(proven_allocator_t alloc, const rp_ir_t *ir, const char *dir, const char *file, uint8_t **out, size_t *len,
                             rp_srcdiags_t *d) {
    rp_buf_t t = rp_buf_new(alloc, 1u << 24);
    // sections in first-appearance order
    for (size_t i = 0; i < ir->ini_count; ++i) {
        const rp_ir_ini_t *x = &ir->inis[i];
        if (x->msi_only || strcmp(x->dir, dir) != 0 || strcmp(x->file, file) != 0 || x->mode == 2) continue;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) {
            const rp_ir_ini_t *y = &ir->inis[j];
            seen = !y->msi_only && y->mode != 2 && strcmp(y->dir, dir) == 0 && strcmp(y->file, file) == 0 && strcmp(y->section, x->section) == 0;
        }
        if (seen) continue;
        rp_buf_puts(&t, "[");
        rp_buf_puts(&t, x->section);
        rp_buf_puts(&t, "]\r\n");
        for (size_t k = i; k < ir->ini_count; ++k) {
            const rp_ir_ini_t *z = &ir->inis[k];
            if (z->msi_only || z->mode == 2 || strcmp(z->dir, dir) != 0 || strcmp(z->file, file) != 0 || strcmp(z->section, x->section) != 0) continue;
            bool done = false;              // a key written already (by an earlier table)
            for (size_t j = i; j < k && !done; ++j) {
                const rp_ir_ini_t *y = &ir->inis[j];
                done = !y->msi_only && y->mode != 2 && strcmp(y->dir, dir) == 0 && strcmp(y->file, file) == 0 && strcmp(y->section, x->section) == 0 &&
                       strcmp(y->key, z->key) == 0;
            }
            if (done) continue;
            rp_buf_puts(&t, z->key);
            rp_buf_puts(&t, "=");
            bool first = true;
            // set: the last set value; add: every added value after it, comma-separated
            const rp_ir_ini_t *base = NULL;
            for (size_t j = k; j < ir->ini_count; ++j) {
                const rp_ir_ini_t *y = &ir->inis[j];
                if (!y->msi_only && y->mode == 0 && strcmp(y->dir, dir) == 0 && strcmp(y->file, file) == 0 && strcmp(y->section, x->section) == 0 &&
                    strcmp(y->key, z->key) == 0) {
                    base = y;
                }
            }
            for (size_t j = k; j < ir->ini_count; ++j) {
                const rp_ir_ini_t *y = &ir->inis[j];
                if (y->msi_only || y->mode == 2 || strcmp(y->dir, dir) != 0 || strcmp(y->file, file) != 0 || strcmp(y->section, x->section) != 0 ||
                    strcmp(y->key, z->key) != 0 || (y->mode == 0 && y != base)) {
                    continue;
                }
                if (y->when) DERR(y->pos, "RP1612", "[ini.%s]: an MSIX writes all its INI entries (when); use msi-only = true", y->id);
                char *v = ini_literal(alloc, y->value ? y->value : "");
                if (v == NULL) {
                    DERR(y->pos, "RP1612", "[ini.%s]: its value has a part Windows Installer fills in at install time ([...]); an MSIX "
                         "writes the file when it is built", y->id);
                    continue;
                }
                if (!first) rp_buf_puts(&t, ",");
                rp_buf_puts(&t, v);
                rp_mem_free(alloc, v);
                first = false;
            }
            rp_buf_puts(&t, "\r\n");
        }
    }
    uint8_t *text = NULL;
    size_t n = 0;
    proven_err_t err = rp_buf_take(&t, &text, &n);
    if (err != PROVEN_OK) return err;
    bool ascii = true;
    for (size_t i = 0; i < n; ++i) ascii &= text[i] < 0x80;
    if (ascii) {
        *out = text;
        *len = n;
        return PROVEN_OK;
    }
    rp_text_result_t r = rp_utf8_to_utf16(text, n, NULL, 0);
    uint16_t *w = r.err == PROVEN_OK ? rp_mem_alloc(alloc, r.units + 1, sizeof *w) : NULL;
    uint8_t *b = w ? rp_mem_alloc(alloc, 2 * r.units + 2, 1) : NULL;
    if (b == NULL) {
        rp_mem_free(alloc, w);
        rp_mem_free(alloc, text);
        return PROVEN_ERR_NOMEM;
    }
    (void)rp_utf8_to_utf16(text, n, w, r.units);
    b[0] = 0xFF;
    b[1] = 0xFE;
    for (size_t i = 0; i < r.units; ++i) {
        b[2 + 2 * i] = (uint8_t)w[i];
        b[3 + 2 * i] = (uint8_t)(w[i] >> 8);
    }
    rp_mem_free(alloc, w);
    rp_mem_free(alloc, text);
    *out = b;
    *len = 2 * r.units + 2;
    return PROVEN_OK;
}

// An [env.*] value for the launcher (RFC-0019): the Windows Installer parts the IR made become what
// the launcher expands when the application starts - a dir of the package %PKG%\..., a Windows folder
// or name its environment variable; [\x] is x. False for a part an MSIX has no answer for.
static bool env_value(const rp_ir_t *ir, const rp_ir_dir_t *root, const char *v, rp_buf_t *out) {
    static const char *const props[][2] = {
        { "ProgramFiles64Folder", "%ProgramW6432%\\" }, { "ProgramFilesFolder", "%ProgramFiles(x86)%\\" },
        { "CommonFiles64Folder", "%CommonProgramW6432%\\" }, { "CommonFilesFolder", "%CommonProgramFiles(x86)%\\" },
        { "CommonAppDataFolder", "%ProgramData%\\" }, { "AppDataFolder", "%APPDATA%\\" }, { "LocalAppDataFolder", "%LOCALAPPDATA%\\" },
        { "TempFolder", "%TEMP%\\" }, { "WindowsFolder", "%SystemRoot%\\" }, { "System64Folder", "%SystemRoot%\\System32\\" },
        { "LogonUser", "%USERNAME%" }, { "ComputerName", "%COMPUTERNAME%" },
    };
    for (size_t i = 0; v[i];) {
        if (v[i] != '[') {
            rp_buf_byte(out, (uint8_t)v[i++]);
            continue;
        }
        const char *close = strchr(v + i, ']');
        if (close == NULL) return false;
        size_t n = (size_t)(close - (v + i + 1));
        const char *in = v + i + 1;
        char name[128];
        if (n == 0 || n >= sizeof name) return false;
        memcpy(name, in, n);
        name[n] = 0;
        i += n + 2;
        if (name[0] == '\\' && n == 2) {
            rp_buf_byte(out, (uint8_t)name[1]);
            continue;
        }
        if (name[0] == '%') {
            rp_buf_byte(out, '%');
            rp_buf_puts(out, name + 1);
            rp_buf_byte(out, '%');
            continue;
        }
        bool done = false;
        for (size_t k = 0; k < sizeof props / sizeof props[0] && !done; ++k) {
            if (strcmp(name, props[k][0]) == 0) {
                rp_buf_puts(out, props[k][1]);
                done = true;
            }
        }
        if (done) continue;
        const rp_ir_dir_t *r = NULL;
        char rel[1024];
        if (find_dir(ir, name) == NULL || !below_root(ir, name, &r, rel, sizeof rel) || r != root) return false;
        rp_buf_puts(out, "%PKG%\\");
        if (rel[0]) {
            rp_buf_puts(out, rel);
            rp_buf_byte(out, '\\');
        }
    }
    return true;
}

static void manifest(rp_buf_t *m, const rp_ir_t *ir, const rp_msix_options_t *opt, const app_paths_t *ap, const rp_buf_t *ext, unsigned ns,
                     const loc_t *loc, const char *store_logo) {
    static const char *const arch[] = { "x64", "arm64", "x86" };
    char version[32], publisher[8400];
    unsigned v[4] = { 0 };
    for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
    snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    snprintf(publisher, sizeof publisher, "%s%s%s", ir->msix_publisher, opt->unsigned_test ? ", " : "", opt->unsigned_test ? UNSIGNED_OID : "");
    rp_buf_puts(m, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                   "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\"\r\n"
                   "         xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\"\r\n"
                   "         xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\"\r\n");
    static const char *const ns_uri[][2] = { { "uap3", "http://schemas.microsoft.com/appx/manifest/uap/windows10/3" },
                                             { "uap4", "http://schemas.microsoft.com/appx/manifest/uap/windows10/4" },
                                             { "desktop", "http://schemas.microsoft.com/appx/manifest/desktop/windows10" },
                                             { "desktop7", "http://schemas.microsoft.com/appx/manifest/desktop/windows10/7" },
                                             { "desktop2", "http://schemas.microsoft.com/appx/manifest/desktop/windows10/2" },
                                             { "desktop6", "http://schemas.microsoft.com/appx/manifest/desktop/windows10/6" },
                                             { "com", "http://schemas.microsoft.com/appx/manifest/com/windows10" },
                                             { "desktop4", "http://schemas.microsoft.com/appx/manifest/desktop/windows10/4" },
                                             { "uap6", "http://schemas.microsoft.com/appx/manifest/uap/windows10/6" },
                                             { "uap7", "http://schemas.microsoft.com/appx/manifest/uap/windows10/7" },
                                             { "rescap6", "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities/6" } };
    for (int k = 0; k < NS_COUNT; ++k) {
        if (!(ns & (1u << k))) continue;
        rp_buf_puts(m, "         xmlns:");
        rp_buf_puts(m, ns_uri[k][0]);
        rp_buf_puts(m, "=\"");
        rp_buf_puts(m, ns_uri[k][1]);
        rp_buf_puts(m, "\"\r\n");
    }
    rp_buf_puts(m, "         IgnorableNamespaces=\"uap rescap");
    for (int k = 0; k < NS_COUNT; ++k) {
        if (!(ns & (1u << k))) continue;
        rp_buf_byte(m, ' ');
        rp_buf_puts(m, ns_uri[k][0]);
    }
    rp_buf_puts(m, "\">\r\n  <Identity");
    attr(m, "Name", ir->msix_identity_name);
    attr(m, "Publisher", publisher);
    attr(m, "Version", version);
    attr(m, "ProcessorArchitecture", arch[ir->arch]);
    rp_buf_puts(m, " />\r\n  <Properties>\r\n    <DisplayName>");
    xml_text(m, loc->pkg_display ? "ms-resource:PackageDisplayName" : ir->msix_display ? ir->msix_display : ir->name);
    rp_buf_puts(m, "</DisplayName>\r\n    <PublisherDisplayName>");
    xml_text(m, loc->pub_display ? "ms-resource:PublisherDisplayName" : ir->msix_publisher_display ? ir->msix_publisher_display : ir->manufacturer);
    rp_buf_puts(m, "</PublisherDisplayName>\r\n    <Logo>");
    xml_text(m, store_logo);
    rp_buf_puts(m, "</Logo>\r\n");
    // RFC-0018: write virtualization turned off (needs unvirtualizedResources, added below).
    if (ir->msix_no_fs_virt) rp_buf_puts(m, "    <desktop6:FileSystemWriteVirtualization>disabled</desktop6:FileSystemWriteVirtualization>\r\n");
    if (ir->msix_no_reg_virt) rp_buf_puts(m, "    <desktop6:RegistryWriteVirtualization>disabled</desktop6:RegistryWriteVirtualization>\r\n");
    if (ir->msix_modification) rp_buf_puts(m, "    <rescap6:ModificationPackage>true</rescap6:ModificationPackage>\r\n");    // RFC-0019
    rp_buf_puts(m, "  </Properties>\r\n  <Dependencies>\r\n    <TargetDeviceFamily Name=\"Windows.Desktop\"");
    attr(m, "MinVersion", ir->msix_min_version ? ir->msix_min_version : "10.0.17763.0");
    rp_buf_puts(m, " MaxVersionTested=\"10.0.26100.0\" />\r\n");
    for (size_t k = 0; k < ir->msix_dep_count; ++k) {         // framework packages (RFC-0018)
        rp_buf_puts(m, "    <PackageDependency");
        attr(m, "Name", ir->msix_deps[k].name);
        attr(m, "Publisher", ir->msix_deps[k].publisher);
        attr(m, "MinVersion", ir->msix_deps[k].min_version);
        rp_buf_puts(m, " />\r\n");
    }
    // RFC-0019: the main package of an optional or modification package (uap4 when it names the
    // publisher; a modification package always does, its own when not given).
    if (ir->msix_main) {
        bool uap4 = ir->msix_main_publisher || ir->msix_modification;
        rp_buf_puts(m, uap4 ? "    <uap4:MainPackageDependency" : "    <uap3:MainPackageDependency");
        attr(m, "Name", ir->msix_main);
        if (uap4) attr(m, "Publisher", ir->msix_main_publisher ? ir->msix_main_publisher : publisher);
        rp_buf_puts(m, " />\r\n");
    }
    rp_buf_puts(m, "  </Dependencies>\r\n  <Resources>\r\n    <Resource");
    attr(m, "Language", ir->language == 1042 ? "ko-KR" : "en-US");
    rp_buf_puts(m, " />\r\n");
    for (size_t k = 0; k < loc->nlangs; ++k) {
        rp_buf_puts(m, "    <Resource");
        attr(m, "Language", loc->langs[k]);
        rp_buf_puts(m, " />\r\n");
    }
    rp_buf_puts(m, "  </Resources>\r\n");
    if (ir->msix_app_count) rp_buf_puts(m, "  <Applications>\r\n");
    for (size_t i = 0; i < ir->msix_app_count; ++i) {
        const rp_ir_msix_app_t *a = &ir->msix_apps[i];
        const char *display = a->display ? a->display : ir->name;
        rp_buf_puts(m, "    <Application");
        attr(m, "Id", a->id);
        attr(m, "Executable", ap[i].start[0] ? ap[i].start : ap[i].exe);
        rp_buf_puts(m, " EntryPoint=\"Windows.FullTrustApplication\">\r\n      <uap:VisualElements");
        char res[160];
        snprintf(res, sizeof res, "ms-resource:%sDisplayName", a->id);
        attr(m, "DisplayName", ap[i].loc_display ? res : display);
        snprintf(res, sizeof res, "ms-resource:%sDescription", a->id);
        attr(m, "Description", ap[i].loc_description ? res : a->description ? a->description : display);
        attr(m, "BackgroundColor", a->background ? a->background : "transparent");
        attr(m, "Square150x150Logo", ap[i].logo[0]);
        attr(m, "Square44x44Logo", ap[i].logo[1]);
        if (a->hidden) rp_buf_puts(m, " AppListEntry=\"none\"");
        rp_buf_puts(m, " />\r\n");
        if (ext[i].len) {
            rp_buf_puts(m, "      <Extensions>\r\n");
            rp_buf_put(m, ext[i].data, ext[i].len);
            rp_buf_puts(m, "      </Extensions>\r\n");
        }
        rp_buf_puts(m, "    </Application>\r\n");
    }
    if (ir->msix_app_count) rp_buf_puts(m, "  </Applications>\r\n");
    const rp_buf_t *pkg = &ext[ir->msix_app_count];     // package-level extensions (firewall rules)
    if (pkg->len) {
        rp_buf_puts(m, "  <Extensions>\r\n");
        rp_buf_put(m, pkg->data, pkg->len);
        rp_buf_puts(m, "  </Extensions>\r\n");
    }
    // Capabilities: those the source names (RFC-0018), in the element order of capability_names'
    // kinds, then the device capabilities; runFullTrust always, and what the features need.
    static const char *const cap_elem[] = { "Capability", "uap:Capability", "uap3:Capability", "uap6:Capability", "uap7:Capability",
                                            "rescap:Capability", "DeviceCapability" };
    // An optional or modification package has none: its main package's apply (RFC-0019).
    if (ir->msix_main) {
        rp_buf_puts(m, "</Package>\r\n");
        return;
    }
    rp_buf_puts(m, "  <Capabilities>\r\n");
    for (int kind = C_FOUNDATION; kind <= C_DEVICE; ++kind) {
        if (kind == C_RESCAP) {
            rp_buf_puts(m, "    <rescap:Capability Name=\"runFullTrust\" />\r\n");
            if (ns & CAP_SERVICES) rp_buf_puts(m, "    <rescap:Capability Name=\"packagedServices\" />\r\n");
            if (ns & CAP_SYSTEM_SERVICES) rp_buf_puts(m, "    <rescap:Capability Name=\"localSystemServices\" />\r\n");
            if (ns & CAP_UNVIRTUALIZED) rp_buf_puts(m, "    <rescap:Capability Name=\"unvirtualizedResources\" />\r\n");
        }
        for (size_t k = 0; k < ir->msix_cap_count; ++k) {
            const char *c = ir->msix_caps[k];
            if (capability_kind(c) != kind) continue;
            if ((strcmp(c, "packagedServices") == 0 && (ns & CAP_SERVICES)) || (strcmp(c, "localSystemServices") == 0 && (ns & CAP_SYSTEM_SERVICES)) ||
                (strcmp(c, "unvirtualizedResources") == 0 && (ns & CAP_UNVIRTUALIZED))) {
                continue;               // there already
            }
            bool dup = false;
            for (size_t j = 0; j < k; ++j) dup |= strcmp(ir->msix_caps[j], c) == 0;
            if (dup) continue;
            rp_buf_puts(m, "    <");
            rp_buf_puts(m, cap_elem[kind]);
            attr(m, "Name", c);
            rp_buf_puts(m, " />\r\n");
        }
    }
    rp_buf_puts(m, "  </Capabilities>\r\n</Package>\r\n");
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

// ---- extensions (RFC-0010 N4; the support table: manual/formats/msix.md "Extensions") -------------

// The build number of min-version ("10.0.17763.0" -> 17763).
static unsigned min_build(const rp_ir_t *ir) {
    const char *v = ir->msix_min_version ? ir->msix_min_version : "10.0.17763.0";
    for (int dots = 0; *v && dots < 2; ++v) dots += *v == '.';
    uint64_t b = 0;
    return rp_read_u64(v, NULL, &b) && b <= 0xFFFFFFFFu ? (unsigned)b : 0;
}

// RFC-0024: the [handler.*] elements of a file type association (desktop2), for the handlers whose
// types are all among `exts` (count n); `want` 1 = those, 0 = the handlers with no type there.
static bool handler_in(const rp_ir_handler_t *h, const char *const *exts, size_t n, bool all) {
    size_t in = 0;
    for (size_t t = 0; t < h->type_count; ++t) {
        for (size_t e = 0; e < n; ++e) {
            if (h->types[t] && exts[e] && strcmp(h->types[t], exts[e]) == 0) {
                ++in;
                break;
            }
        }
    }
    return all ? in == h->type_count : in > 0;
}

// The same set of types (as sets).
static bool same_types(const rp_ir_handler_t *a, const rp_ir_handler_t *b) {
    return handler_in(a, (const char *const *)b->types, b->type_count, true) && handler_in(b, (const char *const *)a->types, a->type_count, true);
}

// A handler the package carries: thumbnail and preview handlers. A packaged property handler was
// seen giving Explorer nothing (DECISIONS 2026-10-02), so property handlers are the MSI's only.
static bool packaged_handler(const rp_ir_handler_t *h) {
    return !h->msi_only && h->kind != RP_HANDLER_PROPERTY;
}

// The handler elements of one file type association: preview handlers first, then thumbnail
// handlers (the order seen working), each for a handler whose types pass keep().
static void handler_list(rp_buf_t *b, const rp_ir_t *ir, bool (*keep)(const rp_ir_handler_t *, const void *), const void *arg) {
    static const char *const el[] = { "desktop2:DesktopPreviewHandler", "desktop2:ThumbnailHandler", "desktop2:DesktopPropertyHandler" };
    static const rp_handler_kind_t order[] = { RP_HANDLER_PREVIEW, RP_HANDLER_THUMBNAIL };
    for (size_t o = 0; o < sizeof order / sizeof order[0]; ++o) {
        for (size_t k = 0; k < ir->handler_count; ++k) {
            const rp_ir_handler_t *h = &ir->handlers[k];
            if (h->kind != order[o] || !packaged_handler(h) || !keep(h, arg)) continue;
            char guid[37];
            snprintf(guid, sizeof guid, "%.36s", h->clsid ? h->clsid + 1 : "");
            rp_buf_puts(b, "            <");
            rp_buf_puts(b, el[h->kind]);
            attr(b, "Clsid", guid);
            rp_buf_puts(b, " />\r\n");
        }
    }
}

typedef struct {
    const char *const *exts;
    size_t n;
} ext_set_t;

static bool in_exts(const rp_ir_handler_t *h, const void *arg) {
    const ext_set_t *e = arg;
    return handler_in(h, e->exts, e->n, true);
}

static bool same_as(const rp_ir_handler_t *h, const void *arg) {
    return same_types(h, arg);
}

static void handler_elements(rp_buf_t *b, const rp_ir_t *ir, const char *const *exts, size_t n) {
    ext_set_t e = { exts, n };
    handler_list(b, ir, in_exts, &e);
}

static size_t app_of_file(const rp_ir_t *ir, const char *file_id) {
    for (size_t a = 0; file_id && a < ir->msix_app_count; ++a) {
        if (ir->msix_apps[a].exe && strcmp(ir->msix_apps[a].exe, file_id) == 0) return a;
    }
    return SIZE_MAX;
}

static size_t app_by_id(const rp_ir_t *ir, const char *id) {
    if (id == NULL) return 0;
    for (size_t a = 0; a < ir->msix_app_count; ++a) {
        if (strcmp(ir->msix_apps[a].id, id) == 0) return a;
    }
    return SIZE_MAX;
}

// The package path of a [file.*], or NULL when it does not go into the package.
static const char *item_path(const item_t *items, size_t n, const char *file_id) {
    for (size_t i = 0; file_id && i < n; ++i) {
        if (items[i].file_id && strcmp(items[i].file_id, file_id) == 0) return items[i].path;
    }
    return NULL;
}

// Each application's <Extensions> content into ext[a], the package's in ext[app count]; *ns gets the
// namespaces and capabilities used.
static void build_extensions(const rp_ir_t *ir, const item_t *items, size_t n, const app_paths_t *ap, rp_buf_t *ext, unsigned *ns,
                             rp_srcdiags_t *d) {
    unsigned build = min_build(ir);
    char args[4096];
    // A feature an older Windows lacks needs min-version raised (RFC-0001 13.1).
#define NEED(pos, what, b)                                                                                                  \
    do {                                                                                                                    \
        if (build < (b)) DERR(pos, "RP1614", "%s needs Windows build %u or later: set [msix] min-version = \"10.0.%u.0\"", what, \
                              (unsigned)(b), (unsigned)(b));                                                                 \
    } while (0)
    // File types, one FileTypeAssociation per prog-id.
    for (size_t k = 0; k < ir->assoc_count; ++k) {
        const rp_ir_assoc_t *x = &ir->assocs[k];
        bool first = true;
        for (size_t j = 0; j < k; ++j) first &= strcmp(ir->assocs[j].prog_id, x->prog_id) != 0;
        if (!first) continue;
        size_t a = app_of_file(ir, x->target_file);
        if (a == SIZE_MAX) {
            DERR(x->pos, "RP1613", "[assoc.%s]: in an MSIX a file type opens an application: target must be an [msix-app.*] executable", x->id);
            continue;
        }
        if (!literal(x->args, args, sizeof args) || args[0] == ' ' || args[0] == 0) {
            DERR(x->pos, "RP1613", "[assoc.%s]: args for an MSIX must be plain text (no [...] filled in at install)", x->id);
            continue;
        }
        NEED(x->pos, "a file type association", 14393u);
        char name[64];
        size_t o = 0;
        for (const char *q = x->prog_id; *q && o + 1 < sizeof name; ++q) name[o++] = (*q >= 'A' && *q <= 'Z') ? (char)(*q + 32) : *q;
        name[o] = 0;
        rp_buf_t *b = &ext[a];
        rp_buf_puts(b, "        <uap:Extension Category=\"windows.fileTypeAssociation\">\r\n          <uap3:FileTypeAssociation");
        attr(b, "Name", name);
        attr(b, "Parameters", args);
        rp_buf_puts(b, ">\r\n");
        if (x->description) {
            rp_buf_puts(b, "            <uap:DisplayName>");
            xml_text(b, x->description);
            rp_buf_puts(b, "</uap:DisplayName>\r\n");
        }
        rp_buf_puts(b, "            <uap:SupportedFileTypes>\r\n");
        const char *exts[64];
        size_t nexts = 0;
        for (size_t j = k; j < ir->assoc_count; ++j) {
            if (strcmp(ir->assocs[j].prog_id, x->prog_id) != 0) continue;
            rp_buf_puts(b, "              <uap:FileType>");
            xml_text(b, ir->assocs[j].extension);
            rp_buf_puts(b, "</uap:FileType>\r\n");
            if (nexts < 64) exts[nexts++] = ir->assocs[j].extension;
        }
        rp_buf_puts(b, "            </uap:SupportedFileTypes>\r\n");
        size_t before = b->len;
        handler_elements(b, ir, exts, nexts);
        if (b->len != before) *ns |= NS_DESKTOP2;
        rp_buf_puts(b, "          </uap3:FileTypeAssociation>\r\n        </uap:Extension>\r\n");
        *ns |= NS_UAP3;
    }
    // RFC-0024: handlers for types no [assoc] opens get a file type association of their own (no
    // verb) in the first application; one that has some types in an [assoc] must have all there.
    {
        const char *all[256];
        size_t nall = 0;
        for (size_t j = 0; j < ir->assoc_count && nall < 256; ++j) all[nall++] = ir->assocs[j].extension;
        for (size_t k = 0; k < ir->handler_count; ++k) {
            const rp_ir_handler_t *h = &ir->handlers[k];
            if (h->msi_only) continue;
            // A packaged property handler gave Explorer nothing on Windows 11 where the same class
            // installed by an MSI did (DECISIONS 2026-10-02): the MSI only.
            if (h->kind == RP_HANDLER_PROPERTY) {
                DERR(h->pos, "RP1612", "[handler.%s]: an MSIX carries thumbnail and preview handlers only (a packaged property handler "
                     "gave Explorer no values); msi-only = true keeps it for the MSI", h->id);
                continue;
            }
            const rp_ir_com_t *cm = NULL;
            for (size_t j = 0; j < ir->com_count; ++j) {
                if (h->clsid && ir->coms[j].clsid && strcmp(ir->coms[j].clsid, h->clsid) == 0) cm = &ir->coms[j];
            }
            if (cm && cm->msi_only) {
                DERR(h->pos, "RP1613", "[handler.%s]: its class is msi-only ([com.%s]); the MSIX would have no server for it", h->id, cm->id);
                continue;
            }
            // A package serves its classes from a surrogate (dllhost), which calls a class that is not
            // single-threaded on a thread without a message loop: a preview window there stays blank
            // (seen; Windows' prevhost, which the MSI uses, always calls on its own STA).
            if (cm && h->kind == RP_HANDLER_PREVIEW && cm->threading && strcmp(cm->threading, "Apartment") != 0) {
                rp_srcdiag_add(d, h->pos, "RP1612", true, "[handler.%s]: in an MSIX its class runs in a surrogate; threading = \"sta\" in "
                               "[com.%s] gives its window a message loop (with threading = %s the preview may stay blank)", h->id, cm->id,
                               strcmp(cm->threading, "Free") == 0 ? "\"mta\"" : strcmp(cm->threading, "Both") == 0 ? "\"both\"" : "\"neutral\"");
            }
            bool some = handler_in(h, all, nall, false), every = handler_in(h, all, nall, true);
            if (some && !every) {
                DERR(h->pos, "RP1613", "[handler.%s]: in an MSIX its types are all in one [assoc] file type, or none", h->id);
                continue;
            }
            if (some) continue;
            // One association per set of types: a handler with the same types as an earlier one is
            // in that one; types shared only in part would be in two.
            bool done = false, clash = false;
            for (size_t j = 0; j < k; ++j) {
                const rp_ir_handler_t *g = &ir->handlers[j];
                if (!packaged_handler(g) || handler_in(g, all, nall, false)) continue;
                if (same_types(g, h)) done = true;
                else if (handler_in(g, (const char *const *)h->types, h->type_count, false)) clash = true;
            }
            if (clash) {
                DERR(h->pos, "RP1613", "[handler.%s]: in an MSIX handlers share all their types or none (a type is in one file type association)", h->id);
                continue;
            }
            if (done) continue;
            if (ir->msix_app_count == 0) {
                DERR(h->pos, "RP1613", "[handler.%s]: an MSIX file type belongs to an application; add an [msix-app.*]", h->id);
                continue;
            }
            NEED(h->pos, "an Explorer handler", 14393u);
            char name[64];
            snprintf(name, sizeof name, "rp-handler-");
            size_t o = strlen(name);
            for (const char *q = h->id; *q && o + 1 < sizeof name; ++q) name[o++] = (*q >= 'A' && *q <= 'Z') ? (char)(*q + 32) : *q == '_' ? '-' : *q;
            name[o] = 0;
            rp_buf_t *b = &ext[0];
            rp_buf_puts(b, "        <uap:Extension Category=\"windows.fileTypeAssociation\">\r\n          <uap3:FileTypeAssociation");
            attr(b, "Name", name);
            rp_buf_puts(b, ">\r\n            <uap:SupportedFileTypes>\r\n");
            for (size_t t = 0; t < h->type_count; ++t) {
                rp_buf_puts(b, "              <uap:FileType>");
                xml_text(b, h->types[t]);
                rp_buf_puts(b, "</uap:FileType>\r\n");
            }
            rp_buf_puts(b, "            </uap:SupportedFileTypes>\r\n");
            handler_list(b, ir, same_as, h);
            rp_buf_puts(b, "          </uap3:FileTypeAssociation>\r\n        </uap:Extension>\r\n");
            *ns |= NS_UAP3 | NS_DESKTOP2;
        }
    }
    for (size_t k = 0; k < ir->protocol_count; ++k) {
        const rp_ir_protocol_t *x = &ir->protocols[k];
        size_t a = app_of_file(ir, x->target_file);
        if (a == SIZE_MAX) {
            DERR(x->pos, "RP1613", "[protocol.%s]: in an MSIX a scheme opens an application: target must be an [msix-app.*] executable", x->id);
            continue;
        }
        if (!literal(x->args, args, sizeof args) || args[0] == ' ' || args[0] == 0) {
            DERR(x->pos, "RP1613", "[protocol.%s]: args for an MSIX must be plain text (no [...] filled in at install)", x->id);
            continue;
        }
        NEED(x->pos, "a protocol", 14393u);
        rp_buf_puts(&ext[a], "        <uap3:Extension Category=\"windows.protocol\">\r\n          <uap3:Protocol");
        attr(&ext[a], "Name", x->name);
        attr(&ext[a], "Parameters", args);
        rp_buf_puts(&ext[a], " />\r\n        </uap3:Extension>\r\n");
        *ns |= NS_UAP3;
    }
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        size_t a = app_by_id(ir, x->app);
        if (a == SIZE_MAX) continue;                        // the IR said so
        rp_buf_t *b = &ext[a];
        if (x->kind != RP_MSIX_EXT_ALIAS && x->kind != RP_MSIX_EXT_STARTUP) continue;   // below
        if (x->kind == RP_MSIX_EXT_ALIAS) {
            NEED(x->pos, "an execution alias", 14393u);
            rp_buf_puts(b, "        <uap3:Extension Category=\"windows.appExecutionAlias\"");
            attr(b, "Executable", ap[a].start[0] ? ap[a].start : ap[a].exe);
            rp_buf_puts(b, " EntryPoint=\"Windows.FullTrustApplication\">\r\n          <uap3:AppExecutionAlias>\r\n            <desktop:ExecutionAlias");
            attr(b, "Alias", x->alias);
            rp_buf_puts(b, " />\r\n          </uap3:AppExecutionAlias>\r\n        </uap3:Extension>\r\n");
            *ns |= NS_UAP3 | NS_DESKTOP;
        } else {
            NEED(x->pos, "a startup task", 14393u);
            const rp_ir_msix_app_t *app = &ir->msix_apps[a];
            rp_buf_puts(b, "        <desktop:Extension Category=\"windows.startupTask\"");
            attr(b, "Executable", ap[a].start[0] ? ap[a].start : ap[a].exe);
            rp_buf_puts(b, " EntryPoint=\"Windows.FullTrustApplication\">\r\n          <desktop:StartupTask");
            attr(b, "TaskId", x->task_id);
            attr(b, "Enabled", x->enabled ? "true" : "false");
            attr(b, "DisplayName", x->display ? x->display : app->display ? app->display : ir->name);
            rp_buf_puts(b, " />\r\n        </desktop:Extension>\r\n");
            *ns |= NS_DESKTOP;
        }
    }
    // Firewall rules (RFC-0016 2): package level, one FirewallRules per program, in ID order.
    rp_buf_t *pk = &ext[ir->msix_app_count];
    for (size_t k = 0; k < ir->msix_ext_count; ++k) {
        const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
        if (x->kind != RP_MSIX_EXT_FIREWALL) continue;
        size_t a = app_by_id(ir, x->app);
        const char *exe = x->file ? item_path(items, n, x->file) : a != SIZE_MAX ? ap[a].exe : NULL;
        if (exe == NULL) {
            DERR(x->pos, "RP1613", "[msix-extension.%s]: file must name a program that goes into the package", x->id);
            continue;
        }
        bool first = true;
        for (size_t j = 0; j < k; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (y->kind != RP_MSIX_EXT_FIREWALL) continue;
            size_t b = app_by_id(ir, y->app);
            const char *e2 = y->file ? item_path(items, n, y->file) : b != SIZE_MAX ? ap[b].exe : NULL;
            first &= !(e2 && strcmp(e2, exe) == 0);
        }
        if (!first) continue;
        NEED(x->pos, "a firewall rule", 14393u);
        rp_buf_puts(pk, "    <desktop2:Extension Category=\"windows.firewallRules\">\r\n      <desktop2:FirewallRules");
        attr(pk, "Executable", exe);
        rp_buf_puts(pk, ">\r\n");
        for (size_t j = k; j < ir->msix_ext_count; ++j) {
            const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
            if (y->kind != RP_MSIX_EXT_FIREWALL) continue;
            size_t b = app_by_id(ir, y->app);
            const char *e2 = y->file ? item_path(items, n, y->file) : b != SIZE_MAX ? ap[b].exe : NULL;
            if (!(e2 && strcmp(e2, exe) == 0)) continue;
            char port[16];
            rp_buf_puts(pk, "        <desktop2:Rule");
            attr(pk, "Direction", y->outbound ? "out" : "in");
            attr(pk, "IPProtocol", y->udp ? "UDP" : "TCP");
            if (y->port_min) {
                snprintf(port, sizeof port, "%u", y->port_min);
                attr(pk, "LocalPortMin", port);
                snprintf(port, sizeof port, "%u", y->port_max);
                attr(pk, "LocalPortMax", port);
            }
            attr(pk, "Profile", y->profile ? y->profile : "all");
            rp_buf_puts(pk, " />\r\n");
        }
        rp_buf_puts(pk, "      </desktop2:FirewallRules>\r\n    </desktop2:Extension>\r\n");
        *ns |= NS_DESKTOP2;
    }
    // Services (RFC-0016 2): [service.*] becomes desktop6:Service in the first application.
    for (size_t k = 0; k < ir->service_count; ++k) {
        const rp_ir_service_t *x = &ir->services[k];
        const char *exe = item_path(items, n, x->file);
        if (exe == NULL) {
            DERR(x->pos, "RP1613", "[service.%s]: file must name a program that goes into the package", x->id);
            continue;
        }
        if (x->args && (!literal(x->args, args, sizeof args) || args[0] == ' ' || args[0] == 0)) {
            DERR(x->pos, "RP1613", "[service.%s]: args for an MSIX must be plain text (no [...] filled in at install)", x->id);
            continue;
        }
        NEED(x->pos, "a service", 19041u);
        static const char *const accounts[] = { "localSystem", "localService", "networkService" };
        const char *start = x->start == 2 ? "auto" : x->start == 4 ? "disabled" : "manual";
        rp_buf_puts(&ext[0], "        <desktop6:Extension Category=\"windows.service\"");
        attr(&ext[0], "Executable", exe);
        rp_buf_puts(&ext[0], " EntryPoint=\"Windows.FullTrustApplication\">\r\n          <desktop6:Service");
        attr(&ext[0], "Name", x->name);
        attr(&ext[0], "StartupType", start);
        attr(&ext[0], "StartAccount", accounts[x->account]);
        if (x->args) attr(&ext[0], "Arguments", args);
        rp_buf_puts(&ext[0], " />\r\n        </desktop6:Extension>\r\n");
        *ns |= NS_DESKTOP6 | CAP_SERVICES | (x->account == 0 ? CAP_SYSTEM_SERVICES : 0);
    }
    // COM classes, toast activators and context menus (RFC-0016 2), per application: one
    // com:ComServer with an ExeServer per program class and a SurrogateServer per DLL class.
    for (size_t k = 0; k < ir->com_count; ++k) {
        const rp_ir_com_t *x = &ir->coms[k];
        if (!x->msi_only && (x->app_id || x->typelib)) {
            rp_srcdiag_add(d, x->pos, "RP1612", true, "[com.%s]: app-id and typelib are written for the MSI only; the MSIX registers the class without them", x->id);
        }
    }
    for (size_t a = 0; a < ir->msix_app_count; ++a) {
        // The schema wants every ExeServer before the SurrogateServers: two buffers, joined at the end.
        rp_buf_t com = rp_buf_new(ext[a].alloc, 1u << 20), sur = rp_buf_new(ext[a].alloc, 1u << 20), menus = rp_buf_new(ext[a].alloc, 1u << 20),
                 pid = rp_buf_new(ext[a].alloc, 1u << 20);      // com:ProgId, after the servers (RFC-0022)
        for (size_t k = 0; k < ir->msix_ext_count; ++k) {
            const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
            if ((x->kind != RP_MSIX_EXT_COM && x->kind != RP_MSIX_EXT_TOAST && x->kind != RP_MSIX_EXT_CONTEXT_MENU) || app_by_id(ir, x->app) != a) continue;
            const char *path = x->file ? item_path(items, n, x->file) : ap[a].exe;
            if (path == NULL) {
                DERR(x->pos, "RP1613", "[msix-extension.%s]: file must name a file that goes into the package", x->id);
                continue;
            }
            size_t pl = strlen(path);
            bool dll = pl > 4 && (strcmp(path + pl - 4, ".dll") == 0 || strcmp(path + pl - 4, ".DLL") == 0);
            bool exe = pl > 4 && (strcmp(path + pl - 4, ".exe") == 0 || strcmp(path + pl - 4, ".EXE") == 0);
            if ((x->kind == RP_MSIX_EXT_CONTEXT_MENU && !dll) || (x->kind == RP_MSIX_EXT_TOAST && !exe) || (!dll && !exe)) {
                DERR(x->pos, "RP1613", "[msix-extension.%s]: file must be %s", x->id,
                     x->kind == RP_MSIX_EXT_CONTEXT_MENU ? "a DLL (the context menu handler)" : x->kind == RP_MSIX_EXT_TOAST ? "a program (.exe)" : "a program (.exe) or a DLL");
                continue;
            }
            if (x->args && (!literal(x->args, args, sizeof args) || args[0] == ' ' || args[0] == 0)) {
                DERR(x->pos, "RP1613", "[msix-extension.%s]: args must be plain text (no [...] filled in at install)", x->id);
                continue;
            }
            char guid[37];
            snprintf(guid, sizeof guid, "%.36s", x->clsid ? x->clsid + 1 : "");
            const rp_ir_msix_app_t *app = &ir->msix_apps[a];
            const char *display = x->display ? x->display : app->display ? app->display : ir->name;
            NEED(x->pos, x->kind == RP_MSIX_EXT_CONTEXT_MENU ? "a context menu" : "a COM class", x->kind == RP_MSIX_EXT_CONTEXT_MENU ? 17134u : 14393u);
            if (x->prog_id) {
                rp_buf_puts(&pid, "            <com:ProgId");
                attr(&pid, "Id", x->prog_id);
                attr(&pid, "Clsid", guid);
                rp_buf_puts(&pid, " />\r\n");
            }
            if (exe) {
                rp_buf_puts(&com, "            <com:ExeServer");
                attr(&com, "Executable", path);
                if (x->args) attr(&com, "Arguments", args);
                attr(&com, "DisplayName", display);
                rp_buf_puts(&com, ">\r\n              <com:Class");
                attr(&com, "Id", guid);
                attr(&com, "DisplayName", display);
                rp_buf_puts(&com, " />\r\n            </com:ExeServer>\r\n");
            } else {
                rp_buf_puts(&sur, "            <com:SurrogateServer");
                attr(&sur, "DisplayName", display);
                rp_buf_puts(&sur, ">\r\n              <com:Class");
                attr(&sur, "Id", guid);
                attr(&sur, "Path", path);
                attr(&sur, "ThreadingModel", x->threading ? x->threading : "STA");
                rp_buf_puts(&sur, " />\r\n            </com:SurrogateServer>\r\n");
            }
            if (x->kind == RP_MSIX_EXT_TOAST) {
                rp_buf_puts(&ext[a], "        <desktop:Extension Category=\"windows.toastNotificationActivation\">\r\n          <desktop:ToastNotificationActivation");
                attr(&ext[a], "ToastActivatorCLSID", guid);
                rp_buf_puts(&ext[a], " />\r\n        </desktop:Extension>\r\n");
                *ns |= NS_DESKTOP;
            }
        }
        // Context menus, grouped by item type: every verb registered for that type.
        for (size_t k = 0; k < ir->msix_ext_count; ++k) {
            const rp_ir_msix_ext_t *x = &ir->msix_exts[k];
            if (x->kind != RP_MSIX_EXT_CONTEXT_MENU || app_by_id(ir, x->app) != a) continue;
            for (size_t t = 0; t < x->type_count; ++t) {
                bool seen = false;
                for (size_t j = 0; j <= k && !seen; ++j) {
                    const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
                    if (y->kind != RP_MSIX_EXT_CONTEXT_MENU || app_by_id(ir, y->app) != a) continue;
                    for (size_t u = 0; u < (j == k ? t : y->type_count) && !seen; ++u) seen = strcmp(y->types[u], x->types[t]) == 0;
                }
                if (seen) continue;
                rp_buf_puts(&menus, "            <desktop4:ItemType");
                attr(&menus, "Type", x->types[t]);
                rp_buf_puts(&menus, ">\r\n");
                for (size_t j = k; j < ir->msix_ext_count; ++j) {
                    const rp_ir_msix_ext_t *y = &ir->msix_exts[j];
                    if (y->kind != RP_MSIX_EXT_CONTEXT_MENU || app_by_id(ir, y->app) != a) continue;
                    for (size_t u = 0; u < y->type_count; ++u) {
                        if (strcmp(y->types[u], x->types[t]) != 0) continue;
                        char g[37];
                        snprintf(g, sizeof g, "%.36s", y->clsid ? y->clsid + 1 : "");
                        rp_buf_puts(&menus, "              <desktop4:Verb");
                        attr(&menus, "Id", y->verb);
                        attr(&menus, "Clsid", g);
                        rp_buf_puts(&menus, " />\r\n");
                    }
                }
                rp_buf_puts(&menus, "            </desktop4:ItemType>\r\n");
            }
        }
        if (menus.len) {
            rp_buf_puts(&ext[a], "        <desktop4:Extension Category=\"windows.fileExplorerContextMenus\">\r\n          <desktop4:FileExplorerContextMenus>\r\n");
            rp_buf_put(&ext[a], menus.data, menus.len);
            rp_buf_puts(&ext[a], "          </desktop4:FileExplorerContextMenus>\r\n        </desktop4:Extension>\r\n");
            *ns |= NS_DESKTOP4;
        }
        if (com.len || sur.len) {
            rp_buf_puts(&ext[a], "        <com:Extension Category=\"windows.comServer\">\r\n          <com:ComServer>\r\n");
            rp_buf_put(&ext[a], com.data, com.len);
            rp_buf_put(&ext[a], sur.data, sur.len);
            rp_buf_put(&ext[a], pid.data, pid.len);
            rp_buf_puts(&ext[a], "          </com:ComServer>\r\n        </com:Extension>\r\n");
            *ns |= NS_COM;
        }
        if (com.err != PROVEN_OK || sur.err != PROVEN_OK || menus.err != PROVEN_OK || pid.err != PROVEN_OK) ext[a].err = PROVEN_ERR_NOMEM;
        rp_buf_free(&pid);
        rp_buf_free(&com);
        rp_buf_free(&sur);
        rp_buf_free(&menus);
    }
    // Shortcuts: the Start menu entry is the application itself; the desktop gets desktop7:Shortcut.
    for (size_t k = 0; k < ir->shortcut_count; ++k) {
        const rp_ir_shortcut_t *x = &ir->shortcuts[k];
        size_t a = app_of_file(ir, x->target_file);
        bool desktop = strcmp(x->dir, "Desktop") == 0, start = strcmp(x->dir, "Programs") == 0 || strcmp(x->dir, "StartMenu") == 0;
        if (!desktop && !start) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: an MSIX has shortcuts on the desktop and in the Start menu only%s", x->id,
                 strcmp(x->dir, "Startup") == 0 ? " (for Startup use [msix-extension] kind = \"startup-task\")" : "");
        } else if (a == SIZE_MAX) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: in an MSIX a shortcut starts an application: target must be an [msix-app.*] executable", x->id);
        } else if (x->working_dir) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: an MSIX shortcut has no working-dir", x->id);
        } else if (x->when) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: an MSIX shortcut is always there (when is for an MSI)", x->id);
        } else if (x->icon_shown) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: an MSIX shortcut shows its application's logo; icon is for an MSI", x->id);
        } else if (start && x->args) {
            DERR(x->pos, "RP1613", "[shortcut.%s]: the Start menu entry of an MSIX is its application, which takes no args", x->id);
        } else if (desktop) {
            if (x->args && (!literal(x->args, args, sizeof args) || args[0] == ' ' || args[0] == 0)) {
                DERR(x->pos, "RP1613", "[shortcut.%s]: args for an MSIX must be plain text (no [...] filled in at install)", x->id);
                continue;
            }
            NEED(x->pos, "a desktop shortcut", 19645u);
            char file[600];
            snprintf(file, sizeof file, "$(Desktop)\\%s.lnk", x->name);
            rp_buf_puts(&ext[a], "        <desktop7:Extension Category=\"windows.shortcut\">\r\n          <desktop7:Shortcut");
            attr(&ext[a], "File", file);
            attr(&ext[a], "Icon", ap[a].exe);
            if (x->args) attr(&ext[a], "Arguments", args);
            if (x->description) attr(&ext[a], "Description", x->description);
            rp_buf_puts(&ext[a], " />\r\n        </desktop7:Extension>\r\n");
            *ns |= NS_DESKTOP7;
        }
    }
    // Fonts, shared with other applications (uap4, in the first application).
    if (ir->font_count) {
        NEED(ir->fonts[0].pos, "a font", 15063u);
        rp_buf_puts(&ext[0], "        <uap4:Extension Category=\"windows.sharedFonts\">\r\n          <uap4:SharedFonts>\r\n");
        for (size_t k = 0; k < ir->font_count; ++k) {
            for (size_t i = 0; i < n; ++i) {
                if (items[i].file_id && strcmp(items[i].file_id, ir->fonts[k].file) == 0) {
                    rp_buf_puts(&ext[0], "            <uap4:Font");
                    attr(&ext[0], "File", items[i].path);
                    rp_buf_puts(&ext[0], " />\r\n");
                }
            }
        }
        rp_buf_puts(&ext[0], "          </uap4:SharedFonts>\r\n        </uap4:Extension>\r\n");
        *ns |= NS_UAP4;
    }
#undef NEED
}

proven_err_t rp_msix_appinstaller(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, bool bundle,
                                  uint8_t **out, size_t *len) {
    static const char *const arch[] = { "x64", "arm64", "x86" };
    *out = NULL;
    *len = 0;
    if (ir->msix_appinstaller_uri == NULL || ir->msix_package_uri == NULL) return PROVEN_OK;
    char version[32], hours[8], publisher[8400];
    unsigned v[4] = { 0 };
    for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
    snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    snprintf(hours, sizeof hours, "%d", ir->msix_update_hours);
    snprintf(publisher, sizeof publisher, "%s%s%s", ir->msix_publisher, opt->unsigned_test ? ", " : "", opt->unsigned_test ? UNSIGNED_OID : "");
    rp_buf_t b = rp_buf_new(alloc, 1u << 20);
    rp_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<AppInstaller xmlns=\"http://schemas.microsoft.com/appx/appinstaller/2018\"");
    attr(&b, "Version", version);
    attr(&b, "Uri", ir->msix_appinstaller_uri);
    rp_buf_puts(&b, ">\r\n  <");
    rp_buf_puts(&b, bundle ? "MainBundle" : "MainPackage");
    attr(&b, "Name", ir->msix_identity_name);
    attr(&b, "Publisher", publisher);
    attr(&b, "Version", version);
    if (!bundle) attr(&b, "ProcessorArchitecture", arch[ir->arch]);
    attr(&b, "Uri", ir->msix_package_uri);
    rp_buf_puts(&b, " />\r\n  <UpdateSettings>\r\n    <OnLaunch");
    attr(&b, "HoursBetweenUpdateChecks", hours);
    if (ir->msix_update_prompt) attr(&b, "ShowPrompt", "true");
    if (ir->msix_update_blocks) attr(&b, "UpdateBlocksActivation", "true");
    rp_buf_puts(&b, " />\r\n");
    if (ir->msix_update_background) rp_buf_puts(&b, "    <AutomaticBackgroundTask />\r\n");
    rp_buf_puts(&b, "  </UpdateSettings>\r\n</AppInstaller>\r\n");
    if (b.err != PROVEN_OK) {
        rp_buf_free(&b);
        return PROVEN_ERR_NOMEM;
    }
    return rp_buf_take(&b, out, len);
}

// A language's resource package (RFC-0019), as makeappx builds one from a packaging layout: the
// main package's identity with a ResourceId and no architecture, ResourcePackage, no execution,
// the language as its one Resource, and the language's part of resources.pri as resources.pri.
static proven_err_t langpack(proven_allocator_t alloc, const rp_ir_t *ir, const rp_msix_options_t *opt, const char *lang,
                             const char *display, const char *publisher_display, const char *store_logo, uint8_t *pri, size_t pri_len,
                             uint8_t **out, size_t *len) {
    char version[32], publisher[8400], rid[48];
    unsigned v[4] = { 0 };
    for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
    snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    snprintf(publisher, sizeof publisher, "%s%s%s", ir->msix_publisher, opt->unsigned_test ? ", " : "", opt->unsigned_test ? UNSIGNED_OID : "");
    snprintf(rid, sizeof rid, "language-%s", lang);
    rp_buf_t m = rp_buf_new(alloc, 1u << 20);
    rp_buf_puts(&m, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\"\r\n"
                    "         xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\"\r\n"
                    "         xmlns:uap6=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/6\"\r\n"
                    "         IgnorableNamespaces=\"uap uap6\">\r\n  <Identity");
    attr(&m, "Name", ir->msix_identity_name);
    attr(&m, "Publisher", publisher);
    attr(&m, "Version", version);
    attr(&m, "ResourceId", rid);
    rp_buf_puts(&m, " />\r\n  <Properties>\r\n    <ResourcePackage>true</ResourcePackage>\r\n"
                    "    <uap6:AllowExecution>false</uap6:AllowExecution>\r\n    <DisplayName>");
    xml_text(&m, display);
    rp_buf_puts(&m, "</DisplayName>\r\n    <PublisherDisplayName>");
    xml_text(&m, publisher_display);
    rp_buf_puts(&m, "</PublisherDisplayName>\r\n    <Logo>");
    xml_text(&m, store_logo);
    rp_buf_puts(&m, "</Logo>\r\n  </Properties>\r\n  <Resources>\r\n    <Resource");
    attr(&m, "Language", lang);
    rp_buf_puts(&m, " />\r\n  </Resources>\r\n  <Dependencies>\r\n    <TargetDeviceFamily Name=\"Windows.Desktop\"");
    attr(&m, "MinVersion", ir->msix_min_version ? ir->msix_min_version : "10.0.17763.0");
    rp_buf_puts(&m, " MaxVersionTested=\"10.0.26100.0\" />\r\n  </Dependencies>\r\n</Package>\r\n");
    item_t it = { .file_id = NULL, .data = pri, .data_len = pri_len };
    snprintf(it.path, sizeof it.path, "resources.pri");
    rp_zip_writer_t z;
    rp_zip_begin(&z, alloc, (size_t)1 << 40);
    rp_buf_t bm = rp_buf_new(alloc, 1u << 20), ct = rp_buf_new(alloc, 1u << 20);
    rp_buf_puts(&bm, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?><BlockMap "
                     "xmlns=\"http://schemas.microsoft.com/appx/2010/blockmap\" xmlns:b4=\"http://schemas.microsoft.com/appx/2021/blockmap\" "
                     "IgnorableNamespaces=\"b4\" HashMethod=\"http://www.w3.org/2001/04/xmlenc#sha256\">");
    proven_err_t err = m.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, &bm, "resources.pri", NULL, pri, pri_len, !opt->store);
    if (err == PROVEN_OK) err = add_payload(alloc, &z, &bm, "AppxManifest.xml", NULL, m.data, m.len, true);
    rp_buf_puts(&bm, "</BlockMap>");
    if (err == PROVEN_OK) err = bm.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "AppxBlockMap.xml", NULL, bm.data, bm.len, true);
    content_types(&ct, &it, 1);
    if (err == PROVEN_OK) err = ct.err;
    if (err == PROVEN_OK) err = add_payload(alloc, &z, NULL, "[Content_Types].xml", "[Content_Types].xml", ct.data, ct.len, true);
    if (err == PROVEN_OK) err = z.out.err;
    if (err == PROVEN_OK) err = rp_zip_finish(&z, out, len);
    else rp_zip_abort(&z);
    rp_buf_free(&m);
    rp_buf_free(&bm);
    rp_buf_free(&ct);
    return err;
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
        DERR(b->pos, "RP1605", "[%s.%s] cannot go into an MSIX; add msi-only = true to build the MSIX without it", b->kind, b->id);
    }
    if (!ir->has_msix) DERR(top, "RP1604", "a .msix output needs an [msix] table (identity-name, publisher)");
    if (ir->msix_app_count == 0 && ir->msix_main == NULL) DERR(top, "RP1606", "a .msix output needs an [msix-app.ID] table naming the executable");
    // RFC-0019: a modification package changes its main package's files and registry; it has no
    // applications of its own.
    if (ir->msix_modification && ir->msix_app_count) DERR(ir->msix_apps[0].pos, "RP1617", "a modification package has no applications of its own");
    if (ir->msix_main && (ir->msix_cap_count || ir->msix_no_fs_virt || ir->msix_no_reg_virt)) {
        DERR(ir->msix_caps_pos.line ? ir->msix_caps_pos : ir->msix_pos, "RP1617",
             "an optional or modification package declares no capabilities and no virtualization settings: its main package's apply");
    }
    if (ir->msix_modification && min_build(ir) < 18362) {
        DERR(ir->msix_pos, "RP1614", "a modification package needs Windows build 18362 or later: set [msix] min-version = \"10.0.18362.0\"");
    }
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;

    // The first application's executable decides the package root; without applications (an
    // optional or modification package) the dir INSTALLDIR does, and the rest goes to the VFS.
    const rp_ir_dir_t *root = NULL;
    char exe_dir[1024] = "";
    if (ir->msix_app_count) {
        const rp_ir_file_t *exe = NULL;
        const rp_ir_msix_app_t *first = &ir->msix_apps[0];
        for (size_t i = 0; i < ir->file_count && first->exe; ++i) {
            if (strcmp(ir->files[i].id, first->exe) == 0) exe = &ir->files[i];
        }
        if (exe == NULL || exe->msi_only) {
            DERR(first->pos, "RP1607", "executable = \"%s\" must name a [file.*] that goes into the package", first->exe ? first->exe : "");
        } else if (!below_root(ir, exe->dir, &root, exe_dir, sizeof exe_dir)) {
            DERR(first->pos, "RP1609", "the executable's folder does not lead to a known location");
        }
    } else if (find_dir(ir, "INSTALLDIR") && !ir->msix_modification) {
        (void)below_root(ir, "INSTALLDIR", &root, exe_dir, sizeof exe_dir);
    }
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;

    // Merge modules (RFC-0019): their files and registry values.
    rp_msix_module_t *mods = ir->merge_count ? rp_mem_alloc(alloc, ir->merge_count, sizeof *mods) : NULL;
    size_t mod_files = 0;
    if (ir->merge_count && mods == NULL) return PROVEN_ERR_NOMEM;
    if (mods) memset(mods, 0, ir->merge_count * sizeof *mods);
    for (size_t k = 0; k < ir->merge_count; ++k) {
        const rp_ir_merge_t *x = &ir->merges[k];
        if (x->msi_only) continue;
        const char *why = NULL;
        if (rp_msix_module_read(alloc, x->source, (const char *const *)x->config, x->config_count, &mods[k], &why) != PROVEN_OK) {
            DERR(x->pos, "RP1517", "[merge.%s]: '%s' %s; or msi-only = true", x->id, x->shown ? x->shown : "", why ? why : "cannot be read");
            continue;
        }
        for (size_t i = 0; i < mods[k].reg_count; ++i) {
            mods[k].regs[i].id = x->id;
            mods[k].regs[i].pos = x->pos;
        }
        mod_files += mods[k].file_count;
    }

    // The payload.
    size_t cap = mod_files + ir->file_count + ir->copy_count + ir->ini_count + 19 * ir->msix_app_count + 7, n = 0;     // + copies, INI files, logo scales, launchers, resources.pri
    item_t *items = rp_mem_alloc(alloc, cap, sizeof *items);
    if (items == NULL) return PROVEN_ERR_NOMEM;
    memset(items, 0, cap * sizeof *items);
    for (size_t i = 0; i < ir->file_count; ++i) {
        const rp_ir_file_t *f = &ir->files[i];
        if (f->msi_only) continue;
        if (f->keep) DERR(f->pos, "RP1612", "'%s': an MSIX removes all its files (keep); use msi-only = true", f->name);
        if (f->when) DERR(f->pos, "RP1612", "'%s': an MSIX installs all its files (when); use msi-only = true", f->name);
        item_t *it = &items[n];
        const char *why = NULL;
        int placed = place(ir, root, f->dir, f->name, is_font(ir, f->id), it->path, sizeof it->path, &why);
        if (placed < 0) {
            DERR(f->pos, "RP1609", "'%s': its folder does not lead to a known location", f->name);
            continue;
        }
        if (placed == 0) {
            DERR(f->pos, "RP1609", "'%s' goes to %s; or msi-only = true", f->name, why);
            continue;
        }
        if (placed == 2 && reserved(it->path)) DERR(f->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
        ++n;
        it->file_id = f->id;
        it->source = f->source_path;
        it->pos = f->pos;
    }
    // [copy.*] (RFC-0018): the source file once more, at the copy's place.
    for (size_t i = 0; i < ir->copy_count; ++i) {
        const rp_ir_copy_t *cp = &ir->copies[i];
        const rp_ir_file_t *f = NULL;
        for (size_t j = 0; j < ir->file_count && !f; ++j) {
            if (strcmp(ir->files[j].id, cp->source_file) == 0) f = &ir->files[j];
        }
        if (cp->msi_only) continue;
        if (f == NULL || f->msi_only) {
            DERR(cp->pos, "RP1613", "[copy.%s]: its source must be a file that goes into the package", cp->id);
            continue;
        }
        item_t *it = &items[n];
        const char *why = NULL, *name = cp->name ? cp->name : f->name;
        int placed = place(ir, root, cp->dir, name, false, it->path, sizeof it->path, &why);
        if (placed <= 0) {
            DERR(cp->pos, "RP1609", "[copy.%s] goes to %s; or msi-only = true", cp->id, placed < 0 ? "a folder that does not lead to a known location" : why);
            continue;
        }
        if (placed == 2 && reserved(it->path)) DERR(cp->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
        ++n;
        it->source = f->source_path;
        it->pos = cp->pos;
    }
    for (size_t k = 0; k < ir->merge_count; ++k) {
        const rp_ir_merge_t *x = &ir->merges[k];
        for (size_t i = 0; i < mods[k].file_count; ++i) {
            const rp_msix_module_file_t *mf = &mods[k].files[i];
            item_t *it = &items[n];
            if (mf->vfs) {
                snprintf(it->path, sizeof it->path, "VFS\\%s\\%s", mf->vfs, mf->rel);
            } else {
                const char *why = NULL;
                int placed = place(ir, root, x->dir, mf->rel, false, it->path, sizeof it->path, &why);
                if (placed <= 0) {
                    DERR(x->pos, "RP1609", "[merge.%s] goes to %s; or msi-only = true", x->id,
                         placed < 0 ? "a folder that does not lead to a known location" : why);
                    break;
                }
                if (placed == 2 && reserved(it->path)) DERR(x->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
            }
            it->data = rp_mem_alloc(alloc, mf->len ? mf->len : 1, 1);
            if (it->data == NULL) {
                DERR(x->pos, "RP1612", "out of memory");
                break;
            }
            memcpy(it->data, mf->data, mf->len);
            it->data_len = mf->len;
            it->pos = x->pos;
            ++n;
        }
    }
    // [ini.*] (RFC-0019): each INI file once, made now.
    for (size_t i = 0; i < ir->ini_count; ++i) {
        const rp_ir_ini_t *x = &ir->inis[i];
        if (x->msi_only) continue;
        bool first = true;
        for (size_t j = 0; j < i && first; ++j) {
            first = ir->inis[j].msi_only || strcmp(ir->inis[j].dir, x->dir) != 0 || strcmp(ir->inis[j].file, x->file) != 0;
        }
        if (!first) continue;
        item_t *it = &items[n];
        const char *why = NULL;
        int placed = place(ir, root, x->dir, x->file, false, it->path, sizeof it->path, &why);
        if (placed <= 0) {
            DERR(x->pos, "RP1609", "[ini.%s] goes to %s; or msi-only = true", x->id, placed < 0 ? "a folder that does not lead to a known location" : why);
            continue;
        }
        if (placed == 2 && reserved(it->path)) DERR(x->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
        if (ini_file(alloc, ir, x->dir, x->file, &it->data, &it->data_len, d) != PROVEN_OK) {
            DERR(x->pos, "RP1612", "out of memory");
            continue;
        }
        it->pos = x->pos;
        ++n;
    }
    // Each application: its executable's package path, and its logos - the three given (checked),
    // or plain ones made here once for all applications.
    app_paths_t *ap = rp_mem_alloc(alloc, ir->msix_app_count ? ir->msix_app_count : 1, sizeof *ap);
    if (ap == NULL) {
        rp_mem_free(alloc, items);
        return PROVEN_ERR_NOMEM;
    }
    memset(ap, 0, (ir->msix_app_count ? ir->msix_app_count : 1) * sizeof *ap);
    static const uint32_t logo_px[3] = { 150, 44, 50 };
    static const char *const logo_default[3] = { "Assets\\DefaultSquare150x150Logo.png", "Assets\\DefaultSquare44x44Logo.png",
                                                 "Assets\\DefaultStoreLogo.png" };
    bool defaults_made = false;
    loc_t *lp = rp_mem_alloc(alloc, 1, sizeof *lp);
    if (lp == NULL) {
        rp_mem_free(alloc, items);
        rp_mem_free(alloc, ap);
        return PROVEN_ERR_NOMEM;
    }
    memset(lp, 0, sizeof *lp);
    loc_t *const locp = lp;
#define loc (*locp)
    loc.alloc = alloc;
    loc.default_lang = ir->language == 1042 ? "ko-KR" : "en-US";
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
            // [env.*] (RFC-0019): the application starts through rubrapack's launcher (added below).
            bool env = false;
            for (size_t i = 0; i < ir->env_count && !env; ++i) env = !ir->envs[i].msi_only;
            if (env) snprintf(ap[a].start, sizeof ap[a].start, "rubrapack\\%s.exe", app->id);
        }
        for (int k = 0; k < 3; ++k) {
            // RFC-0016 3: Logo.scale-NNN.png next to the logo - the logo in several scales, found
            // through resources.pri; Logo.png itself, when there, is the scale-100 one.
            static const char *const scale_of[] = { "100", "125", "150", "200", "400" };
            static const uint32_t scale_n[] = { 100, 125, 150, 200, 400 };
            char vpath[5][2048];
            bool have[5] = { false }, any = false;
            const char *dot = app->logo_path[k] ? strrchr(app->logo_path[k], '.') : NULL, *sl = app->logo_path[k] ? strrchr(app->logo_path[k], '/') : NULL;
            for (int v = 0; dot && (!sl || dot > sl) && v < 5; ++v) {
                uint64_t size;
                snprintf(vpath[v], sizeof vpath[v], "%.*s.scale-%s%s", (int)(dot - app->logo_path[k]), app->logo_path[k], scale_of[v], dot);
                have[v] = rp_pal_stat(alloc, vpath[v], &size) == RP_FS_FILE;
                any |= have[v];
            }
            if (any) {
                uint64_t size;
                bool base = rp_pal_stat(alloc, app->logo_path[k], &size) == RP_FS_FILE;
                if (base && have[0]) {
                    DERR(app->pos, "RP1608", "logo '%s' and its .scale-100 variant are both there: keep one", app->logo[k]);
                } else if (base) {
                    have[0] = true;
                    snprintf(vpath[0], sizeof vpath[0], "%s", app->logo_path[k]);
                }
                const char *b = strrchr(app->logo[k], '/');
                b = b ? b + 1 : app->logo[k];
                const char *bdot = strrchr(b, '.');
                bool ascii_name = true;
                for (const char *q = b; *q; ++q) ascii_name &= (unsigned char)*q < 0x80 && *q != '\\';
                if (!ascii_name || bdot == NULL) {
                    DERR(app->pos, "RP1608", "logo '%s': a logo with scale variants needs an ASCII file name with an extension", app->logo[k]);
                    continue;
                }
                snprintf(ap[a].logo[k], sizeof ap[a].logo[k], "Assets\\%s", b);
                const char *res = loc_dup(&loc, "Files/Assets/%s%s", b, "");
                for (int v = 0; v < 5; ++v) {
                    if (!have[v]) continue;
                    uint8_t *png = NULL;
                    size_t pl = 0;
                    uint32_t w = 0, h = 0, want = (logo_px[k] * scale_n[v] + 50) / 100;
                    if (rp_pal_read_file(alloc, vpath[v], 16u << 20, &png, &pl) != PROVEN_OK || !png_size(png, pl, &w, &h)) {
                        DERR(app->pos, "RP1608", "logo '%s' at scale %s is not a PNG file", app->logo[k], scale_of[v]);
                    } else if (w != want || h != want) {
                        DERR(app->pos, "RP1608", "logo '%s' at scale %s is %ux%u pixels; it must be %ux%u", app->logo[k], scale_of[v],
                             (unsigned)w, (unsigned)h, (unsigned)want, (unsigned)want);
                    }
                    rp_mem_free(alloc, png);
                    char pbuf[2300];
                    snprintf(pbuf, sizeof pbuf, "Assets\\%.*s.scale-%s%s", (int)(bdot - b), b, scale_of[v], bdot);
                    const char *pp = loc_dup(&loc, "%s%s", pbuf, "");
                    bool dup = false;
                    for (size_t i = 0; i < n; ++i) dup |= strcmp(items[i].path, pp) == 0;
                    if (!dup) {
                        item_t *it = &items[n++];
                        snprintf(it->path, sizeof it->path, "%s", pp);
                        it->source = loc_dup(&loc, "%s%s", vpath[v], "");
                        it->pos = app->pos;
                    }
                    loc_add(&loc, res, RP_PRI_SCALE, scale_of[v], true, pp);
                }
                continue;
            }
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
    const char *store_logo = ir->msix_app_count ? ap[0].logo[2] : logo_default[2];
    if (ir->msix_app_count == 0) {          // the package's logo (an optional or modification package)
        item_t *it = &items[n++];
        snprintf(it->path, sizeof it->path, "%s", logo_default[2]);
        it->pos = top;
        if (default_logo(alloc, logo_px[2], &it->data, &it->data_len) != PROVEN_OK) DERR(top, "RP1608", "cannot make a default logo");
    }
    // Texts in several languages (RFC-0016 3), then resources.pri when anything needs it.
    loc.pkg_display = loc_text(&loc, "PackageDisplayName", ir->msix_display ? ir->msix_display : ir->name, ir->msix_display_by_lang,
                               ir->msix_display_by_lang_count, ir->msix_pos, d);
    loc.pub_display = loc_text(&loc, "PublisherDisplayName", ir->msix_publisher_display ? ir->msix_publisher_display : ir->manufacturer,
                               ir->msix_publisher_display_by_lang, ir->msix_publisher_display_by_lang_count, ir->msix_pos, d);
    for (size_t a = 0; a < ir->msix_app_count; ++a) {
        const rp_ir_msix_app_t *app = &ir->msix_apps[a];
        const char *display = app->display ? app->display : ir->name;
        char name[128];
        snprintf(name, sizeof name, "%sDisplayName", app->id);
        ap[a].loc_display = loc_text(&loc, name, display, app->display_by_lang, app->display_by_lang_count, app->pos, d);
        snprintf(name, sizeof name, "%sDescription", app->id);
        ap[a].loc_description = loc_text(&loc, name, app->description ? app->description : display, app->description_by_lang,
                                         app->description_by_lang_count, app->pos, d);
    }
    if (loc.full) DERR(top, "RP1606", "too many localized texts and logo scales for resources.pri");
    // RFC-0019: in a bundle, other languages' texts go into resource packages.
    bool split = opt->langpacks && !ir->msix_no_langpacks && loc.nlangs > 0;
    if (loc.n && d->errors == errors) {
        item_t *it = &items[n++];
        snprintf(it->path, sizeof it->path, "resources.pri");
        it->pos = top;
        if (rp_pri_write_part(alloc, ir->msix_identity_name, loc.default_lang, loc.c, loc.n, split ? "" : NULL, &it->data, &it->data_len) !=
            PROVEN_OK) {
            DERR(top, "RP1606", "cannot write resources.pri");
        }
    }
    if (split && d->errors == errors) {
        rp_msix_langpack_t *lp = rp_mem_alloc(alloc, loc.nlangs, sizeof *lp);
        if (lp == NULL) DERR(top, "RP1606", "out of memory");
        size_t nlp = 0;
        for (size_t k = 0; lp && k < loc.nlangs; ++k) {
            uint8_t *pri = NULL;
            size_t pl = 0;
            rp_msix_langpack_t *p = &lp[nlp];
            snprintf(p->language, sizeof p->language, "%s", loc.langs[k]);
            const char *disp = loc.pkg_display ? "ms-resource:PackageDisplayName" : ir->msix_display ? ir->msix_display : ir->name;
            const char *pub = loc.pub_display ? "ms-resource:PublisherDisplayName" : ir->msix_publisher_display ? ir->msix_publisher_display : ir->manufacturer;
            if (rp_pri_write_part(alloc, ir->msix_identity_name, loc.default_lang, loc.c, loc.n, loc.langs[k], &pri, &pl) != PROVEN_OK ||
                langpack(alloc, ir, opt, loc.langs[k], disp, pub, store_logo, pri, pl, &p->data, &p->len) != PROVEN_OK) {
                DERR(top, "RP1606", "cannot write the resource package of %s", loc.langs[k]);
            } else {
                ++nlp;
            }
            rp_mem_free(alloc, pri);
        }
        *opt->langpacks = lp;
        *opt->nlangpacks = nlp;
    }
    // [registry] values: Registry.dat and User.dat.
    rp_regf_t *machine = rp_regf_new(alloc), *user = rp_regf_new(alloc);
    bool used_m = false, used_u = false;
    if (machine == NULL || user == NULL) DERR(top, "RP1612", "out of memory");
    for (size_t i = 0; machine && user && i < ir->registry_count; ++i) {
        if (!ir->registries[i].msi_only) (void)add_registry(alloc, &ir->registries[i], machine, user, &used_m, &used_u, d);
    }
    for (size_t k = 0; k < ir->merge_count; ++k) {
        for (size_t i = 0; i < mods[k].reg_count; ++i) (void)add_registry(alloc, &mods[k].regs[i], machine, user, &used_m, &used_u, d);
    }
    for (size_t k = 0; k < ir->merge_count; ++k) {
        for (size_t i = 0; i < mods[k].reg_count; ++i) mods[k].regs[i].id = NULL;     // not owned
        rp_msix_module_free(alloc, &mods[k]);
    }
    rp_mem_free(alloc, mods);
    mods = NULL;
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
    // [env.*] (RFC-0019): every application starts through rubrapack's launcher, which gives its
    // processes the variables (an MSIX cannot change the machine's environment).
    size_t nenv = 0;
    for (size_t i = 0; i < ir->env_count; ++i) nenv += !ir->envs[i].msi_only;
    if (nenv && d->errors == errors) {
        rp_buf_t lt = rp_buf_new(alloc, 1u << 20);
        for (size_t a = 0; a < ir->msix_app_count; ++a) {
            const rp_ir_msix_app_t *app = &ir->msix_apps[a];
            rp_buf_puts(&lt, "app ");
            rp_buf_puts(&lt, app->id);
            rp_buf_puts(&lt, "\r\ntarget ");
            rp_buf_puts(&lt, ap[a].exe);
            rp_buf_puts(&lt, "\r\n");
            for (size_t i = 0; i < ir->env_count; ++i) {
                const rp_ir_env_t *e = &ir->envs[i];
                if (e->msi_only) continue;
                if (a == 0 && e->when) DERR(e->pos, "RP1612", "[env.%s]: an MSIX sets all its variables (when); use msi-only = true", e->id);
                rp_buf_puts(&lt, e->mode == 0 ? "set " : e->mode == 1 ? "append " : "prepend ");
                rp_buf_puts(&lt, e->name);
                rp_buf_byte(&lt, '=');
                if (!env_value(ir, root, e->value ? e->value : "", &lt) && a == 0) {
                    DERR(e->pos, "RP1612", "[env.%s]: its value names a place an MSIX cannot give (a folder outside the package that "
                         "is not a Windows folder, or an install-time part); use msi-only = true", e->id);
                }
                rp_buf_puts(&lt, "\r\n");
            }
            // The launcher of the program's own kind: a console program shares its console.
            const item_t *x = NULL;
            for (size_t i = 0; i < n && !x; ++i) {
                if (items[i].file_id && strcmp(items[i].path, ap[a].exe) == 0) x = &items[i];
            }
            uint8_t *exe = NULL;
            size_t exe_len = 0;
            rp_pe_info_t pi = { 0 };
            if (x && x->source && rp_pal_read_file(alloc, x->source, 1u << 30, &exe, &exe_len) == PROVEN_OK) (void)rp_pe_read(exe, exe_len, &pi);
            rp_mem_free(alloc, exe);
            bool console = pi.subsystem == 3;
            const unsigned char *bin = ir->arch == RP_ARCH_X64 ? (console ? rp_launchc_x64 : rp_launch_x64)
                                     : ir->arch == RP_ARCH_X86 ? (console ? rp_launchc_x86 : rp_launch_x86)
                                                               : (console ? rp_launchc_arm64 : rp_launch_arm64);
            size_t bin_len = ir->arch == RP_ARCH_X64 ? (console ? rp_launchc_x64_len : rp_launch_x64_len)
                           : ir->arch == RP_ARCH_X86 ? (console ? rp_launchc_x86_len : rp_launch_x86_len)
                                                     : (console ? rp_launchc_arm64_len : rp_launch_arm64_len);
            item_t *it = &items[n++];
            snprintf(it->path, sizeof it->path, "%s", ap[a].start);
            it->pos = app->pos;
            it->data = rp_mem_alloc(alloc, bin_len ? bin_len : 1, 1);
            if (it->data == NULL) {
                DERR(top, "RP1612", "out of memory");
                break;
            }
            memcpy(it->data, bin, bin_len);
            it->data_len = bin_len;
        }
        item_t *it = &items[n++];
        snprintf(it->path, sizeof it->path, "rubrapack\\launch.txt");
        it->pos = top;
        if (rp_buf_take(&lt, &it->data, &it->data_len) != PROVEN_OK) DERR(top, "RP1612", "out of memory");
    }
    // Two paths that Windows would take for one (letters compared without case).
    for (size_t i = 0; i < n; ++i) {
        for (size_t k = i + 1; k < n; ++k) {
            if (ieq(items[i].path, items[k].path)) DERR(items[k].pos, "RP1611", "'%s' and '%s' are the same path in the package", items[i].path, items[k].path);
        }
    }
    rp_buf_t *ext = rp_mem_alloc(alloc, ir->msix_app_count + 1, sizeof *ext);    // + package level
    unsigned ns = 0;
    if (ext == NULL) {
        for (size_t i = 0; i < n; ++i) rp_mem_free(alloc, items[i].data);
        rp_mem_free(alloc, items);
        rp_mem_free(alloc, ap);
        loc_free(&loc);
        rp_mem_free(alloc, locp);
        return PROVEN_ERR_NOMEM;
    }
    for (size_t a = 0; a <= ir->msix_app_count; ++a) ext[a] = rp_buf_new(alloc, 1u << 20);
    if (d->errors == errors) build_extensions(ir, items, n, ap, ext, &ns, d);
    if (ir->msix_main) ns |= ir->msix_main_publisher || ir->msix_modification ? NS_UAP4 : NS_UAP3;     // RFC-0019
    if (ir->msix_modification) ns |= NS_RESCAP6;
    // RFC-0018: the capabilities the source names, and write virtualization turned off.
    for (size_t k = 0; k < ir->msix_cap_count; ++k) {
        const char *c = ir->msix_caps[k];
        int kind = capability_kind(c);
        if (strcmp(c, "runFullTrust") == 0) continue;           // every package has it
        if (kind < 0) {
            DERR(ir->msix_caps_pos, "RP1616", "capability '%s' is not one rubrapack knows (the manual lists them)", c);
            continue;
        }
        if (kind == C_UAP3) ns |= NS_UAP3;
        if (kind == C_UAP6) ns |= NS_UAP6;
        if (kind == C_UAP7) ns |= NS_UAP7;
        if (kind == C_UAP6 && min_build(ir) < 17763) DERR(ir->msix_caps_pos, "RP1614", "capability '%s' needs Windows build 17763 or later", c);
        if (kind == C_UAP7 && min_build(ir) < 17763) DERR(ir->msix_caps_pos, "RP1614", "capability '%s' needs Windows build 17763 or later", c);
    }
    if (ir->msix_no_fs_virt || ir->msix_no_reg_virt) {
        ns |= NS_DESKTOP6 | CAP_UNVIRTUALIZED;
        if (min_build(ir) < 18362) {
            DERR(ir->msix_pos, "RP1614", "file-system-virtualization and registry-virtualization = false need Windows build 18362 or "
                 "later: set [msix] min-version = \"10.0.18362.0\"");
        }
    }
    for (size_t a = 0; a <= ir->msix_app_count; ++a) {
        if (ext[a].err != PROVEN_OK) DERR(top, "RP1613", "out of memory");
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
        manifest(&man, ir, opt, ap, ext, ns, &loc, store_logo);
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
    loc_free(&loc);
    rp_mem_free(alloc, locp);
#undef loc
    for (size_t i = 0; i < n; ++i) rp_mem_free(alloc, items[i].data);
    rp_mem_free(alloc, items);
    rp_mem_free(alloc, ap);
    for (size_t a = 0; a <= ir->msix_app_count; ++a) rp_buf_free(&ext[a]);
    rp_mem_free(alloc, ext);
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

// Decodes padded base64 of exactly `n` bytes (at most 64: a digest).
static bool unbase64(const char *s, uint8_t *out, size_t n) {
    size_t sl = strlen(s);
    uint8_t t[66];
    proven_size_t w = 0;
    if (n > 64 || sl != (n + 2) / 3 * 4) return false;
    if (proven_base64_decode((proven_mem_view_t){ (const proven_byte_t *)s, sl }, t, sizeof t, &w) != PROVEN_OK || w != n) return false;
    memcpy(out, t, n);
    return true;
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
        // RFC-0019: a resource package (a language's resources) has a ResourceId instead of an architecture.
        bool resource = strcmp(type, "resource") == 0;
        if (!resource && strcmp(type, "application") != 0) {
            *why = "a Package of the bundle manifest of an unknown Type";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        char rid[64] = "";
        if (!attr_value(from, te, "Version", ver, sizeof ver) ||
            (resource ? !attr_value(from, te, "ResourceId", rid, sizeof rid) : !attr_value(from, te, "Architecture", arch, sizeof arch))) {
            *why = resource ? "a resource Package of the bundle manifest without Version or ResourceId"
                            : "a Package of the bundle manifest without Version or Architecture";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (resource) snprintf(arch, sizeof arch, "~%.14s", rid);   // kept apart from the architectures below
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
                *why = resource ? "two resource packages of one ResourceId in the bundle" : "two packages of one architecture in the bundle";
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
        if (err == PROVEN_OK && resource) {
            char prid[64];
            bad_id = !read_identity((const char *)im, iml, &pid) || !rp_xml_attr((const char *)im, iml, "Identity", "ResourceId", prid, sizeof prid) ||
                     strcmp(pid.name, bid.name) != 0 || strcmp(pid.publisher, bid.publisher) != 0 || strcmp(pid.version, ver) != 0 ||
                     strcmp(prid, rid) != 0;
        } else if (err == PROVEN_OK) {
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
    char zip_names[MAX_PARTS][1024], rids[MAX_PARTS][64];
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
        // RFC-0019: a resource package names a ResourceId instead of an architecture.
        rids[i][0] = '\0';
        bool resource = rp_xml_attr(m, mlens[i], "Identity", "ResourceId", rids[i], sizeof rids[i]);
        if (!read_identity(m, mlens[i], &ids[i]) ||
            (!resource && !rp_xml_attr(m, mlens[i], "Identity", "ProcessorArchitecture", ids[i].arch, sizeof ids[i].arch))) {
            *why = "a package without Identity Name, Publisher, Version and ProcessorArchitecture (or ResourceId)";
        } else if (i && (strcmp(ids[i].name, ids[0].name) != 0 || strcmp(ids[i].publisher, ids[0].publisher) != 0 || strcmp(ids[i].version, ids[0].version) != 0)) {
            *why = "the packages of a bundle need the same Name, Publisher and Version";
        } else if (strlen(parts[i].file_name) > 255 || !ext || (!ieq(ext, "msix") && !ieq(ext, "appx")) || strpbrk(parts[i].file_name, "/\\")) {
            *why = "a package's name in the bundle must be a file name ending in .msix or .appx";
        } else {
            part_name(parts[i].file_name, zip_names[i], sizeof zip_names[i]);
            for (size_t k = 0; k < i && !*why; ++k) {
                if (!resource && !rids[k][0] && strcmp(ids[k].arch, ids[i].arch) == 0) *why = "two packages of one architecture in a bundle";
                else if (resource && strcmp(rids[k], rids[i]) == 0) *why = "two resource packages of one ResourceId in a bundle";
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
            rp_zip_add(&z, zip_names[i], 0, parts[i].data, parts[i].len, proven_crc32((proven_mem_view_t){ (const proven_byte_t *)(parts[i].data), parts[i].len }), parts[i].len, &lfh);
            rp_buf_puts(&man, rids[i][0] ? "\t\t<Package Type=\"resource\"" : "\t\t<Package Type=\"application\"");
            attr(&man, "Version", ids[i].version);
            if (rids[i][0]) attr(&man, "ResourceId", rids[i]);
            else attr(&man, "Architecture", ids[i].arch);
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
