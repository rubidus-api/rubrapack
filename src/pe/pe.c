// src/pe/pe.c - PE machine type and version resource reader (include/rubrapack/pe.h).
// Layout from the Microsoft PE/COFF specification and the VS_VERSIONINFO documentation.

#include "rubrapack/crypto.h"
#include "rubrapack/in.h"
#include "rubrapack/limits.h"
#include "rubrapack/pe.h"

#include <string.h>

typedef struct {
    rp_in_t  in;            // the whole file
    size_t   sections;      // offset of the section table
    uint16_t nsections;
} pe_t;

// File offset of an RVA range, or false when no section holds all of it.
static bool rva_to_off(const pe_t *pe, uint32_t rva, uint32_t size, size_t *off) {
    rp_in_t in = pe->in;
    for (uint16_t k = 0; k < pe->nsections; ++k) {
        rp_in_t s = rp_in_sub(&in, pe->sections + 40u * k, 40);
        uint32_t vsize = rp_in32(&s, 8), va = rp_in32(&s, 12), rawsize = rp_in32(&s, 16), raw = rp_in32(&s, 20);
        if (s.bad) return false;
        uint32_t span = vsize > rawsize ? vsize : rawsize;
        if (rva >= va && (uint64_t)rva - va + size <= span) {
            uint64_t o = (uint64_t)raw + (rva - va);
            if (!rp_range_ok(o, size, in.n) || (uint64_t)rva - va + size > rawsize) return false;
            *off = (size_t)o;
            return true;
        }
    }
    return false;
}

// Looks up one level of the resource directory tree: the entry with integer ID `id`, or the
// first entry when id < 0. Returns the entry's offset field and sets *lang_id to its ID.
static bool res_entry(const pe_t *pe, size_t root, size_t dir, uint32_t root_size, int32_t id, uint32_t *target,
                      uint32_t *found_id) {
    rp_in_t in = pe->in;
    if (!rp_range_ok(dir, 16, root + root_size)) return false;
    uint16_t named = rp_in16(&in, dir + 12), ids = rp_in16(&in, dir + 14);
    size_t count = (size_t)named + ids;
    if (!rp_range_ok(dir + 16, count * 8, root + root_size)) return false;
    rp_in_t list = rp_in_sub(&in, dir + 16, count * 8);
    for (size_t k = 0; k < count && !in.bad; ++k) {
        uint32_t name = rp_in32(&list, 8 * k), off = rp_in32(&list, 8 * k + 4);
        if (list.bad) return false;
        if (name & 0x80000000u) continue;           // named entries are not what we look for
        if (id < 0 || name == (uint32_t)id) {
            *target = off;
            *found_id = name;
            return true;
        }
    }
    return false;
}

// VarFileInfo\Translation: first language, searched as UTF-16 "Translation" inside the
// version resource (the block layout nests; a key search inside its bounds is enough).
static uint16_t translation_language(rp_in_t *v) {
    static const char key[] = "Translation";
    for (size_t i = 0; i + 2 * sizeof key + 4 <= v->n; i += 2) {
        const uint8_t *p = rp_in_bytes(v, i, 2 * sizeof key);
        if (p == NULL) return 0;
        bool hit = true;
        for (size_t k = 0; k < sizeof key; ++k) {       // includes the terminating NUL
            if (p[2 * k] != (uint8_t)key[k] || p[2 * k + 1] != 0) {
                hit = false;
                break;
            }
        }
        if (!hit) continue;
        size_t at = (i + 2 * sizeof key + 3) & ~(size_t)3;   // value is 32-bit aligned
        if (rp_in_has(v, at, 2)) return rp_in16(v, at);
    }
    return 0;
}

