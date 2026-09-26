// src/pe/pe.c - PE machine type and version resource reader (include/rubrapack/pe.h).
// Layout from the Microsoft PE/COFF specification and the VS_VERSIONINFO documentation.

#include "rubrapack/limits.h"
#include "rubrapack/pe.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    const uint8_t *d;
    size_t         n;
    const uint8_t *sections;
    uint16_t       nsections;
} pe_t;

// File offset of an RVA range, or false when no section holds all of it.
static bool rva_to_off(const pe_t *pe, uint32_t rva, uint32_t size, size_t *off) {
    for (uint16_t k = 0; k < pe->nsections; ++k) {
        const uint8_t *s = pe->sections + 40u * k;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), rawsize = rd32(s + 16), raw = rd32(s + 20);
        uint32_t span = vsize > rawsize ? vsize : rawsize;
        if (rva >= va && (uint64_t)rva - va + size <= span) {
            uint64_t o = (uint64_t)raw + (rva - va);
            if (!rp_range_ok(o, size, pe->n) || (uint64_t)rva - va + size > rawsize) return false;
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
    if (!rp_range_ok(dir, 16, root + root_size)) return false;
    uint16_t named = rd16(pe->d + dir + 12), ids = rd16(pe->d + dir + 14);
    size_t count = (size_t)named + ids;
    if (!rp_range_ok(dir + 16, count * 8, root + root_size)) return false;
    for (size_t k = 0; k < count; ++k) {
        const uint8_t *e = pe->d + dir + 16 + 8 * k;
        uint32_t name = rd32(e), off = rd32(e + 4);
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
static uint16_t translation_language(const uint8_t *p, size_t n) {
    static const char key[] = "Translation";
    for (size_t i = 0; i + 2 * sizeof key + 4 <= n; i += 2) {
        bool hit = true;
        for (size_t k = 0; k < sizeof key; ++k) {       // includes the terminating NUL
            if (p[i + 2 * k] != (uint8_t)key[k] || p[i + 2 * k + 1] != 0) {
                hit = false;
                break;
            }
        }
        if (!hit) continue;
        size_t v = (i + 2 * sizeof key + 3) & ~(size_t)3;   // value is 32-bit aligned
        if (v + 2 <= n) return rd16(p + v);
    }
    return 0;
}

proven_err_t rp_pe_read(const uint8_t *data, size_t len, rp_pe_info_t *info) {
    if (info == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    memset(info, 0, sizeof *info);
    if (len < 0x40 || data[0] != 'M' || data[1] != 'Z') return PROVEN_OK;
    uint32_t pe_off = rd32(data + 0x3C);
    if (!rp_range_ok(pe_off, 24, len) || memcmp(data + pe_off, "PE\0\0", 4) != 0) return PROVEN_OK;
    info->is_pe = true;
    const uint8_t *coff = data + pe_off + 4;
    info->machine = rd16(coff);
    info->is_dll = (rd16(coff + 18) & 0x2000) != 0;
    uint16_t nsections = rd16(coff + 2), opt_size = rd16(coff + 16);
    size_t opt = pe_off + 24u;
    if (!rp_range_ok(opt, opt_size, len) || opt_size < 2) return PROVEN_ERR_INVALID_FORMAT;
    uint16_t magic = rd16(data + opt);
    size_t dirs_at = magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0;
    if (dirs_at == 0 || opt_size < dirs_at + 8 * 3) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t ndirs = rd32(data + opt + dirs_at - 4);
    if (!rp_range_ok(opt, (size_t)opt_size + 40u * nsections, len)) return PROVEN_ERR_INVALID_FORMAT;
    pe_t pe = { data, len, data + opt + opt_size, nsections };
    if (ndirs <= 2) return PROVEN_OK;                       // no resource directory entry
    uint32_t res_rva = rd32(data + opt + dirs_at + 16), res_size = rd32(data + opt + dirs_at + 20);
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
    uint32_t data_rva = rd32(data + root + t3), data_size = rd32(data + root + t3 + 4);
    size_t vi;
    if (!rva_to_off(&pe, data_rva, data_size, &vi) || data_size < 6 + 32 + 52) return PROVEN_ERR_INVALID_FORMAT;

    // VS_VERSIONINFO: wLength, wValueLength, wType, "VS_VERSION_INFO\0" (UTF-16), padding,
    // VS_FIXEDFILEINFO (signature FEEF04BD).
    const uint8_t *v = data + vi;
    size_t fixed = (6 + 32 + 3) & ~(size_t)3;               // header + key, 32-bit aligned
    if (rd16(v + 2) < 52 || rd32(v + fixed) != 0xFEEF04BDu) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t ms = rd32(v + fixed + 8), lsw = rd32(v + fixed + 12);
    info->has_version = true;
    info->version[0] = (uint16_t)(ms >> 16);
    info->version[1] = (uint16_t)ms;
    info->version[2] = (uint16_t)(lsw >> 16);
    info->version[3] = (uint16_t)lsw;
    uint16_t tl = translation_language(v, data_size);
    info->language = tl ? tl : (uint16_t)lang;
    return PROVEN_OK;
}
