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
#include "rubrapack/zip.h"

#include <stdio.h>
#include <string.h>

enum { BLOCK = 65536, LEVEL = 6 };

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
    char        path[2200];     // package path, '\' separated
    const char *source;         // file to read, or NULL for `data`
    uint8_t    *data;           // generated content (logos), owned
    size_t      data_len;
    rp_pos_t    pos;
} item_t;

#define DERR(pos, code, ...) rp_srcdiag_add(d, (pos), (code), false, __VA_ARGS__)

static bool reserved(const char *path) {
    static const char *const names[] = { "AppxManifest.xml", "AppxBlockMap.xml", "[Content_Types].xml", "AppxSignature.p7x",
                                         "CodeIntegrity.cat", "resources.pri", NULL };
    for (size_t i = 0; names[i]; ++i) {
        if (ieq(path, names[i])) return true;
    }
    return strncmp(path, "AppxMetadata\\", 13) == 0 || ieq(path, "AppxMetadata");
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

static void manifest(rp_buf_t *m, const rp_ir_t *ir, const rp_msix_options_t *opt, const char *exe, char logos[3][2200]) {
    static const char *const arch[] = { "x64", "arm64", "x86" };
    char version[32], publisher[8400];
    unsigned v[4] = { 0 };
    for (size_t i = 0; i < ir->version_count && i < 4; ++i) v[i] = ir->version_parts[i];
    snprintf(version, sizeof version, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    snprintf(publisher, sizeof publisher, "%s%s%s", ir->msix_publisher, opt->unsigned_test ? ", " : "", opt->unsigned_test ? UNSIGNED_OID : "");
    const char *display = ir->msix_app_display ? ir->msix_app_display : ir->name;
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
    xml_text(m, logos[2]);
    rp_buf_puts(m, "</Logo>\r\n  </Properties>\r\n  <Dependencies>\r\n    <TargetDeviceFamily Name=\"Windows.Desktop\"");
    attr(m, "MinVersion", ir->msix_min_version ? ir->msix_min_version : "10.0.17763.0");
    rp_buf_puts(m, " MaxVersionTested=\"10.0.26100.0\" />\r\n  </Dependencies>\r\n  <Resources>\r\n    <Resource");
    attr(m, "Language", ir->language == 1042 ? "ko-KR" : "en-US");
    rp_buf_puts(m, " />\r\n  </Resources>\r\n  <Applications>\r\n    <Application");
    attr(m, "Id", ir->msix_app_id);
    attr(m, "Executable", exe);
    rp_buf_puts(m, " EntryPoint=\"Windows.FullTrustApplication\">\r\n      <uap:VisualElements");
    attr(m, "DisplayName", display);
    attr(m, "Description", ir->msix_app_description ? ir->msix_app_description : display);
    rp_buf_puts(m, " BackgroundColor=\"transparent\"");
    attr(m, "Square150x150Logo", logos[0]);
    attr(m, "Square44x44Logo", logos[1]);
    rp_buf_puts(m, " />\r\n    </Application>\r\n  </Applications>\r\n  <Capabilities>\r\n"
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
        bool later = strcmp(b->kind, "registry") == 0 || strcmp(b->kind, "shortcut") == 0 || strcmp(b->kind, "font") == 0;
        DERR(b->pos, "RP1605", "[%s.%s] cannot go into an MSIX%s; add msi-only = true to build the MSIX without it", b->kind, b->id,
             later ? " yet (planned for P8b)" : "");
    }
    if (!ir->has_msix) DERR(top, "RP1604", "a .msix output needs an [msix] table (identity-name, publisher)");
    if (ir->msix_app_count == 0) DERR(top, "RP1606", "a .msix output needs an [msix-app.ID] table naming the executable");
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;

    // The executable decides the package root.
    const rp_ir_file_t *exe = NULL;
    for (size_t i = 0; i < ir->file_count && ir->msix_app_exe; ++i) {
        if (strcmp(ir->files[i].id, ir->msix_app_exe) == 0) exe = &ir->files[i];
    }
    const rp_ir_dir_t *root = NULL;
    char exe_path[1024] = "";
    if (exe == NULL || exe->msi_only) {
        DERR(ir->msix_app_pos, "RP1607", "executable = \"%s\" must name a [file.*] that goes into the package", ir->msix_app_exe ? ir->msix_app_exe : "");
    } else if (!ieq(extension(exe->name) ? extension(exe->name) : "", "exe")) {
        DERR(ir->msix_app_pos, "RP1607", "the executable '%s' must be an .exe", exe->name);
    } else if (!below_root(ir, exe->dir, &root, exe_path, sizeof exe_path)) {
        DERR(ir->msix_app_pos, "RP1609", "the executable's folder does not lead to a known location");
    }
    if (d->errors != errors) return PROVEN_ERR_INVALID_FORMAT;
    if (exe_path[0]) strncat(exe_path, "\\", sizeof exe_path - strlen(exe_path) - 1);
    strncat(exe_path, exe->name, sizeof exe_path - strlen(exe_path) - 1);

    // The payload.
    size_t cap = ir->file_count + 3, n = 0;
    item_t *items = rp_mem_alloc(alloc, cap, sizeof *items);
    if (items == NULL) return PROVEN_ERR_NOMEM;
    memset(items, 0, cap * sizeof *items);
    for (size_t i = 0; i < ir->file_count; ++i) {
        const rp_ir_file_t *f = &ir->files[i];
        if (f->msi_only) continue;
        const rp_ir_dir_t *r = NULL;
        char dir[1024];
        if (!below_root(ir, f->dir, &r, dir, sizeof dir) || r != root) {
            DERR(f->pos, "RP1609", "'%s' is outside the application's folder; files elsewhere need VFS (planned for P8b) or msi-only = true", f->name);
            continue;
        }
        item_t *it = &items[n++];
        snprintf(it->path, sizeof it->path, "%s%s%s", dir, dir[0] ? "\\" : "", f->name);
        it->source = f->source_path;
        it->pos = f->pos;
        if (reserved(it->path)) DERR(f->pos, "RP1610", "'%s' is a name the MSIX format keeps for itself", it->path);
    }
    // Logos: the three given (checked), or plain ones made here.
    static const uint32_t logo_px[3] = { 150, 44, 50 };
    static const char *const logo_default[3] = { "Assets\\Square150x150Logo.png", "Assets\\Square44x44Logo.png", "Assets\\StoreLogo.png" };
    char logos[3][2200];
    for (int k = 0; k < 3; ++k) {
        if (ir->msix_logo_path[k]) {
            // A given logo must also be a file of the package: find it by source path.
            const item_t *found = NULL;
            for (size_t i = 0; i < n && !found; ++i) {
                if (items[i].source && strcmp(items[i].source, ir->msix_logo_path[k]) == 0) found = &items[i];
            }
            uint8_t *png = NULL;
            size_t pl = 0;
            uint32_t w = 0, h = 0;
            if (rp_pal_read_file(alloc, ir->msix_logo_path[k], 16u << 20, &png, &pl) != PROVEN_OK || !png_size(png, pl, &w, &h)) {
                DERR(ir->msix_app_pos, "RP1608", "logo '%s' is not a PNG file", ir->msix_logo[k]);
            } else if (w != logo_px[k] || h != logo_px[k]) {
                DERR(ir->msix_app_pos, "RP1608", "logo '%s' is %ux%u pixels; it must be %ux%u", ir->msix_logo[k], (unsigned)w, (unsigned)h,
                     (unsigned)logo_px[k], (unsigned)logo_px[k]);
            }
            rp_mem_free(alloc, png);
            if (found) {
                snprintf(logos[k], sizeof logos[k], "%s", found->path);
            } else {
                // Not otherwise installed: it goes in under Assets\ with its own name.
                const char *b = strrchr(ir->msix_logo[k], '/');
                item_t *it = &items[n++];
                snprintf(it->path, sizeof it->path, "Assets\\%s", b ? b + 1 : ir->msix_logo[k]);
                it->source = ir->msix_logo_path[k];
                it->pos = ir->msix_app_pos;
                snprintf(logos[k], sizeof logos[k], "%s", it->path);
            }
        } else {
            item_t *it = &items[n++];
            snprintf(it->path, sizeof it->path, "%s", logo_default[k]);
            it->pos = ir->msix_app_pos;
            if (default_logo(alloc, logo_px[k], &it->data, &it->data_len) != PROVEN_OK) DERR(top, "RP1608", "cannot make a default logo");
            snprintf(logos[k], sizeof logos[k], "%s", it->path);
        }
    }
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
        manifest(&man, ir, opt, exe_path, logos);
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
    return err;
}
