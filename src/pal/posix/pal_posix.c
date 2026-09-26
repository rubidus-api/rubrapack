// src/pal/posix/pal_posix.c - POSIX: output and paths are UTF-8 bytes as they are.

#define _POSIX_C_SOURCE 200809L

#include "rubrapack/mem.h"
#include "rubrapack/pal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len) {
    FILE *f = (out == RP_OUT_STDERR) ? stderr : stdout;
    if (utf8 == NULL && len != 0) return PROVEN_ERR_INVALID_ARG;
    if (len != 0 && fwrite(utf8, 1, len, f) != len) return PROVEN_ERR_IO;
    if (fflush(f) != 0) return PROVEN_ERR_IO;
    return PROVEN_OK;
}

proven_err_t rp_pal_puts(rp_out_t out, const char *utf8) {
    if (utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    return rp_pal_write(out, (const uint8_t *)utf8, strlen(utf8));
}

proven_err_t rp_pal_read_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes,
                              uint8_t **data, size_t *len) {
    if (path_utf8 == NULL || data == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    FILE *f = fopen(path_utf8, "rb");
    if (f == NULL) return PROVEN_ERR_NOT_FOUND;
    proven_err_t err = PROVEN_OK;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) err = PROVEN_ERR_IO;
    else if ((unsigned long)size > max_bytes) err = PROVEN_ERR_OUT_OF_BOUNDS;
    uint8_t *buf = NULL;
    if (err == PROVEN_OK) {
        buf = rp_mem_alloc(alloc, (size_t)size, 1);
        if (buf == NULL) err = PROVEN_ERR_NOMEM;
        else if (fread(buf, 1, (size_t)size, f) != (size_t)size) err = PROVEN_ERR_IO;
    }
    fclose(f);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, buf);
        return err;
    }
    *data = buf;
    *len = (size_t)size;
    return PROVEN_OK;
}

rp_fskind_t rp_pal_stat(proven_allocator_t alloc, const char *path_utf8, uint64_t *size) {
    (void)alloc;
    struct stat st;
    if (path_utf8 == NULL || lstat(path_utf8, &st) != 0) return RP_FS_NONE;
    if (S_ISLNK(st.st_mode)) return RP_FS_LINK;
    if (S_ISDIR(st.st_mode)) return RP_FS_DIR;
    if (!S_ISREG(st.st_mode)) return RP_FS_OTHER;
    if (size) *size = (uint64_t)st.st_size;
    return RP_FS_FILE;
}

proven_err_t rp_pal_write_file_atomic(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    size_t n = strlen(path_utf8);
    char *tmp = rp_mem_alloc(alloc, n + 16, 1);
    if (tmp == NULL) return PROVEN_ERR_NOMEM;
    snprintf(tmp, n + 16, "%s.rp-tmp", path_utf8);
    FILE *f = fopen(tmp, "wb");
    proven_err_t err = PROVEN_OK;
    if (f == NULL) {
        err = PROVEN_ERR_IO;
    } else {
        if (len && fwrite(data, 1, len, f) != len) err = PROVEN_ERR_IO;
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) err = PROVEN_ERR_IO;
        if (fclose(f) != 0) err = PROVEN_ERR_IO;
        if (err == PROVEN_OK && rename(tmp, path_utf8) != 0) err = PROVEN_ERR_IO;
        if (err != PROVEN_OK) remove(tmp);
    }
    rp_mem_free(alloc, tmp);
    return err;
}
