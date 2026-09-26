// src/container/cfb.c - compound file reader (include/rubrapack/cfb.h, docs: format notes F1).

#include "rubrapack/cfb.h"
#include "rubrapack/mem.h"

#include <string.h>

#define MAXREGSECT UINT32_C(0xFFFFFFFA)
#define ENDOFCHAIN UINT32_C(0xFFFFFFFE)

enum {
    HEADER_DIFAT = 109,
    DIR_ENTRY = 128,
    MINI_SECTOR = 64,
};

static const uint8_t signature[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static const uint8_t *sector(const rp_cfb_t *c, uint32_t s) {
    return c->data + ((size_t)s + 1) * c->sector_size;
}

// Copies `size` bytes following a regular chain from `start`. The chain must end exactly after
// the sectors the size needs; a loop never reaches ENDOFCHAIN, so it is refused too.
static proven_err_t read_regular(const rp_cfb_t *c, uint32_t start, uint64_t size, uint8_t *dst) {
    if (size == 0) return PROVEN_OK;
    uint64_t need = (size + c->sector_size - 1) / c->sector_size;
    if (need > c->sector_count) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t s = start;
    for (uint64_t i = 0; i < need; ++i) {
        if (s >= c->sector_count || s >= c->fat_len) return PROVEN_ERR_INVALID_FORMAT;
        size_t take = (size - i * c->sector_size) < c->sector_size ? (size_t)(size - i * c->sector_size)
                                                                     : c->sector_size;
        memcpy(dst + i * c->sector_size, sector(c, s), take);
        s = c->fat[s];
    }
    return s == ENDOFCHAIN ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

static proven_err_t read_mini(const rp_cfb_t *c, uint32_t start, uint64_t size, uint8_t *dst) {
    if (size == 0) return PROVEN_OK;
    uint64_t need = (size + MINI_SECTOR - 1) / MINI_SECTOR;
    if (need > c->minifat_len) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t s = start;
    for (uint64_t i = 0; i < need; ++i) {
        if (s >= c->minifat_len || ((uint64_t)s + 1) * MINI_SECTOR > c->mini_len) {
            return PROVEN_ERR_INVALID_FORMAT;
        }
        size_t take = (size - i * MINI_SECTOR) < MINI_SECTOR ? (size_t)(size - i * MINI_SECTOR) : MINI_SECTOR;
        memcpy(dst + i * MINI_SECTOR, c->mini + (size_t)s * MINI_SECTOR, take);
        s = c->minifat[s];
    }
    return s == ENDOFCHAIN ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

// Walks a chain of unknown length (directory, mini FAT) and returns its sectors.
static proven_err_t walk_chain(const rp_cfb_t *c, uint32_t start, uint32_t **out, size_t *count) {
    size_t n = 0;
    for (uint32_t s = start; s != ENDOFCHAIN; s = c->fat[s]) {
        if (s >= c->sector_count || s >= c->fat_len || n >= c->sector_count) return PROVEN_ERR_INVALID_FORMAT;
        ++n;
    }
    uint32_t *list = rp_mem_alloc(c->alloc, n, sizeof *list);
    if (list == NULL) return PROVEN_ERR_NOMEM;
    size_t i = 0;
    for (uint32_t s = start; s != ENDOFCHAIN; s = c->fat[s]) list[i++] = s;
    *out = list;
    *count = n;
    return PROVEN_OK;
}

void rp_cfb_close(rp_cfb_t *cfb) {
    if (cfb == NULL) return;
    rp_mem_free(cfb->alloc, cfb->fat);
    rp_mem_free(cfb->alloc, cfb->minifat);
    rp_mem_free(cfb->alloc, cfb->mini);
    rp_mem_free(cfb->alloc, cfb->entries);
    memset(cfb, 0, sizeof *cfb);
}

#define FAIL(e, msg) \
    do { \
        err = (e); \
        if (why) *why = (msg); \
        goto fail; \
    } while (0)

proven_err_t rp_cfb_open(rp_cfb_t *cfb, proven_allocator_t alloc, const uint8_t *data, size_t len,
                         const rp_limits_t *limits, const char **why) {
    proven_err_t err = PROVEN_OK;
    uint32_t *fat_sectors = NULL, *dir_sectors = NULL, *minifat_sectors = NULL;
    if (why) *why = NULL;
    if (cfb == NULL || data == NULL || limits == NULL || !proven_alloc_is_valid(alloc)) {
        return PROVEN_ERR_INVALID_ARG;
    }
    memset(cfb, 0, sizeof *cfb);
    cfb->alloc = alloc;
    cfb->data = data;
    cfb->len = len;
    rp_budget_t meta = rp_budget(limits->max_metadata);

    if (len < 512 || memcmp(data, signature, 8) != 0) FAIL(PROVEN_ERR_INVALID_FORMAT, "not a compound file");
    if (rd16(data + 28) != 0xFFFE) FAIL(PROVEN_ERR_INVALID_FORMAT, "bad byte order mark");
    cfb->major = rd16(data + 26);
    uint16_t shift = rd16(data + 30);
    if (!((cfb->major == 3 && shift == 9) || (cfb->major == 4 && shift == 12))) {
        FAIL(PROVEN_ERR_UNSUPPORTED, "unsupported version or sector size");
    }
    if (rd16(data + 32) != 6) FAIL(PROVEN_ERR_INVALID_FORMAT, "mini sector size is not 64");
    cfb->sector_size = 1u << shift;
    if (len < cfb->sector_size) FAIL(PROVEN_ERR_INVALID_FORMAT, "file shorter than its header");
    size_t sectors = (len - cfb->sector_size) / cfb->sector_size;
    if (sectors > MAXREGSECT) FAIL(PROVEN_ERR_UNSUPPORTED, "file too large");
    cfb->sector_count = (uint32_t)sectors;
    cfb->mini_cutoff = rd32(data + 56);
    if (cfb->mini_cutoff != 4096) FAIL(PROVEN_ERR_INVALID_FORMAT, "mini stream cutoff is not 4096");

    uint32_t num_fat = rd32(data + 44);
    uint32_t first_dir = rd32(data + 48);
    uint32_t first_minifat = rd32(data + 60);
    uint32_t num_minifat = rd32(data + 64);
    uint32_t first_difat = rd32(data + 68);
    uint32_t num_difat = rd32(data + 72);
    if (num_fat == 0 || num_fat > cfb->sector_count) FAIL(PROVEN_ERR_INVALID_FORMAT, "bad FAT sector count");
    if (num_difat > cfb->sector_count) FAIL(PROVEN_ERR_INVALID_FORMAT, "bad DIFAT sector count");

    // FAT sector numbers: 109 in the header, the rest in the DIFAT chain.
    fat_sectors = rp_mem_alloc(alloc, num_fat, sizeof *fat_sectors);
    if (fat_sectors == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
    size_t have = 0;
    for (size_t i = 0; i < HEADER_DIFAT && have < num_fat; ++i) fat_sectors[have++] = rd32(data + 76 + 4 * i);
    uint32_t per_difat = cfb->sector_size / 4 - 1;
    uint32_t d = first_difat;
    for (uint32_t k = 0; k < num_difat && have < num_fat; ++k) {
        if (d >= cfb->sector_count) FAIL(PROVEN_ERR_INVALID_FORMAT, "DIFAT chain leaves the file");
        const uint8_t *p = sector(cfb, d);
        for (uint32_t i = 0; i < per_difat && have < num_fat; ++i) fat_sectors[have++] = rd32(p + 4 * i);
        d = rd32(p + 4 * per_difat);
    }
    if (have < num_fat) FAIL(PROVEN_ERR_INVALID_FORMAT, "DIFAT lists fewer FAT sectors than the header");

    uint32_t per_fat = cfb->sector_size / 4;
    if (rp_budget_charge(&meta, (uint64_t)num_fat * cfb->sector_size) != PROVEN_OK) {
        FAIL(PROVEN_ERR_OUT_OF_BOUNDS, "FAT larger than the metadata limit");
    }
    cfb->fat_len = (size_t)num_fat * per_fat;
    cfb->fat = rp_mem_alloc(alloc, cfb->fat_len, sizeof *cfb->fat);
    if (cfb->fat == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
    for (uint32_t f = 0; f < num_fat; ++f) {
        if (fat_sectors[f] >= cfb->sector_count) FAIL(PROVEN_ERR_INVALID_FORMAT, "FAT sector outside the file");
        const uint8_t *p = sector(cfb, fat_sectors[f]);
        for (uint32_t i = 0; i < per_fat; ++i) cfb->fat[(size_t)f * per_fat + i] = rd32(p + 4 * i);
    }

    // Directory.
    size_t dir_count = 0;
    if (walk_chain(cfb, first_dir, &dir_sectors, &dir_count) != PROVEN_OK || dir_count == 0) {
        FAIL(PROVEN_ERR_INVALID_FORMAT, "directory chain is broken or loops");
    }
    size_t per_sector = cfb->sector_size / DIR_ENTRY;
    size_t entries = dir_count * per_sector;
    if (entries > limits->max_entries) FAIL(PROVEN_ERR_OUT_OF_BOUNDS, "more directory entries than the limit");
    if (rp_budget_charge(&meta, (uint64_t)dir_count * cfb->sector_size) != PROVEN_OK) {
        FAIL(PROVEN_ERR_OUT_OF_BOUNDS, "directory larger than the metadata limit");
    }
    cfb->entries = rp_mem_alloc(alloc, entries, sizeof *cfb->entries);
    if (cfb->entries == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
    cfb->entry_count = entries;
    for (size_t i = 0; i < entries; ++i) {
        const uint8_t *e = sector(cfb, dir_sectors[i / per_sector]) + (i % per_sector) * DIR_ENTRY;
        rp_cfb_entry_t *out = &cfb->entries[i];
        memset(out, 0, sizeof *out);
        out->type = e[66];
        if (out->type == RP_CFB_UNUSED) {
            out->left = out->right = out->child = RP_CFB_NONE;
            continue;
        }
        if (out->type != RP_CFB_STORAGE && out->type != RP_CFB_STREAM && out->type != RP_CFB_ROOT) {
            FAIL(PROVEN_ERR_INVALID_FORMAT, "unknown directory entry type");
        }
        uint16_t name_bytes = rd16(e + 64);
        if (name_bytes < 2 || name_bytes > 64 || name_bytes % 2 != 0) {
            FAIL(PROVEN_ERR_INVALID_FORMAT, "bad directory name length");
        }
        uint32_t units = name_bytes / 2;
        for (uint32_t k = 0; k < units; ++k) out->name[k] = rd16(e + 2 * k);
        if (out->name[units - 1] != 0) FAIL(PROVEN_ERR_INVALID_FORMAT, "directory name not terminated");
        out->name_len = units - 1;
        out->left = rd32(e + 68);
        out->right = rd32(e + 72);
        out->child = rd32(e + 76);
        memcpy(out->clsid, e + 80, 16);
        out->start = rd32(e + 116);
        out->size = cfb->major == 3 ? rd32(e + 120) : rd64(e + 120);
        uint32_t links[3] = { out->left, out->right, out->child };
        for (int k = 0; k < 3; ++k) {
            if (links[k] != RP_CFB_NONE && links[k] >= entries) FAIL(PROVEN_ERR_INVALID_FORMAT, "directory link out of range");
        }
    }
    if (cfb->entries[0].type != RP_CFB_ROOT) FAIL(PROVEN_ERR_INVALID_FORMAT, "first directory entry is not the root");

    // Mini FAT and mini stream.
    if (num_minifat > 0) {
        size_t n = 0;
        if (walk_chain(cfb, first_minifat, &minifat_sectors, &n) != PROVEN_OK || n != num_minifat) {
            FAIL(PROVEN_ERR_INVALID_FORMAT, "mini FAT chain is broken or has the wrong length");
        }
        if (rp_budget_charge(&meta, (uint64_t)n * cfb->sector_size) != PROVEN_OK) {
            FAIL(PROVEN_ERR_OUT_OF_BOUNDS, "mini FAT larger than the metadata limit");
        }
        cfb->minifat_len = n * per_fat;
        cfb->minifat = rp_mem_alloc(alloc, cfb->minifat_len, sizeof *cfb->minifat);
        if (cfb->minifat == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
        for (size_t f = 0; f < n; ++f) {
            const uint8_t *p = sector(cfb, minifat_sectors[f]);
            for (uint32_t i = 0; i < per_fat; ++i) cfb->minifat[f * per_fat + i] = rd32(p + 4 * i);
        }
    }
    const rp_cfb_entry_t *root = &cfb->entries[0];
    if (root->size > 0) {
        if (root->size > (uint64_t)cfb->sector_count * cfb->sector_size) {
            FAIL(PROVEN_ERR_INVALID_FORMAT, "mini stream larger than the file");
        }
        cfb->mini_len = (size_t)root->size;
        cfb->mini = rp_mem_alloc(alloc, cfb->mini_len, 1);
        if (cfb->mini == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
        if (read_regular(cfb, root->start, root->size, cfb->mini) != PROVEN_OK) {
            FAIL(PROVEN_ERR_INVALID_FORMAT, "mini stream chain is broken");
        }
    }

    rp_mem_free(alloc, fat_sectors);
    rp_mem_free(alloc, dir_sectors);
    rp_mem_free(alloc, minifat_sectors);
    return PROVEN_OK;

fail:
    rp_mem_free(alloc, fat_sectors);
    rp_mem_free(alloc, dir_sectors);
    rp_mem_free(alloc, minifat_sectors);
    rp_cfb_close(cfb);
    return err;
}

proven_err_t rp_cfb_children(const rp_cfb_t *cfb, uint32_t storage, uint32_t *ids, size_t cap,
                             size_t *count) {
    if (cfb == NULL || count == NULL || storage >= cfb->entry_count) return PROVEN_ERR_INVALID_ARG;
    const rp_cfb_entry_t *parent = &cfb->entries[storage];
    if (parent->type != RP_CFB_STORAGE && parent->type != RP_CFB_ROOT) return PROVEN_ERR_INVALID_ARG;

    // In-order walk with an explicit stack; a node seen twice means the tree loops.
    uint32_t *stack = rp_mem_alloc(cfb->alloc, cfb->entry_count, sizeof *stack);
    uint8_t *seen = rp_mem_alloc(cfb->alloc, cfb->entry_count, 1);
    if (stack == NULL || seen == NULL) {
        rp_mem_free(cfb->alloc, stack);
        rp_mem_free(cfb->alloc, seen);
        return PROVEN_ERR_NOMEM;
    }
    memset(seen, 0, cfb->entry_count);
    proven_err_t err = PROVEN_OK;
    size_t depth = 0, n = 0;
    uint32_t node = parent->child;
    while (err == PROVEN_OK && (node != RP_CFB_NONE || depth > 0)) {
        while (node != RP_CFB_NONE) {
            if (seen[node] || cfb->entries[node].type == RP_CFB_UNUSED || cfb->entries[node].type == RP_CFB_ROOT) {
                err = PROVEN_ERR_INVALID_FORMAT;
                break;
            }
            seen[node] = 1;
            stack[depth++] = node;
            node = cfb->entries[node].left;
        }
        if (err != PROVEN_OK) break;
        node = stack[--depth];
        if (ids != NULL) {
            if (n >= cap) {
                err = PROVEN_ERR_OUT_OF_BOUNDS;
                break;
            }
            ids[n] = node;
        }
        ++n;
        node = cfb->entries[node].right;
    }
    rp_mem_free(cfb->alloc, stack);
    rp_mem_free(cfb->alloc, seen);
    if (err == PROVEN_OK) *count = n;
    return err;
}

proven_err_t rp_cfb_find(const rp_cfb_t *cfb, uint32_t storage, const uint16_t *name, size_t name_len,
                         uint32_t *id) {
    size_t n = 0;
    proven_err_t err = rp_cfb_children(cfb, storage, NULL, 0, &n);
    if (err != PROVEN_OK) return err;
    uint32_t *ids = rp_mem_alloc(cfb->alloc, n, sizeof *ids);
    if (ids == NULL) return PROVEN_ERR_NOMEM;
    err = rp_cfb_children(cfb, storage, ids, n, &n);
    proven_err_t result = err == PROVEN_OK ? PROVEN_ERR_NOT_FOUND : err;
    for (size_t i = 0; err == PROVEN_OK && i < n; ++i) {
        const rp_cfb_entry_t *e = &cfb->entries[ids[i]];
        if (e->name_len == name_len && memcmp(e->name, name, name_len * sizeof *name) == 0) {
            *id = ids[i];
            result = PROVEN_OK;
            break;
        }
    }
    rp_mem_free(cfb->alloc, ids);
    return result;
}

proven_err_t rp_cfb_read(const rp_cfb_t *cfb, uint32_t id, uint8_t *dst, size_t cap) {
    if (cfb == NULL || id >= cfb->entry_count || (dst == NULL && cap != 0)) return PROVEN_ERR_INVALID_ARG;
    const rp_cfb_entry_t *e = &cfb->entries[id];
    if (e->type != RP_CFB_STREAM) return PROVEN_ERR_INVALID_ARG;
    if (e->size > cap) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (e->size < cfb->mini_cutoff) return read_mini(cfb, e->start, e->size, dst);
    return read_regular(cfb, e->start, e->size, dst);
}
