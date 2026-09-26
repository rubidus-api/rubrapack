#ifndef RUBRAPACK_MD5_H
#define RUBRAPACK_MD5_H

// include/rubrapack/md5.h - MD5 (RFC 1321). Used only where Windows Installer requires it: the
// MsiFileHash table of unversioned files (RFC-0001 F8). Not a security primitive here.

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t length;
    uint8_t  block[64];
    size_t   used;
} rp_md5_t;

void rp_md5_init(rp_md5_t *c);
void rp_md5_update(rp_md5_t *c, const void *data, size_t len);
void rp_md5_final(rp_md5_t *c, uint8_t out[16]);
void rp_md5(const void *data, size_t len, uint8_t out[16]);

#endif // RUBRAPACK_MD5_H
