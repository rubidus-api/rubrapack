#ifndef RUBRAPACK_NFC_TABLES_H
#define RUBRAPACK_NFC_TABLES_H

// src/text/nfc_tables.h - the data nfc_tables.c holds (generated from the UCD; see that file).

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t first, last;   // code point range with the same canonical combining class
    uint8_t  ccc;
} rp_nfc_ccc_t;

typedef struct {
    uint32_t cp, a, b;      // cp -> a b (b = 0: singleton)
} rp_nfc_decomp_t;

typedef struct {
    uint32_t a, b, c;       // a + b -> c
} rp_nfc_comp_t;

extern const rp_nfc_ccc_t    rp_nfc_ccc[];
extern const size_t          rp_nfc_ccc_count;
extern const rp_nfc_decomp_t rp_nfc_decomp[];
extern const size_t          rp_nfc_decomp_count;
extern const rp_nfc_comp_t   rp_nfc_comp[];
extern const size_t          rp_nfc_comp_count;

#endif // RUBRAPACK_NFC_TABLES_H