proven_err_t rp_pe_read(const uint8_t *data, size_t len, rp_pe_info_t *info) {
    if (info == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    memset(info, 0, sizeof *info);
    rp_in_t in = rp_in(data, len);
    if (len < 0x40 || rp_in8(&in, 0) != 'M' || rp_in8(&in, 1) != 'Z') return PROVEN_OK;
    uint32_t pe_off = rp_in32(&in, 0x3C);
    const uint8_t *sig = rp_range_ok(pe_off, 24, len) ? rp_in_bytes(&in, pe_off, 4) : NULL;
    if (sig == NULL || memcmp(sig, "PE\0\0", 4) != 0) return PROVEN_OK;
    info->is_pe = true;
    rp_in_t coff = rp_in_sub(&in, (size_t)pe_off + 4, 20);
    info->machine = rp_in16(&coff, 0);
    info->is_dll = (rp_in16(&coff, 18) & 0x2000) != 0;
    uint16_t nsections = rp_in16(&coff, 2), opt_size = rp_in16(&coff, 16);
    size_t opt = pe_off + 24u;
    if (!rp_range_ok(opt, opt_size, len) || opt_size < 2 || coff.bad) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t oh = rp_in_sub(&in, opt, opt_size);
    uint16_t magic = rp_in16(&oh, 0);
    if (opt_size >= 70) info->subsystem = rp_in16(&oh, 68);     // the same place in PE32 and PE32+
    size_t dirs_at = magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0;
    if (dirs_at == 0 || opt_size < dirs_at + 8 * 3) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t ndirs = rp_in32(&oh, dirs_at - 4);
    if (!rp_range_ok(opt, (size_t)opt_size + 40u * nsections, len)) return PROVEN_ERR_INVALID_FORMAT;
    pe_t pe = { in, opt + opt_size, nsections };
    if (ndirs <= 2) return PROVEN_OK;                       // no resource directory entry
    uint32_t res_rva = rp_in32(&oh, dirs_at + 16), res_size = rp_in32(&oh, dirs_at + 20);
    if (oh.bad) return PROVEN_ERR_INVALID_FORMAT;
    if (res_rva == 0 || res_size == 0) return PROVEN_OK;
    size_t root;
    if (!rva_to_off(&pe, res_rva, res_size, &root)) return PROVEN_ERR_INVALID_FORMAT;

    // RT_VERSION (16) -> first name -> first language -> data entry.
    uint32_t t1, t2, t3, id, lang;
    if (!res_entry(&pe, root, root, res_size, 16, &t1, &id)) return PROVEN_OK;
    if (!(t1 & 0x80000000u) || !res_entry(&pe, root, root + (t1 & 0x7FFFFFFFu), res_size, -1, &t2, &id)) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (!(t2 & 0x80000000u) || !res_entry(&pe, root, root + (t2 & 0x7FFFFFFFu), res_size, -1, &t3, &lang)) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if ((t3 & 0x80000000u) || !rp_range_ok(root + t3, 16, root + res_size)) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t data_rva = rp_in32(&in, root + t3), data_size = rp_in32(&in, root + t3 + 4);
    size_t vi;
    if (in.bad || !rva_to_off(&pe, data_rva, data_size, &vi) || data_size < 6 + 32 + 52) return PROVEN_ERR_INVALID_FORMAT;

    // VS_VERSIONINFO: wLength, wValueLength, wType, "VS_VERSION_INFO\0" (UTF-16), padding,
    // VS_FIXEDFILEINFO (signature FEEF04BD).
    rp_in_t v = rp_in_sub(&in, vi, data_size);
    size_t fixed = (6 + 32 + 3) & ~(size_t)3;               // header + key, 32-bit aligned
    if (rp_in16(&v, 2) < 52 || rp_in32(&v, fixed) != 0xFEEF04BDu) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t ms = rp_in32(&v, fixed + 8), lsw = rp_in32(&v, fixed + 12);
    if (v.bad) return PROVEN_ERR_INVALID_FORMAT;
    info->has_version = true;
    info->version[0] = (uint16_t)(ms >> 16);
    info->version[1] = (uint16_t)ms;
    info->version[2] = (uint16_t)(lsw >> 16);
    info->version[3] = (uint16_t)lsw;
    uint16_t tl = translation_language(&v);
    info->language = tl ? tl : (uint16_t)lang;
    return PROVEN_OK;
}

// ---- .NET assemblies (RFC-0031 B9; ECMA-335 Partition II) ----------------------------------------

// Column kinds of the metadata tables before Assembly (0x20): fixed sizes, heap indexes, an index
// into one table (IX + table), or a coded index (CI_*).
enum { C2 = 1, C4, CS, CG, CB, CI_TDOR, CI_HCONST, CI_HCA, CI_HFM, CI_HDS, CI_MRP, CI_HSEM, CI_MDOR, CI_MFWD, CI_CAT, CI_RS, IX = 0x40, END = 0 };
static const uint8_t table_cols[0x21][10] = {
    [0x00] = { C2, CS, CG, CG, CG },                         // Module
    [0x01] = { CI_RS, CS, CS },                              // TypeRef
    [0x02] = { C4, CS, CS, CI_TDOR, IX + 0x04, IX + 0x06 },  // TypeDef
    [0x03] = { IX + 0x04 },                                  // FieldPtr
    [0x04] = { C2, CS, CB },                                 // Field
    [0x05] = { IX + 0x06 },                                  // MethodPtr
    [0x06] = { C4, C2, C2, CS, CB, IX + 0x08 },              // MethodDef
    [0x07] = { IX + 0x08 },                                  // ParamPtr
    [0x08] = { C2, C2, CS },                                 // Param
    [0x09] = { IX + 0x02, CI_TDOR },                         // InterfaceImpl
    [0x0A] = { CI_MRP, CS, CB },                             // MemberRef
    [0x0B] = { C2, CI_HCONST, CB },                          // Constant (type and a padding byte)
    [0x0C] = { CI_HCA, CI_CAT, CB },                         // CustomAttribute
    [0x0D] = { CI_HFM, CB },                                 // FieldMarshal
    [0x0E] = { C2, CI_HDS, CB },                             // DeclSecurity
    [0x0F] = { C2, C4, IX + 0x02 },                          // ClassLayout
    [0x10] = { C4, IX + 0x04 },                              // FieldLayout
    [0x11] = { CB },                                         // StandAloneSig
    [0x12] = { IX + 0x02, IX + 0x14 },                       // EventMap
    [0x13] = { IX + 0x14 },                                  // EventPtr
    [0x14] = { C2, CS, CI_TDOR },                            // Event
    [0x15] = { IX + 0x02, IX + 0x17 },                       // PropertyMap
    [0x16] = { IX + 0x17 },                                  // PropertyPtr
    [0x17] = { C2, CS, CB },                                 // Property
    [0x18] = { C2, IX + 0x06, CI_HSEM },                     // MethodSemantics
    [0x19] = { IX + 0x02, CI_MDOR, CI_MDOR },                // MethodImpl
    [0x1A] = { CS },                                         // ModuleRef
    [0x1B] = { CB },                                         // TypeSpec
    [0x1C] = { C2, CI_MFWD, CS, IX + 0x1A },                 // ImplMap
    [0x1D] = { C4, IX + 0x04 },                              // FieldRVA
    [0x1E] = { C4, C4 },                                     // ENCLog
    [0x1F] = { C4 },                                         // ENCMap
    [0x20] = { C4, C2, C2, C2, C2, C4, CB, CS, CS },         // Assembly
};

// The tables a coded index can name (0xFF: a tag no table has), and its tag bits.
static const struct { uint8_t bits; uint8_t n; uint8_t t[22]; } coded[] = {
    [CI_TDOR] = { 2, 3, { 0x02, 0x01, 0x1B } },
    [CI_HCONST] = { 2, 3, { 0x04, 0x08, 0x17 } },
    [CI_HCA] = { 5, 22, { 0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C,
                          0x2B } },
    [CI_HFM] = { 1, 2, { 0x04, 0x08 } },
    [CI_HDS] = { 2, 3, { 0x02, 0x06, 0x20 } },
    [CI_MRP] = { 3, 5, { 0x02, 0x01, 0x1A, 0x06, 0x1B } },
    [CI_HSEM] = { 1, 2, { 0x14, 0x17 } },
    [CI_MDOR] = { 1, 2, { 0x06, 0x0A } },
    [CI_MFWD] = { 1, 2, { 0x04, 0x06 } },
    [CI_CAT] = { 3, 5, { 0xFF, 0xFF, 0x06, 0x0A, 0xFF } },
    [CI_RS] = { 2, 4, { 0x00, 0x1A, 0x23, 0x01 } },
};

typedef struct {
    uint32_t rows[64];
    unsigned str, guid, blob;   // heap index sizes
} md_t;

static unsigned col_size(const md_t *m, uint8_t c) {
    if (c == C2) return 2;
    if (c == C4) return 4;
    if (c == CS) return m->str;
    if (c == CG) return m->guid;
    if (c == CB) return m->blob;
    if (c >= IX) return m->rows[c - IX] < 0x10000 ? 2 : 4;
    uint32_t most = 0;
    for (unsigned i = 0; i < coded[c].n; ++i) {
        if (coded[c].t[i] != 0xFF && m->rows[coded[c].t[i]] > most) most = m->rows[coded[c].t[i]];
    }
    return most < (1u << (16 - coded[c].bits)) ? 2 : 4;
}

static uint32_t rd_n(rp_in_t *in, size_t off, unsigned n) { return n == 2 ? rp_in16(in, off) : rp_in32(in, off); }

proven_err_t rp_pe_assembly(const uint8_t *data, size_t len, rp_pe_assembly_t *out) {
    if (out == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    rp_in_t in = rp_in(data, len);
    if (len < 0x40 || rp_in8(&in, 0) != 'M' || rp_in8(&in, 1) != 'Z') return PROVEN_OK;
    uint32_t pe_off = rp_in32(&in, 0x3C);
    const uint8_t *sig = rp_range_ok(pe_off, 24, len) ? rp_in_bytes(&in, pe_off, 4) : NULL;
    if (sig == NULL || memcmp(sig, "PE\0\0", 4) != 0) return PROVEN_OK;
    rp_in_t coff = rp_in_sub(&in, (size_t)pe_off + 4, 20);
    uint16_t machine = rp_in16(&coff, 0), nsections = rp_in16(&coff, 2), opt_size = rp_in16(&coff, 16);
    size_t opt = pe_off + 24u;
    if (!rp_range_ok(opt, opt_size, len) || opt_size < 2 || coff.bad) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t oh = rp_in_sub(&in, opt, opt_size);
    uint16_t magic = rp_in16(&oh, 0);
    size_t dirs_at = magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0;
    if (dirs_at == 0 || opt_size < dirs_at + 8 * 15 || rp_in32(&oh, dirs_at - 4) < 15) return PROVEN_OK;      // no CLI header entry
    if (!rp_range_ok(opt, (size_t)opt_size + 40u * nsections, len)) return PROVEN_ERR_INVALID_FORMAT;
    pe_t pe = { in, opt + opt_size, nsections };
    uint32_t cli_rva = rp_in32(&oh, dirs_at + 14 * 8), cli_size = rp_in32(&oh, dirs_at + 14 * 8 + 4);
    if (oh.bad) return PROVEN_ERR_INVALID_FORMAT;
    if (cli_rva == 0 || cli_size < 72) return PROVEN_OK;
    size_t cli, root, tabs = 0, strs = 0, blobs = 0;
    uint32_t tabs_n = 0, strs_n = 0, blobs_n = 0;
    if (!rva_to_off(&pe, cli_rva, 72, &cli)) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t ch = rp_in_sub(&in, cli, 72);
    uint32_t md_rva = rp_in32(&ch, 8), md_size = rp_in32(&ch, 12), flags = rp_in32(&ch, 16);
    if (ch.bad || md_size < 32 || !rva_to_off(&pe, md_rva, md_size, &root)) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t md = rp_in_sub(&in, root, md_size);
    if (rp_in32(&md, 0) != 0x424A5342u) return PROVEN_ERR_INVALID_FORMAT;
    // The metadata root: version string, then the stream headers (offset, size, name padded to 4).
    uint32_t vlen = rp_in32(&md, 12);
    size_t at = 16 + (size_t)vlen;
    if (vlen > md_size || at + 4 > md_size) return PROVEN_ERR_INVALID_FORMAT;
    uint16_t nstreams = rp_in16(&md, at + 2);
    at += 4;
    for (uint16_t i = 0; i < nstreams; ++i) {
        if (at + 8 > md_size) return PROVEN_ERR_INVALID_FORMAT;
        uint32_t off = rp_in32(&md, at), size = rp_in32(&md, at + 4);
        size_t nl = 0;
        while (at + 8 + nl < md_size && nl < 32 && rp_in8(&md, at + 8 + nl)) ++nl;
        if (at + 8 + nl >= md_size || nl == 32) return PROVEN_ERR_INVALID_FORMAT;
        if ((uint64_t)off + size > md_size) return PROVEN_ERR_INVALID_FORMAT;
        const uint8_t *name = rp_in_bytes(&md, at + 8, nl);
        if (name == NULL) return PROVEN_ERR_INVALID_FORMAT;
        if (nl == 2 && memcmp(name, "#~", 2) == 0) tabs = root + off, tabs_n = size;
        else if (nl == 8 && memcmp(name, "#Strings", 8) == 0) strs = root + off, strs_n = size;
        else if (nl == 5 && memcmp(name, "#Blob", 5) == 0) blobs = root + off, blobs_n = size;
        at += 8 + ((nl + 4) & ~(size_t)3);
    }
    if (md.bad || tabs == 0 || tabs_n < 24 || strs == 0) return PROVEN_ERR_INVALID_FORMAT;
    // The #~ stream: heap sizes, the present tables, their row counts, then the tables.
    rp_in_t tb = rp_in_sub(&in, tabs, tabs_n);
    uint8_t heaps = rp_in8(&tb, 6);
    md_t m = { .str = heaps & 1 ? 4 : 2, .guid = heaps & 2 ? 4 : 2, .blob = heaps & 4 ? 4 : 2 };
    uint64_t valid = rp_in64(&tb, 8);
    size_t row_at = 24;
    for (unsigned t = 0; t < 64; ++t) {
        if (!(valid >> t & 1)) continue;
        if (row_at + 4 > tabs_n) return PROVEN_ERR_INVALID_FORMAT;
        m.rows[t] = rp_in32(&tb, row_at);
        row_at += 4;
    }
    if (tb.bad) return PROVEN_ERR_INVALID_FORMAT;
    if (!(valid >> 0x20 & 1) || m.rows[0x20] == 0) return PROVEN_OK;      // a module without an Assembly row
    uint64_t skip = 0;
    for (unsigned t = 0; t < 0x20; ++t) {
        if (!(valid >> t & 1)) continue;
        if (table_cols[t][0] == END) return PROVEN_ERR_INVALID_FORMAT;
        unsigned row = 0;
        for (unsigned c = 0; c < 10 && table_cols[t][c] != END; ++c) row += col_size(&m, table_cols[t][c]);
        skip += (uint64_t)row * m.rows[t];
    }
    unsigned arow = 0;
    for (unsigned c = 0; c < 10 && table_cols[0x20][c] != END; ++c) arow += col_size(&m, table_cols[0x20][c]);
    if (row_at + skip + arow > tabs_n) return PROVEN_ERR_INVALID_FORMAT;
    rp_in_t a = rp_in_sub(&tb, row_at + (size_t)skip, arow);
    out->version[0] = rp_in16(&a, 4);
    out->version[1] = rp_in16(&a, 6);
    out->version[2] = rp_in16(&a, 8);
    out->version[3] = rp_in16(&a, 10);
    uint32_t aflags = rp_in32(&a, 12), pk = rd_n(&a, 16, m.blob), nm = rd_n(&a, 16 + m.blob, m.str), cu = rd_n(&a, 16 + m.blob + m.str, m.str);
    if (a.bad) return PROVEN_ERR_INVALID_FORMAT;
    // Name and Culture from #Strings (NUL-terminated UTF-8).
    rp_in_t sh = rp_in_sub(&in, strs, strs_n);
    struct { uint32_t idx; char *dst; size_t cap; } s2[2] = { { nm, out->name, sizeof out->name }, { cu, out->culture, sizeof out->culture } };
    for (int k = 0; k < 2; ++k) {
        if (s2[k].idx >= strs_n) return PROVEN_ERR_INVALID_FORMAT;
        size_t n = 0;
        while (s2[k].idx + n < strs_n && rp_in8(&sh, s2[k].idx + n)) ++n;
        if (s2[k].idx + n >= strs_n || n >= s2[k].cap || !rp_in_copy(&sh, s2[k].idx, s2[k].dst, n)) return PROVEN_ERR_INVALID_FORMAT;
        s2[k].dst[n] = 0;
    }
    if (out->name[0] == 0) return PROVEN_ERR_INVALID_FORMAT;
    // The public key from #Blob (a compressed length, then the bytes): its token.
    if (pk && blobs && pk < blobs_n) {
        rp_in_t b = rp_in_sub(&in, blobs + pk, blobs_n - pk);
        size_t room = b.n, hl, bl;
        uint8_t b0 = rp_in8(&b, 0);
        if ((b0 & 0x80) == 0) hl = 1, bl = b0;
        else if ((b0 & 0xC0) == 0x80 && room >= 2) hl = 2, bl = (size_t)(b0 & 0x3F) << 8 | rp_in8(&b, 1);
        else if ((b0 & 0xE0) == 0xC0 && room >= 4) hl = 4, bl = (size_t)(b0 & 0x1F) << 24 | (size_t)rp_in8(&b, 1) << 16 | (size_t)rp_in8(&b, 2) << 8 | rp_in8(&b, 3);
        else return PROVEN_ERR_INVALID_FORMAT;
        const uint8_t *key = hl + bl <= room ? rp_in_bytes(&b, hl, bl) : NULL;
        if (key == NULL || b.bad) return PROVEN_ERR_INVALID_FORMAT;
        if (bl && (aflags & 1)) {                 // afPublicKey: the whole key
            rp_sha1_t h;
            uint8_t d[20];
            rp_sha1_init(&h);
            rp_sha1_update(&h, key, bl);
            rp_sha1_final(&h, d);
            for (int k = 0; k < 8; ++k) out->token[k] = d[19 - k];
            out->has_token = true;
        } else if (bl == 8) {                     // a token as it is
            memcpy(out->token, key, 8);
            out->has_token = true;
        }
    }
    // ILONLY (1) without 32BITREQUIRED (2): MSIL; else the machine's.
    const char *arch = (flags & 1) && !(flags & 2) && machine == RP_PE_I386 ? "MSIL"
                       : machine == RP_PE_I386                            ? "x86"
                       : machine == RP_PE_AMD64                           ? "AMD64"
                       : machine == RP_PE_ARM64                           ? "ARM64"
                                                                          : "";
    memcpy(out->arch, arch, strlen(arch) + 1);
    out->is_assembly = true;
    return PROVEN_OK;
}
