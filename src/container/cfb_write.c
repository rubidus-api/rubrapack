// src/container/cfb_write.c - compound file writer (include/rubrapack/cfb.h, format notes F1).
//
// Sector layout: FAT sectors, DIFAT sectors, directory, mini FAT, mini stream, then each large
// stream in the order given. Streams under 4096 bytes go to the mini stream in 64-byte units.

#include "rubrapack/cfb.h"
#include "rubrapack/mem.h"

#include <stdckdint.h>
#include <string.h>

#define FREESECT   UINT32_C(0xFFFFFFFF)
#define ENDOFCHAIN UINT32_C(0xFFFFFFFE)
#define FATSECT    UINT32_C(0xFFFFFFFD)
#define DIFSECT    UINT32_C(0xFFFFFFFC)

enum { CUTOFF = 4096, MINI = 64, HEADER_DIFAT = 109, ENTRY = 128 };

static const uint8_t signature[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t upper(uint16_t c) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c; }

// MS-CFB sibling order: shorter names first, then upper-cased UTF-16 comparison.
static int name_cmp(const rp_cfb_stream_t *a, const rp_cfb_stream_t *b) {
    if (a->name_len != b->name_len) return a->name_len < b->name_len ? -1 : 1;
    for (size_t i = 0; i < a->name_len; ++i) {
        uint16_t x = upper(a->name[i]), y = upper(b->name[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// Builds a balanced binary tree over sorted[lo, hi) and returns the root's directory id.
// Directory id of stream k (in the caller's order) is k + 1.
static uint32_t build_tree(const size_t *sorted, size_t lo, size_t hi, uint32_t *left, uint32_t *right) {
    if (lo >= hi) return FREESECT;
    size_t mid = lo + (hi - lo) / 2;
    uint32_t id = (uint32_t)(sorted[mid] + 1);
    left[id] = build_tree(sorted, lo, mid, left, right);
    right[id] = build_tree(sorted, mid + 1, hi, left, right);
    return id;
}

static size_t ceil_div(size_t a, size_t b) { return a / b + (a % b != 0); }

proven_err_t rp_cfb_write(proven_allocator_t alloc, unsigned sector_shift, const uint8_t clsid[16],
                          const rp_cfb_stream_t *streams, size_t count, const rp_limits_t *limits, uint8_t **out,
                          size_t *len) {
    if (out == NULL || len == NULL || limits == NULL || (streams == NULL && count != 0)) return PROVEN_ERR_INVALID_ARG;
    if (sector_shift != 9 && sector_shift != 12) return PROVEN_ERR_INVALID_ARG;
    if (count + 1 > limits->max_entries || count >= FREESECT - 1) return PROVEN_ERR_OUT_OF_BOUNDS;
    const size_t ss = (size_t)1 << sector_shift;
    const size_t per_fat = ss / 4;

    // Sizes.
    size_t mini_len = 0, big_sectors = 0;
    for (size_t k = 0; k < count; ++k) {
        const rp_cfb_stream_t *s = &streams[k];
        if (s->name_len == 0 || s->name_len > 31 || (s->size != 0 && s->data == NULL)) return PROVEN_ERR_INVALID_ARG;
        if (s->size < CUTOFF) {
            if (ckd_add(&mini_len, mini_len, ceil_div(s->size, MINI) * MINI)) return PROVEN_ERR_OVERFLOW;
        } else if (ckd_add(&big_sectors, big_sectors, ceil_div(s->size, ss))) {
            return PROVEN_ERR_OVERFLOW;
        }
    }
    size_t dir_sectors = ceil_div((count + 1) * ENTRY, ss);
    size_t mini_sectors = mini_len / MINI;
    size_t minifat_sectors = ceil_div(mini_sectors * 4, ss);
    size_t ministream_sectors = ceil_div(mini_len, ss);
    size_t data_sectors = dir_sectors + minifat_sectors + ministream_sectors + big_sectors;
    size_t fat = 1, difat = 0;
    for (;;) {      // the FAT has to cover its own sectors and the DIFAT's too
        size_t total = data_sectors + fat + difat;
        size_t need_fat = ceil_div(total, per_fat);
        size_t need_difat = need_fat > HEADER_DIFAT ? ceil_div(need_fat - HEADER_DIFAT, per_fat - 1) : 0;
        if (need_fat == fat && need_difat == difat) break;
        fat = need_fat;
        difat = need_difat;
    }
    size_t total_sectors = data_sectors + fat + difat;
    if (total_sectors >= FREESECT - 5) return PROVEN_ERR_OUT_OF_BOUNDS;
    size_t file_len;
    if (ckd_mul(&file_len, total_sectors + 1, ss)) return PROVEN_ERR_OVERFLOW;
    if (file_len > limits->max_output) return PROVEN_ERR_OUT_OF_BOUNDS;

    uint8_t *f = rp_mem_alloc(alloc, file_len, 1);
    size_t *sorted = rp_mem_alloc(alloc, count, sizeof *sorted);
    uint32_t *left = rp_mem_alloc(alloc, count + 1, sizeof *left);
    uint32_t *right = rp_mem_alloc(alloc, count + 1, sizeof *right);
    uint32_t *table = rp_mem_alloc(alloc, fat * per_fat, sizeof *table);
    uint32_t *minifat = rp_mem_alloc(alloc, minifat_sectors * per_fat, sizeof *minifat);
    if (f == NULL || sorted == NULL || left == NULL || right == NULL || table == NULL || minifat == NULL) {
        rp_mem_free(alloc, f);
        rp_mem_free(alloc, sorted);
        rp_mem_free(alloc, left);
        rp_mem_free(alloc, right);
        rp_mem_free(alloc, table);
        rp_mem_free(alloc, minifat);
        return PROVEN_ERR_NOMEM;
    }
    memset(f, 0, file_len);
    for (size_t i = 0; i < fat * per_fat; ++i) table[i] = FREESECT;
    for (size_t i = 0; i < minifat_sectors * per_fat; ++i) minifat[i] = FREESECT;
    proven_err_t err = PROVEN_OK;

    // Directory tree: insertion sort by name order (stream counts are small), duplicates refused.
    for (size_t k = 0; k < count; ++k) {
        size_t j = k;
        while (j > 0 && name_cmp(&streams[sorted[j - 1]], &streams[k]) > 0) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = k;
        if (j > 0 && name_cmp(&streams[sorted[j - 1]], &streams[k]) == 0) err = PROVEN_ERR_INVALID_ARG;
        if (j + 1 <= k && name_cmp(&streams[sorted[j + 1]], &streams[k]) == 0) err = PROVEN_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i <= count; ++i) left[i] = right[i] = FREESECT;
    uint32_t tree_root = build_tree(sorted, 0, count, left, right);

    // Sector numbers.
    uint32_t next = 0;
    uint32_t fat_first = next;
    next += (uint32_t)fat;
    uint32_t difat_first = next;
    next += (uint32_t)difat;
    uint32_t dir_first = next;
    next += (uint32_t)dir_sectors;
    uint32_t minifat_first = next;
    next += (uint32_t)minifat_sectors;
    uint32_t mini_first = next;
    next += (uint32_t)ministream_sectors;

    for (size_t i = 0; i < fat; ++i) table[fat_first + i] = FATSECT;
    for (size_t i = 0; i < difat; ++i) table[difat_first + i] = DIFSECT;
    // Chains for the contiguous runs.
    uint32_t runs[3][2] = { { dir_first, (uint32_t)dir_sectors },
                            { minifat_first, (uint32_t)minifat_sectors },
                            { mini_first, (uint32_t)ministream_sectors } };
    for (int r = 0; r < 3; ++r) {
        for (uint32_t i = 0; i < runs[r][1]; ++i) {
            table[runs[r][0] + i] = (i + 1 < runs[r][1]) ? runs[r][0] + i + 1 : ENDOFCHAIN;
        }
    }

    // Directory entries.
    uint8_t *dir = f + ((size_t)dir_first + 1) * ss;
    for (size_t i = 0; i < dir_sectors * ss / ENTRY; ++i) {     // unused entries: links = NOSTREAM
        uint8_t *e = dir + i * ENTRY;
        wr32(e + 68, FREESECT);
        wr32(e + 72, FREESECT);
        wr32(e + 76, FREESECT);
    }
    static const uint16_t root_name[] = { 'R', 'o', 'o', 't', ' ', 'E', 'n', 't', 'r', 'y' };
    uint8_t *root = dir;
    for (size_t i = 0; i < 10; ++i) wr16(root + 2 * i, root_name[i]);
    wr16(root + 64, 22);
    root[66] = 5;
    root[67] = 1;       // black
    wr32(root + 76, tree_root);
    if (clsid) memcpy(root + 80, clsid, 16);
    wr32(root + 116, mini_len ? mini_first : ENDOFCHAIN);
    wr64(root + 120, mini_len);

    uint32_t mini_next = 0;
    for (size_t k = 0; k < count && err == PROVEN_OK; ++k) {
        const rp_cfb_stream_t *s = &streams[k];
        uint8_t *e = dir + (k + 1) * ENTRY;
        for (size_t i = 0; i < s->name_len; ++i) wr16(e + 2 * i, s->name[i]);
        wr16(e + 64, (uint16_t)((s->name_len + 1) * 2));
        e[66] = 2;
        e[67] = 1;
        wr32(e + 68, left[k + 1]);
        wr32(e + 72, right[k + 1]);
        wr32(e + 76, FREESECT);
        wr64(e + 120, s->size);
        if (s->size == 0) {
            wr32(e + 116, ENDOFCHAIN);
        } else if (s->size < CUTOFF) {
            uint32_t n = (uint32_t)ceil_div(s->size, MINI);
            wr32(e + 116, mini_next);
            // Mini sectors are laid out back to back inside the contiguous mini stream sectors.
            memcpy(f + ((size_t)mini_first + 1) * ss + (size_t)mini_next * MINI, s->data, s->size);
            for (uint32_t i = 0; i < n; ++i) minifat[mini_next + i] = (i + 1 < n) ? mini_next + i + 1 : ENDOFCHAIN;
            mini_next += n;
        } else {
            uint32_t n = (uint32_t)ceil_div(s->size, ss);
            wr32(e + 116, next);
            memcpy(f + ((size_t)next + 1) * ss, s->data, s->size);
            for (uint32_t i = 0; i < n; ++i) table[next + i] = (i + 1 < n) ? next + i + 1 : ENDOFCHAIN;
            next += n;
        }
    }

    // Mini FAT sectors, FAT sectors, DIFAT sectors.
    for (size_t i = 0; i < minifat_sectors * per_fat; ++i) {
        wr32(f + ((size_t)minifat_first + 1) * ss + i * 4, minifat[i]);
    }
    for (size_t i = 0; i < fat * per_fat; ++i) wr32(f + ((size_t)fat_first + 1) * ss + i * 4, table[i]);
    for (size_t d = 0; d < difat; ++d) {
        uint8_t *p = f + ((size_t)difat_first + d + 1) * ss;
        for (size_t i = 0; i < per_fat - 1; ++i) {
            size_t idx = HEADER_DIFAT + d * (per_fat - 1) + i;
            wr32(p + 4 * i, idx < fat ? fat_first + (uint32_t)idx : FREESECT);
        }
        wr32(p + 4 * (per_fat - 1), d + 1 < difat ? difat_first + (uint32_t)d + 1 : ENDOFCHAIN);
    }

    // Header.
    memcpy(f, signature, 8);
    wr16(f + 24, 0x003E);
    wr16(f + 26, sector_shift == 12 ? 4 : 3);
    wr16(f + 28, 0xFFFE);
    wr16(f + 30, (uint16_t)sector_shift);
    wr16(f + 32, 6);
    wr32(f + 40, sector_shift == 12 ? (uint32_t)dir_sectors : 0);
    wr32(f + 44, (uint32_t)fat);
    wr32(f + 48, dir_first);
    wr32(f + 56, CUTOFF);
    wr32(f + 60, minifat_sectors ? minifat_first : ENDOFCHAIN);
    wr32(f + 64, (uint32_t)minifat_sectors);
    wr32(f + 68, difat ? difat_first : ENDOFCHAIN);
    wr32(f + 72, (uint32_t)difat);
    for (size_t i = 0; i < HEADER_DIFAT; ++i) wr32(f + 76 + 4 * i, i < fat ? fat_first + (uint32_t)i : FREESECT);

    rp_mem_free(alloc, sorted);
    rp_mem_free(alloc, left);
    rp_mem_free(alloc, right);
    rp_mem_free(alloc, table);
    rp_mem_free(alloc, minifat);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, f);
        return err;
    }
    *out = f;
    *len = file_len;
    return PROVEN_OK;
}
