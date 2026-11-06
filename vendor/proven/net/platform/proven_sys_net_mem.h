#ifndef PROVEN_PLATFORM_SYS_NET_MEM_H
#define PROVEN_PLATFORM_SYS_NET_MEM_H

#include "proven/types.h"

/**
 * @file proven_sys_net_mem.h
 * @brief The network library's block copies, in its own platform layer (RFC-0043): it uses
 *        nothing of the base library's but its public headers, and these are on every hot path
 *        of the record and frame code, so they stay inline - the C library's where there is one,
 *        plain loops in the freestanding profile. As the base library's: a size of zero, or a
 *        null pointer with a size, does nothing.
 */

#ifdef PROVEN_FREESTANDING

static inline void proven_net_mem_copy(void *dst, const void *src, proven_size_t size) {
    if (size > 0 && dst && src) {
        char *d = (char *)dst;
        const char *s = (const char *)src;
        for (proven_size_t i = 0; i < size; ++i) d[i] = s[i];
    }
}

static inline void proven_net_mem_move(void *dst, const void *src, proven_size_t size) {
    if (size > 0 && dst && src && dst != src) {
        char *d = (char *)dst;
        const char *s = (const char *)src;
        if (d < s) {
            for (proven_size_t i = 0; i < size; ++i) d[i] = s[i];
        } else {
            for (proven_size_t i = size; i > 0; --i) d[i - 1] = s[i - 1];
        }
    }
}

static inline void proven_net_mem_set(void *dst, int value, proven_size_t size) {
    if (size > 0 && dst) {
        char *d = (char *)dst;
        for (proven_size_t i = 0; i < size; ++i) d[i] = (char)value;
    }
}

#else

#include <string.h>

static inline void proven_net_mem_copy(void *dst, const void *src, proven_size_t size) {
    if (size > 0 && dst && src) memcpy(dst, src, size);
}

static inline void proven_net_mem_move(void *dst, const void *src, proven_size_t size) {
    if (size > 0 && dst && src) memmove(dst, src, size);
}

static inline void proven_net_mem_set(void *dst, int value, proven_size_t size) {
    if (size > 0 && dst) memset(dst, value, size);
}

#endif

static inline void proven_net_mem_zero(void *dst, proven_size_t size) { proven_net_mem_set(dst, 0, size); }

#endif /* PROVEN_PLATFORM_SYS_NET_MEM_H */
