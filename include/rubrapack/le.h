#ifndef RUBRAPACK_LE_H
#define RUBRAPACK_LE_H

// include/rubrapack/le.h - little-endian integers in a byte buffer, as the compound file, MSI,
// cabinet, PE, ZIP and registry hive formats store them. The caller has checked the bytes are there.

#include <stdint.h>

static inline uint16_t rp_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rp_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rp_rd64(const uint8_t *p) { return (uint64_t)rp_rd32(p) | ((uint64_t)rp_rd32(p + 4) << 32); }

static inline void rp_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void rp_wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void rp_wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

#endif // RUBRAPACK_LE_H
