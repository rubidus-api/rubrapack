// src/container/cab.c - cabinet writer and reader (include/rubrapack/cab.h, MS-CAB).

#include "rubrapack/buf.h"
#include "rubrapack/cab.h"
#include "rubrapack/deflate.h"
#include "rubrapack/mem.h"

#include <string.h>

enum {
    HEADER = 36,
    FOLDER = 8,
    BLOCK = 32768,          // uncompressed bytes per CFDATA block
    TYPE_NONE = 0,
    TYPE_MSZIP = 1,
    ATTR_ARCH = 0x20,
    DATE_1980_01_01 = (0 << 9) | (1 << 5) | 1,
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// MS-CAB checksum: XOR of little-endian 32-bit words; the 1-3 trailing bytes form one more
// word with the first of them in the highest position used.
static uint32_t checksum(const uint8_t *p, size_t n, uint32_t seed) {
    uint32_t c = seed;
    size_t words = n / 4;
    for (size_t k = 0; k < words; ++k, p += 4) c ^= rd32(p);
    uint32_t tail = 0;
    switch (n % 4) {
    case 3: tail |= (uint32_t)*p++ << 16; [[fallthrough]];
    case 2: tail |= (uint32_t)*p++ << 8; [[fallthrough]];
    case 1: tail |= *p++; break;
    default: break;
    }
    return c ^ tail;
}

static uint32_t block_checksum(const uint8_t *data, uint16_t cb_data, uint16_t cb_uncomp) {
    uint8_t sizes[4] = { (uint8_t)cb_data, (uint8_t)(cb_data >> 8), (uint8_t)cb_uncomp, (uint8_t)(cb_uncomp >> 8) };
    return checksum(sizes, 4, checksum(data, cb_data, 0));
}

proven_err_t rp_cab_write(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress,
                          const rp_limits_t *limits, uint8_t **out, size_t *len) {
    if (compress < -1 || compress > 9) return PROVEN_ERR_INVALID_ARG;
    if ((files == NULL && count != 0) || limits == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    if (count > 0xFFFF) return PROVEN_ERR_OUT_OF_BOUNDS;
    uint64_t total = 0, names = 0;
    for (size_t k = 0; k < count; ++k) {
        const char *n = files[k].name;
        size_t nl = n ? strlen(n) : 0;
        if (nl == 0 || nl > 255 || (files[k].size && files[k].data == NULL)) return PROVEN_ERR_INVALID_ARG;
        for (size_t j = 0; j < nl; ++j) {
            if ((uint8_t)n[j] < 0x20 || (uint8_t)n[j] >= 0x7F) return PROVEN_ERR_INVALID_ARG;   // ASCII names only
        }
        total += files[k].size;
        names += nl + 1;
    }
    uint64_t blocks = (total + BLOCK - 1) / BLOCK;
    if (blocks > 0xFFFF) return PROVEN_ERR_OUT_OF_BOUNDS;       // one folder holds at most 65535 blocks
    uint64_t files_off = HEADER + FOLDER;
    uint64_t data_off = files_off + 16 * (uint64_t)count + names;
    // Worst case per MSZIP block: "CK" + a stored deflate block (5 bytes of header).
    uint64_t cab_len = data_off + 8 * blocks + total + (compress >= 0 ? 7 * blocks : 0);
    if (cab_len > UINT32_MAX || cab_len > limits->max_output) return PROVEN_ERR_OUT_OF_BOUNDS;

    rp_buf_t b = rp_buf_new(alloc, (size_t)cab_len);
    rp_buf_put(&b, "MSCF", 4);
    rp_buf_u32le(&b, 0);
    size_t cab_len_at = b.len;
    rp_buf_u32le(&b, 0);                    // cabinet size, patched below
    rp_buf_u32le(&b, 0);
    rp_buf_u32le(&b, (uint32_t)files_off);
    rp_buf_u32le(&b, 0);
    rp_buf_byte(&b, 3);                     // version 1.3
    rp_buf_byte(&b, 1);
    rp_buf_u16le(&b, 1);                    // folders
    rp_buf_u16le(&b, (uint16_t)count);
    rp_buf_u16le(&b, 0);                    // flags
    rp_buf_u16le(&b, 0);                    // set id
    rp_buf_u16le(&b, 0);                    // cabinet number
    rp_buf_u32le(&b, (uint32_t)data_off);   // CFFOLDER
    rp_buf_u16le(&b, (uint16_t)blocks);
    rp_buf_u16le(&b, compress >= 0 ? TYPE_MSZIP : TYPE_NONE);
    uint64_t at = 0;
    for (size_t k = 0; k < count; ++k) {    // CFFILE
        rp_buf_u32le(&b, (uint32_t)files[k].size);
        rp_buf_u32le(&b, (uint32_t)at);
        rp_buf_u16le(&b, 0);                // folder 0
        rp_buf_u16le(&b, DATE_1980_01_01);
        rp_buf_u16le(&b, 0);                // 00:00:00
        rp_buf_u16le(&b, ATTR_ARCH);
        rp_buf_put(&b, files[k].name, strlen(files[k].name) + 1);
        at += files[k].size;
    }
    // CFDATA: the folder's bytes (all files back to back) in 32 KiB blocks.
    uint8_t *chunk = rp_mem_alloc(alloc, BLOCK, 1);
    if (chunk == NULL) {
        rp_buf_free(&b);
        return PROVEN_ERR_NOMEM;
    }
    size_t file = 0, file_off = 0;
    for (uint64_t blk = 0; blk < blocks; ++blk) {
        size_t fill = 0;
        while (fill < BLOCK && file < count) {
            size_t left = files[file].size - file_off;
            size_t take = left < BLOCK - fill ? left : BLOCK - fill;
            if (take) memcpy(chunk + fill, files[file].data + file_off, take);
            fill += take;
            file_off += take;
            if (file_off == files[file].size) {
                ++file;
                file_off = 0;
            }
        }
        if (compress < 0) {
            rp_buf_u32le(&b, block_checksum(chunk, (uint16_t)fill, (uint16_t)fill));
            rp_buf_u16le(&b, (uint16_t)fill);
            rp_buf_u16le(&b, (uint16_t)fill);
            rp_buf_put(&b, chunk, fill);
            continue;
        }
        uint8_t *z = NULL;
        size_t zn = 0;
        proven_err_t err = rp_deflate(alloc, chunk, fill, compress, &z, &zn);
        if (err != PROVEN_OK || zn + 2 > 0xFFFF) {
            rp_mem_free(alloc, z);
            rp_mem_free(alloc, chunk);
            rp_buf_free(&b);
            return err != PROVEN_OK ? err : PROVEN_ERR_OUT_OF_BOUNDS;
        }
        uint8_t *blk = rp_mem_alloc(alloc, zn + 2, 1);
        if (blk == NULL) {
            rp_mem_free(alloc, z);
            rp_mem_free(alloc, chunk);
            rp_buf_free(&b);
            return PROVEN_ERR_NOMEM;
        }
        blk[0] = 'C';
        blk[1] = 'K';
        memcpy(blk + 2, z, zn);
        rp_buf_u32le(&b, block_checksum(blk, (uint16_t)(zn + 2), (uint16_t)fill));
        rp_buf_u16le(&b, (uint16_t)(zn + 2));
        rp_buf_u16le(&b, (uint16_t)fill);
        rp_buf_put(&b, blk, zn + 2);
        rp_mem_free(alloc, blk);
        rp_mem_free(alloc, z);
    }
    rp_mem_free(alloc, chunk);
    if (b.err == PROVEN_OK) {
        uint32_t total_len = (uint32_t)b.len;
        for (int k = 0; k < 4; ++k) b.data[cab_len_at + (size_t)k] = (uint8_t)(total_len >> (8 * k));
    }
    return rp_buf_take(&b, out, len);
}

proven_err_t rp_cab_read(proven_allocator_t alloc, const uint8_t *cab, size_t len, const rp_limits_t *limits,
                         rp_cab_file_t **files, size_t *count, uint8_t **arena) {
    if (cab == NULL || limits == NULL || files == NULL || count == NULL || arena == NULL) return PROVEN_ERR_INVALID_ARG;
    if (len < HEADER || memcmp(cab, "MSCF", 4) != 0 || rd32(cab + 8) != len) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t files_off = rd32(cab + 16);
    uint16_t nfolders = rd16(cab + 26), nfiles = rd16(cab + 28), flags = rd16(cab + 30);
    if (flags != 0 || nfolders == 0 || nfiles > limits->max_entries) return PROVEN_ERR_UNSUPPORTED;
    if ((uint64_t)HEADER + (uint64_t)FOLDER * nfolders > len) return PROVEN_ERR_INVALID_FORMAT;

    // Uncompressed folder sizes first, to allocate one arena for all data.
    uint64_t total = 0;
    for (uint16_t f = 0; f < nfolders; ++f) {
        const uint8_t *fo = cab + HEADER + FOLDER * f;
        uint32_t off = rd32(fo);
        uint16_t nblocks = rd16(fo + 4), type = rd16(fo + 6);
        if (type != TYPE_NONE && type != TYPE_MSZIP) return PROVEN_ERR_UNSUPPORTED;
        for (uint16_t k = 0; k < nblocks; ++k) {
            if (!rp_range_ok(off, 8, len)) return PROVEN_ERR_INVALID_FORMAT;
            uint16_t cbd = rd16(cab + off + 4), cbu = rd16(cab + off + 6);
            if ((type == TYPE_NONE && cbd != cbu) || cbu > BLOCK || !rp_range_ok((uint64_t)off + 8, cbd, len)) {
                return PROVEN_ERR_INVALID_FORMAT;
            }
            if (rd32(cab + off) != 0 && rd32(cab + off) != block_checksum(cab + off + 8, cbd, cbu)) {
                return PROVEN_ERR_INVALID_FORMAT;
            }
            total += cbu;
            off += 8u + cbd;
        }
    }
    if (total > limits->max_output) return PROVEN_ERR_OUT_OF_BOUNDS;
    uint8_t *buf = rp_mem_alloc(alloc, (size_t)total, 1);
    size_t *folder_base = rp_mem_alloc(alloc, nfolders, sizeof *folder_base);
    size_t *folder_len = rp_mem_alloc(alloc, nfolders, sizeof *folder_len);
    rp_cab_file_t *list = rp_mem_alloc(alloc, nfiles, sizeof *list);
    if (buf == NULL || folder_base == NULL || folder_len == NULL || list == NULL) {
        rp_mem_free(alloc, buf);
        rp_mem_free(alloc, folder_base);
        rp_mem_free(alloc, folder_len);
        rp_mem_free(alloc, list);
        return PROVEN_ERR_NOMEM;
    }
    size_t at = 0;
    proven_err_t err = PROVEN_OK;
    for (uint16_t f = 0; f < nfolders && err == PROVEN_OK; ++f) {
        const uint8_t *fo = cab + HEADER + FOLDER * f;
        uint32_t off = rd32(fo);
        uint16_t type = rd16(fo + 6);
        folder_base[f] = at;
        for (uint16_t k = 0; k < rd16(fo + 4) && err == PROVEN_OK; ++k) {
            uint16_t cbd = rd16(cab + off + 4), cbu = rd16(cab + off + 6);
            const uint8_t *data = cab + off + 8;
            if (type == TYPE_NONE) {
                memcpy(buf + at, data, cbd);
                at += cbd;
            } else {
                // MSZIP: "CK", then a deflate stream that may reach back into this folder's
                // earlier blocks (history starts at the folder's first byte).
                size_t pos = at - folder_base[f], used = 0;
                if (cbd < 2 || data[0] != 'C' || data[1] != 'K' ||
                    rp_inflate(data + 2, cbd - 2u, buf + folder_base[f], pos + cbu, &pos, &used) != PROVEN_OK ||
                    pos != at - folder_base[f] + cbu) {
                    err = PROVEN_ERR_INVALID_FORMAT;
                    break;
                }
                at += cbu;
            }
            off += 8u + cbd;
        }
        folder_len[f] = at - folder_base[f];
    }
    size_t off = files_off;
    for (uint16_t k = 0; k < nfiles && err == PROVEN_OK; ++k) {
        if (!rp_range_ok(off, 17, len)) {
            err = PROVEN_ERR_INVALID_FORMAT;
            break;
        }
        uint32_t size = rd32(cab + off), start = rd32(cab + off + 4);
        uint16_t folder = rd16(cab + off + 8);
        const char *name = (const char *)cab + off + 16;
        const void *nul = memchr(name, 0, len - off - 16);
        if (nul == NULL || folder >= nfolders || !rp_range_ok(start, size, folder_len[folder])) {
            err = PROVEN_ERR_INVALID_FORMAT;
            break;
        }
        list[k] = (rp_cab_file_t){ name, buf + folder_base[folder] + start, size };
        off = (size_t)((const char *)nul - (const char *)cab) + 1;
    }
    rp_mem_free(alloc, folder_base);
    rp_mem_free(alloc, folder_len);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, buf);
        rp_mem_free(alloc, list);
        return err;
    }
    *files = list;
    *count = nfiles;
    *arena = buf;
    return PROVEN_OK;
}
