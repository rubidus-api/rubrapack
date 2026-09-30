// src/container/cab.c - cabinet writer and reader (include/rubrapack/cab.h, MS-CAB).

#include "rubrapack/buf.h"
#include "rubrapack/cab.h"
#include "rubrapack/deflate.h"
#include "rubrapack/lzx.h"
#include "rubrapack/mem.h"
#include "rubrapack/pal.h"

#include <string.h>

enum {
    HEADER = 36,
    FOLDER = 8,
    BLOCK = 32768,          // uncompressed bytes per CFDATA block
    TYPE_NONE = 0,
    TYPE_MSZIP = 1,
    TYPE_LZX = 3,           // | window bits << 8
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

// One MSZIP block to compress (RFC-0013 E2/E3): its raw bytes, and the previous block's raw bytes
// of the same folder as the dictionary (NULL for a folder's first block).
typedef struct {
    proven_allocator_t alloc;
    int                level;
    const uint8_t     *raw, *dict;
    size_t             rn, dn;
    uint8_t           *z;
    size_t             zn;
    proven_err_t       err;
} zjob_t;

static void zjob_run(void *ctx, size_t i) {
    zjob_t *j = &((zjob_t *)ctx)[i];
    j->err = rp_deflate_dict(j->alloc, j->dict, j->dn, j->raw, j->rn, j->level, &j->z, &j->zn);
}

enum { MAX_FOLDERS = 0xFFFF };
size_t rp_cab_folder_blocks = 0xFFFF;       // blocks per folder at most (a test may lower it)

proven_err_t rp_cab_write_ex(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress, size_t jobs,
                             const rp_limits_t *limits, uint8_t **out, size_t *len) {
    bool lzx = compress >= RP_CAB_LZX(RP_LZX_MIN_WBITS) && compress <= RP_CAB_LZX(RP_LZX_MAX_WBITS);
    if ((compress < -1 || compress > 9) && !lzx) return PROVEN_ERR_INVALID_ARG;
    if ((files == NULL && count != 0) || limits == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    if (count > 0xFFFF) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (jobs == 0) jobs = 1;
    if (jobs > 64) jobs = 64;
    if (rp_cab_folder_blocks == 0 || rp_cab_folder_blocks > 0xFFFF) rp_cab_folder_blocks = 0xFFFF;
    // Folders (RFC-0013 E4): a new one starts at a file boundary before 65,535 blocks.
    uint16_t *folder_of = rp_mem_alloc(alloc, count + 1, sizeof *folder_of);
    uint64_t *fbytes = rp_mem_alloc(alloc, count + 1, sizeof *fbytes);      // uncompressed bytes per folder
    if (folder_of == NULL || fbytes == NULL) {
        rp_mem_free(alloc, folder_of);
        rp_mem_free(alloc, fbytes);
        return PROVEN_ERR_NOMEM;
    }
    uint64_t names = 0, blocks = 0, total = 0;
    size_t nfold = 1;
    fbytes[0] = 0;
    for (size_t k = 0; k < count; ++k) {
        const char *n = files[k].name;
        size_t nl = n ? strlen(n) : 0;
        bool bad = nl == 0 || nl > 255 || (files[k].size && files[k].data == NULL) ||
                   files[k].size > (uint64_t)rp_cab_folder_blocks * BLOCK;
        for (size_t j = 0; !bad && j < nl; ++j) bad = (uint8_t)n[j] < 0x20 || (uint8_t)n[j] >= 0x7F;     // ASCII names only
        if (bad) {
            rp_mem_free(alloc, folder_of);
            rp_mem_free(alloc, fbytes);
            return PROVEN_ERR_INVALID_ARG;
        }
        if (fbytes[nfold - 1] > 0 && fbytes[nfold - 1] + files[k].size > (uint64_t)rp_cab_folder_blocks * BLOCK) fbytes[nfold++] = 0;
        folder_of[k] = (uint16_t)(nfold - 1);
        fbytes[nfold - 1] += files[k].size;
        total += files[k].size;
        names += nl + 1;
    }
    for (size_t f = 0; f < nfold; ++f) blocks += (fbytes[f] + BLOCK - 1) / BLOCK;
    uint64_t files_off = HEADER + (uint64_t)FOLDER * nfold;
    uint64_t data_off = files_off + 16 * (uint64_t)count + names;
    // Worst case per MSZIP block: "CK" + a stored deflate block (5 bytes of header).
    // LZX: at most about 9.4 bits a byte (entropy of 656 symbols + 1) and the trees.
    uint64_t cab_len = data_off + 8 * blocks + total + (lzx ? (BLOCK / 4 + 2048) * blocks : compress >= 0 ? 7 * blocks : 0);
    if (nfold > MAX_FOLDERS || cab_len > UINT32_MAX || cab_len > limits->max_output) {
        rp_mem_free(alloc, folder_of);
        rp_mem_free(alloc, fbytes);
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    rp_buf_t b = rp_buf_new(alloc, (size_t)cab_len);       // a limit (the worst case), not a size
    rp_buf_put(&b, "MSCF", 4);
    rp_buf_u32le(&b, 0);
    size_t cab_len_at = b.len;
    rp_buf_u32le(&b, 0);                    // cabinet size, patched below
    rp_buf_u32le(&b, 0);
    rp_buf_u32le(&b, (uint32_t)files_off);
    rp_buf_u32le(&b, 0);
    rp_buf_byte(&b, 3);                     // version 1.3
    rp_buf_byte(&b, 1);
    rp_buf_u16le(&b, (uint16_t)nfold);      // folders
    rp_buf_u16le(&b, (uint16_t)count);
    rp_buf_u16le(&b, 0);                    // flags
    rp_buf_u16le(&b, 0);                    // set id
    rp_buf_u16le(&b, 0);                    // cabinet number
    size_t folder_at = b.len;
    for (size_t f = 0; f < nfold; ++f) {    // CFFOLDER: first CFDATA patched below
        rp_buf_u32le(&b, 0);
        rp_buf_u16le(&b, (uint16_t)((fbytes[f] + BLOCK - 1) / BLOCK));
        rp_buf_u16le(&b, lzx ? (uint16_t)(TYPE_LZX | (compress - RP_CAB_LZX(0)) << 8) : compress >= 0 ? TYPE_MSZIP : TYPE_NONE);
    }
    uint64_t at = 0;
    for (size_t k = 0; k < count; ++k) {    // CFFILE
        if (k > 0 && folder_of[k] != folder_of[k - 1]) at = 0;
        rp_buf_u32le(&b, (uint32_t)files[k].size);
        rp_buf_u32le(&b, (uint32_t)at);
        rp_buf_u16le(&b, folder_of[k]);
        rp_buf_u16le(&b, DATE_1980_01_01);
        rp_buf_u16le(&b, 0);                // 00:00:00
        rp_buf_u16le(&b, ATTR_ARCH);
        rp_buf_put(&b, files[k].name, strlen(files[k].name) + 1);
        at += files[k].size;
    }
    // CFDATA: each folder's bytes (its files back to back) in 32 KiB blocks, compressed a batch at a
    // time on `jobs` threads and written in order; a block's dictionary is the previous block of
    // its folder.
    size_t batch = compress >= 0 && !lzx ? jobs * 4 : 1;
    uint8_t *raw = rp_mem_alloc(alloc, (batch + 1) * BLOCK, 1);        // [previous block][batch blocks]
    zjob_t *zj = rp_mem_alloc(alloc, batch, sizeof *zj);
    proven_err_t err = raw && zj ? PROVEN_OK : PROVEN_ERR_NOMEM;
    size_t file = 0, file_off = 0;
    rp_lzx_enc_t *enc = NULL;
    for (size_t f = 0; err == PROVEN_OK && f < nfold; ++f) {
        if (lzx) {                          // LZX: one encoder per folder, its frames in order
            rp_lzx_enc_free(enc);
            enc = NULL;
            err = rp_lzx_enc_new(alloc, (unsigned)(compress - RP_CAB_LZX(0)), &enc);
            if (err != PROVEN_OK) break;
        }
        if (b.err == PROVEN_OK) {
            uint32_t v = (uint32_t)b.len;
            for (int k = 0; k < 4; ++k) b.data[folder_at + FOLDER * f + (size_t)k] = (uint8_t)(v >> (8 * k));
        }
        uint64_t fblocks = (fbytes[f] + BLOCK - 1) / BLOCK;
        size_t prev_len = 0;                // bytes of the previous block in raw[0..BLOCK)
        for (uint64_t b0 = 0; err == PROVEN_OK && b0 < fblocks; b0 += batch) {
            size_t nb = fblocks - b0 < batch ? (size_t)(fblocks - b0) : batch;
            size_t fill_of[64 * 4 + 1];
            for (size_t j = 0; j < nb; ++j) {
                uint8_t *chunk = raw + (j + 1) * BLOCK;
                size_t fill = 0;
                while (fill < BLOCK && file < count && folder_of[file] == f) {
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
                fill_of[j] = fill;
            }
            // skip files of this folder that are empty (no bytes to place)
            while (file < count && folder_of[file] == f && files[file].size == 0) ++file;
            if (lzx) {
                for (size_t j = 0; j < nb && err == PROVEN_OK; ++j) {
                    const uint8_t *z = NULL;
                    size_t zn = 0;
                    err = rp_lzx_enc_frame(enc, raw + (j + 1) * BLOCK, fill_of[j], &z, &zn);
                    if (err == PROVEN_OK && zn > 0xFFFF) err = PROVEN_ERR_OUT_OF_BOUNDS;
                    if (err != PROVEN_OK) break;
                    rp_buf_u32le(&b, block_checksum(z, (uint16_t)zn, (uint16_t)fill_of[j]));
                    rp_buf_u16le(&b, (uint16_t)zn);
                    rp_buf_u16le(&b, (uint16_t)fill_of[j]);
                    rp_buf_put(&b, z, zn);
                }
                continue;
            }
            if (compress < 0) {
                for (size_t j = 0; j < nb; ++j) {
                    const uint8_t *chunk = raw + (j + 1) * BLOCK;
                    rp_buf_u32le(&b, block_checksum(chunk, (uint16_t)fill_of[j], (uint16_t)fill_of[j]));
                    rp_buf_u16le(&b, (uint16_t)fill_of[j]);
                    rp_buf_u16le(&b, (uint16_t)fill_of[j]);
                    rp_buf_put(&b, chunk, fill_of[j]);
                }
                continue;
            }
            for (size_t j = 0; j < nb; ++j) {
                bool first = b0 == 0 && j == 0;
                zj[j] = (zjob_t){ .alloc = alloc, .level = compress, .raw = raw + (j + 1) * BLOCK, .rn = fill_of[j],
                                  .dict = first ? NULL : raw + j * BLOCK, .dn = first ? 0 : (j ? fill_of[j - 1] : prev_len) };
            }
            rp_pal_parallel_for(jobs, nb, zjob_run, zj);
            for (size_t j = 0; j < nb; ++j) {
                if (err == PROVEN_OK && (zj[j].err != PROVEN_OK || zj[j].zn + 2 > 0xFFFF)) err = zj[j].err != PROVEN_OK ? zj[j].err : PROVEN_ERR_OUT_OF_BOUNDS;
                if (err == PROVEN_OK) {
                    uint8_t *blk = rp_mem_alloc(alloc, zj[j].zn + 2, 1);
                    if (blk == NULL) {
                        err = PROVEN_ERR_NOMEM;
                    } else {
                        blk[0] = 'C';
                        blk[1] = 'K';
                        memcpy(blk + 2, zj[j].z, zj[j].zn);
                        rp_buf_u32le(&b, block_checksum(blk, (uint16_t)(zj[j].zn + 2), (uint16_t)fill_of[j]));
                        rp_buf_u16le(&b, (uint16_t)(zj[j].zn + 2));
                        rp_buf_u16le(&b, (uint16_t)fill_of[j]);
                        rp_buf_put(&b, blk, zj[j].zn + 2);
                        rp_mem_free(alloc, blk);
                    }
                }
                rp_mem_free(alloc, zj[j].z);
            }
            // The batch's last block is the next batch's first dictionary.
            memmove(raw, raw + nb * BLOCK, fill_of[nb - 1]);
            prev_len = fill_of[nb - 1];
        }
    }
    rp_lzx_enc_free(enc);
    rp_mem_free(alloc, raw);
    rp_mem_free(alloc, zj);
    rp_mem_free(alloc, folder_of);
    rp_mem_free(alloc, fbytes);
    if (err != PROVEN_OK) {
        rp_buf_free(&b);
        return err;
    }
    if (b.err == PROVEN_OK) {
        uint32_t total_len = (uint32_t)b.len;
        for (int k = 0; k < 4; ++k) b.data[cab_len_at + (size_t)k] = (uint8_t)(total_len >> (8 * k));
    }
    return rp_buf_take(&b, out, len);
}

proven_err_t rp_cab_write(proven_allocator_t alloc, const rp_cab_file_t *files, size_t count, int compress,
                          const rp_limits_t *limits, uint8_t **out, size_t *len) {
    return rp_cab_write_ex(alloc, files, count, compress, 1, limits, out, len);
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
        unsigned wbits = type >> 8 & 0x1F;
        if (type != TYPE_NONE && type != TYPE_MSZIP &&
            ((type & 0xFF) != TYPE_LZX || wbits < RP_LZX_MIN_WBITS || wbits > RP_LZX_MAX_WBITS)) {
            return PROVEN_ERR_UNSUPPORTED;
        }
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
        rp_lzx_dec_t *dec = NULL;
        if ((type & 0xFF) == TYPE_LZX) err = rp_lzx_dec_new(alloc, type >> 8 & 0x1F, &dec);
        for (uint16_t k = 0; k < rd16(fo + 4) && err == PROVEN_OK; ++k) {
            uint16_t cbd = rd16(cab + off + 4), cbu = rd16(cab + off + 6);
            const uint8_t *data = cab + off + 8;
            if (dec) {
                if (cbu == 0 || rp_lzx_dec_frame(dec, data, cbd, buf + at, cbu) != PROVEN_OK) {
                    err = PROVEN_ERR_INVALID_FORMAT;
                    break;
                }
                at += cbu;
            } else if (type == TYPE_NONE) {
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
        rp_lzx_dec_free(dec);
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
