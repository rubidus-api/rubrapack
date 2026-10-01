// src/pal/pal_fs.c - the file side of the platform layer (include/rubrapack/pal.h) on
// proven_c_lib's file system and mapping calls (proven/fs.h, proven/mmap.h): one source for POSIX
// and Windows, where proven converts UTF-8 paths (long ones with the \\?\ prefix) itself.
// pal_posix.c and pal_win32.c keep what proven has no call for: a stat that does not follow
// links, file identity, the output mapping with its space reserved up front, the console,
// sockets and threads.

#include "rubrapack/pal.h"

#include "rubrapack/mem.h"

#include "proven/fs.h"
#include "proven/mmap.h"

#include <string.h>

static proven_u8str_view_t path_view(const char *path_utf8) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)path_utf8, .size = strlen(path_utf8) };
}

proven_err_t rp_pal_read_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes,
                              uint8_t **data, size_t *len) {
    if (path_utf8 == NULL || data == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t r = proven_fs_read_all_bounded(alloc, path_view(path_utf8), max_bytes);
    if (r.err != PROVEN_OK) return r.err;
    uint8_t *buf = r.value.ptr;
    if (buf == NULL) {          // an empty file: still a buffer to free, as callers expect
        buf = rp_mem_alloc(alloc, 1, 1);
        if (buf == NULL) return PROVEN_ERR_NOMEM;
    }
    *data = buf;
    *len = r.value.size;
    return PROVEN_OK;
}

proven_err_t rp_pal_write_file_atomic(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    // A temporary beside it, flushed to the disk, then renamed over the name (and the folder synced).
    return proven_fs_write_file_durable(alloc, path_view(path_utf8), (proven_mem_view_t){ data, len });
}

proven_err_t rp_pal_remove_file(proven_allocator_t alloc, const char *path_utf8) {
    if (path_utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    if (rp_pal_stat(alloc, path_utf8, NULL) == RP_FS_NONE) return PROVEN_ERR_NOT_FOUND;
    return proven_fs_remove(alloc, path_view(path_utf8));
}

proven_err_t rp_pal_mkdir_new(proven_allocator_t alloc, const char *path_utf8) {
    if (path_utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_err_t err = proven_fs_mkdir(alloc, path_view(path_utf8));
    // The creation itself refuses an existing name; the look afterwards only names the reason.
    if (err != PROVEN_OK && rp_pal_stat(alloc, path_utf8, NULL) != RP_FS_NONE) err = PROVEN_ERR_BUSY;
    return err;
}

proven_err_t rp_pal_write_file_new(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    // CREATE_NEW fails on any existing name, a link included (it is never followed).
    proven_result_file_t f = proven_fs_open(alloc, path_view(path_utf8), PROVEN_FS_WRITE | PROVEN_FS_CREATE_NEW);
    if (f.err != PROVEN_OK) return f.err == PROVEN_ERR_EXISTS ? PROVEN_ERR_BUSY : PROVEN_ERR_IO;
    proven_err_t err = len ? proven_fs_write_all(f.value, (proven_mem_view_t){ data, len }) : PROVEN_OK;
    if (proven_fs_close(f.value) != PROVEN_OK) err = PROVEN_ERR_IO;
    if (err != PROVEN_OK) {
        (void)proven_fs_remove(alloc, path_view(path_utf8));
        return PROVEN_ERR_IO;
    }
    return PROVEN_OK;
}

proven_err_t rp_pal_list_dir(proven_allocator_t alloc, const char *path_utf8, char ***names, size_t *count) {
    if (path_utf8 == NULL || names == NULL || count == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_result_dir_t d = proven_fs_dir_open(alloc, path_view(path_utf8));
    if (d.err != PROVEN_OK) return PROVEN_ERR_NOT_FOUND;
    char **v = NULL;
    size_t n = 0, cap = 0;
    proven_err_t err = PROVEN_OK;
    proven_fs_dir_entry_t e;
    proven_err_t step;
    while (err == PROVEN_OK && (step = proven_fs_dir_next(&d.value, &e)) == PROVEN_OK) {
        if ((e.name.size == 1 && e.name.ptr[0] == '.') || (e.name.size == 2 && e.name.ptr[0] == '.' && e.name.ptr[1] == '.')) continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char **nv = rp_mem_alloc(alloc, ncap, sizeof *nv);
            if (nv == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            if (n) memcpy(nv, v, n * sizeof *nv);
            rp_mem_free(alloc, v);
            v = nv;
            cap = ncap;
        }
        v[n] = rp_mem_alloc(alloc, e.name.size + 1, 1);
        if (v[n] == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        memcpy(v[n], e.name.ptr, e.name.size);
        v[n++][e.name.size] = '\0';
    }
    if (err == PROVEN_OK && step != PROVEN_ERR_EOF) err = PROVEN_ERR_IO;     // a failed read is not the end
    proven_fs_dir_close(&d.value);
    if (err != PROVEN_OK) {
        for (size_t i = 0; i < n; ++i) rp_mem_free(alloc, v[i]);
        rp_mem_free(alloc, v);
        return err;
    }
    *names = v;
    *count = n;
    return PROVEN_OK;
}

// ---- mapped files (RFC-0013 E1) ------------------------------------------------------------------

struct rp_map {
    proven_mmap_t m;
    bool          mapped;
};

proven_err_t rp_pal_map_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes, const uint8_t **data, size_t *len,
                             rp_map_t **map) {
    if (path_utf8 == NULL || data == NULL || len == NULL || map == NULL) return PROVEN_ERR_INVALID_ARG;
    *data = NULL;
    *len = 0;
    *map = NULL;
    if (rp_pal_stat(alloc, path_utf8, NULL) != RP_FS_FILE) {
        return rp_pal_stat(alloc, path_utf8, NULL) == RP_FS_NONE ? PROVEN_ERR_NOT_FOUND : PROVEN_ERR_IO;
    }
    proven_result_file_t f = proven_fs_open(alloc, path_view(path_utf8), PROVEN_FS_READ);
    if (f.err != PROVEN_OK) return PROVEN_ERR_NOT_FOUND;
    proven_result_size_t size = proven_fs_size(f.value);
    proven_err_t err = size.err != PROVEN_OK ? PROVEN_ERR_IO : size.value > max_bytes ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_OK;
    rp_map_t *m = err == PROVEN_OK ? rp_mem_alloc(alloc, 1, sizeof *m) : NULL;
    if (err == PROVEN_OK && m == NULL) err = PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK) {
        *m = (rp_map_t){ 0 };
        if (size.value) {           // an empty file maps to nothing
            proven_result_mmap_t r = proven_mmap_create(f.value, 0, size.value, PROVEN_MMAP_READ, PROVEN_MMAP_PRIVATE);
            if (r.err != PROVEN_OK) err = PROVEN_ERR_IO;
            else m->m = r.value, m->mapped = true;
        }
    }
    // The mapping holds the file's contents by itself; the handle is not needed any more.
    (void)proven_fs_close(f.value);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, m);
        return err;
    }
    *data = m->mapped ? m->m.ptr : NULL;
    *len = m->mapped ? m->m.size : 0;
    *map = m;
    return PROVEN_OK;
}

void rp_pal_unmap(proven_allocator_t alloc, rp_map_t *map) {
    if (map == NULL) return;
    if (map->mapped) (void)proven_mmap_destroy(&map->m);
    rp_mem_free(alloc, map);
}
