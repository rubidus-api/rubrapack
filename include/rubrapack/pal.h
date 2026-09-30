#ifndef RUBRAPACK_PAL_H
#define RUBRAPACK_PAL_H

// include/rubrapack/pal.h
//
// Platform layer. The rest of rubrapack sees UTF-8 only; each PAL converts at its own
// boundary (RFC-0001 section 15.4):
//   Windows  wmain + UTF-16 argv -> UTF-8; a console gets WriteConsoleW (UTF-16), a pipe or
//            file gets the UTF-8 bytes unchanged.
//   POSIX    argv and output are UTF-8 bytes as they are.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"

typedef enum {
    RP_OUT_STDOUT,
    RP_OUT_STDERR,
} rp_out_t;

// Writes UTF-8 text to stdout or stderr. The text must be valid UTF-8.
[[nodiscard]] proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len);

// Writes a NUL-terminated UTF-8 string.
[[nodiscard]] proven_err_t rp_pal_puts(rp_out_t out, const char *utf8);

// Reads one line from standard input as UTF-8 (NUL-terminated, without the line end; free with
// rp_mem_free). A Windows console is read as UTF-16 (ReadConsoleW) so any script can be typed; a
// pipe or a file is taken as UTF-8 bytes. PROVEN_ERR_NOT_FOUND at the end of the input,
// PROVEN_ERR_INVALID_ENCODING for bytes that are not UTF-8, PROVEN_ERR_OUT_OF_BOUNDS past max_bytes.
[[nodiscard]] proven_err_t rp_pal_read_line(proven_allocator_t alloc, size_t max_bytes, char **line);

// Reads a whole file named by a UTF-8 path into memory from `alloc` (free with rp_mem_free).
// A file larger than max_bytes is refused with PROVEN_ERR_OUT_OF_BOUNDS before it is read.
[[nodiscard]] proven_err_t rp_pal_read_file(proven_allocator_t alloc, const char *path_utf8,
                                            size_t max_bytes, uint8_t **data, size_t *len);

typedef enum {
    RP_FS_NONE,         // does not exist (or cannot be seen)
    RP_FS_FILE,
    RP_FS_DIR,
    RP_FS_LINK,         // symlink or reparse point - never followed (RFC-0002 8.1)
    RP_FS_OTHER,
} rp_fskind_t;

// What a UTF-8 path names, without following links. *size is set for files.
[[nodiscard]] rp_fskind_t rp_pal_stat(proven_allocator_t alloc, const char *path_utf8, uint64_t *size);

// Lists the entries of a directory (without "." and ".."), UTF-8, in no particular order.
// Free each name and the array with rp_mem_free. A name that is not valid Unicode makes the
// whole listing fail with PROVEN_ERR_INVALID_ENCODING.
[[nodiscard]] proven_err_t rp_pal_list_dir(proven_allocator_t alloc, const char *path_utf8, char ***names, size_t *count);

// True when both paths exist and name the same file (same device and file identity).
[[nodiscard]] bool rp_pal_same_file(proven_allocator_t alloc, const char *a_utf8, const char *b_utf8);

// Writes `data` to a temporary file next to `path_utf8`, then renames it over `path_utf8`, so a
// failure never leaves a half-written output (RFC-0001 7.1).
[[nodiscard]] proven_err_t rp_pal_write_file_atomic(proven_allocator_t alloc, const char *path_utf8,
                                                    const uint8_t *data, size_t len);

// Removes a file (not a directory). PROVEN_ERR_NOT_FOUND when there is none.
[[nodiscard]] proven_err_t rp_pal_remove_file(proven_allocator_t alloc, const char *path_utf8);

// Creates a new directory. PROVEN_ERR_BUSY when the name is already taken (by anything): an
// extractor never reuses what was there (RFC-0001 14.3).
[[nodiscard]] proven_err_t rp_pal_mkdir_new(proven_allocator_t alloc, const char *path_utf8);

// Creates a new file and writes `data`. PROVEN_ERR_BUSY when the name is already taken - an
// existing file, directory or link is never opened, followed or replaced. A failed write
// removes the new file.
[[nodiscard]] proven_err_t rp_pal_write_file_new(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data,
                                                 size_t len);

// An environment variable as UTF-8 (NULL when unset); free with rp_mem_free.
[[nodiscard]] char *rp_pal_getenv(proven_allocator_t alloc, const char *name);

// ---- network (RFC-0008) -------------------------------------------------------------------------

typedef struct rp_sock rp_sock_t;

// Connects over TCP to host:port (resolved by the OS, IPv4 or IPv6) within timeout_ms.
// PROVEN_ERR_NOT_FOUND: the name did not resolve; PROVEN_ERR_AGAIN: timed out; PROVEN_ERR_IO: refused.
[[nodiscard]] proven_err_t rp_pal_tcp_connect(proven_allocator_t alloc, const char *host, uint16_t port, int timeout_ms, rp_sock_t **out);
// Sends all of `data` within timeout_ms.
[[nodiscard]] proven_err_t rp_pal_tcp_send(rp_sock_t *s, const uint8_t *data, size_t len, int timeout_ms);
// Receives up to `cap` bytes within timeout_ms; *got = 0 when the peer closed.
[[nodiscard]] proven_err_t rp_pal_tcp_recv(rp_sock_t *s, uint8_t *buf, size_t cap, size_t *got, int timeout_ms);
void rp_pal_tcp_close(rp_sock_t *s);
// Milliseconds from a monotonic clock.
[[nodiscard]] int64_t rp_pal_now_ms(void);

// The operating system's trusted root certificates, handed to `sink` as data: on Windows each
// certificate of the local machine's and user's ROOT store (DER), elsewhere the CA bundle file
// ($SSL_CERT_FILE, or the first of the usual paths: PEM). PROVEN_ERR_NOT_FOUND when there is none.
[[nodiscard]] proven_err_t rp_pal_system_roots(proven_allocator_t alloc, void (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx);

// A file mapped read-only into memory (RFC-0013 E1): the pages come from the file as they are
// read and cost no heap. *data is NULL for an empty file. Unmap with rp_pal_unmap.
typedef struct rp_map rp_map_t;
[[nodiscard]] proven_err_t rp_pal_map_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes,
                                           const uint8_t **data, size_t *len, rp_map_t **map);
void rp_pal_unmap(proven_allocator_t alloc, rp_map_t *map);

// An output file written through a writable mapping (RFC-0013 R2b), so a large package costs no
// heap: <path>.rp-map is made `len` bytes long (zeros, its space reserved) and mapped at *data.
// Commit flushes it and renames it to <path>; discard removes it. Both free the handle.
typedef struct rp_outmap rp_outmap_t;
[[nodiscard]] proven_err_t rp_pal_outmap_create(proven_allocator_t alloc, const char *path_utf8, size_t len, uint8_t **data,
                                                rp_outmap_t **om);
[[nodiscard]] proven_err_t rp_pal_outmap_commit(proven_allocator_t alloc, rp_outmap_t *om);
void rp_pal_outmap_discard(proven_allocator_t alloc, rp_outmap_t *om);

// Processors this process may use (at least 1).
[[nodiscard]] size_t rp_pal_cpu_count(void);

// Runs fn(ctx, i) for every i in [0, count) on up to `jobs` threads (1: in this thread, in order).
// fn must only touch what belongs to its own i. Returns when all are done (RFC-0013 E2).
void rp_pal_parallel_for(size_t jobs, size_t count, void (*fn)(void *ctx, size_t i), void *ctx);

#endif // RUBRAPACK_PAL_H
