#ifndef RUBRAPACK_MSI_H
#define RUBRAPACK_MSI_H

// include/rubrapack/msi.h - Windows Installer database reader over a compound file
// (format notes F2). Strings stay in the pool's code page (UTF-8 for 65001).

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/cfb.h"
#include "rubrapack/limits.h"

// _Columns.Type bits.
#define RP_MSI_COL_WIDTH       0x00FFu
#define RP_MSI_COL_VALID       0x0100u
#define RP_MSI_COL_LOCALIZABLE 0x0200u
#define RP_MSI_COL_NONBINARY   0x0400u
#define RP_MSI_COL_STRING      0x0800u
#define RP_MSI_COL_NULLABLE    0x1000u
#define RP_MSI_COL_KEY         0x2000u

typedef struct {
    uint32_t offset;        // into the string data
    uint32_t length;        // bytes
    uint16_t refcount;      // 0 = free id
} rp_msi_string_t;

typedef struct {
    uint32_t name;          // string id
    uint16_t type;          // _Columns.Type
} rp_msi_column_t;

typedef struct {
    uint32_t         name;  // string id
    rp_msi_column_t *columns;
    size_t           column_count;
} rp_msi_table_t;

typedef struct {
    proven_allocator_t alloc;
    const rp_cfb_t    *cfb;         // borrowed
    rp_limits_t        limits;
    uint32_t           codepage;
    bool               long_refs;   // 3-byte string references
    uint8_t           *string_data;
    size_t             string_data_len;
    rp_msi_string_t   *strings;     // index = string id; [0] is the null string
    size_t             string_count;
    rp_msi_table_t    *tables;      // in _Tables order
    size_t             table_count;
} rp_msi_t;

enum { RP_MSI_NULL, RP_MSI_INT, RP_MSI_STR, RP_MSI_BINARY };

typedef struct {
    uint8_t  kind;          // RP_MSI_*
    int32_t  i;             // RP_MSI_INT
    uint32_t s;             // RP_MSI_STR: string id
} rp_msi_value_t;

typedef struct {
    size_t          row_count;
    size_t          column_count;
    rp_msi_value_t *cells;  // row-major: cells[row * column_count + column]
} rp_msi_rows_t;

[[nodiscard]] proven_err_t rp_msi_open(rp_msi_t *msi, proven_allocator_t alloc, const rp_cfb_t *cfb,
                                       const rp_limits_t *limits, const char **why);
void rp_msi_close(rp_msi_t *msi);

// String bytes for a string id (id 0 gives an empty string).
[[nodiscard]] proven_err_t rp_msi_string(const rp_msi_t *msi, uint32_t id, const uint8_t **bytes, size_t *len);

// Finds a table by name (UTF-8); *index is into msi->tables.
[[nodiscard]] proven_err_t rp_msi_find_table(const rp_msi_t *msi, const char *name, size_t *index);

// Reads a table's rows in stored order. Free with rp_msi_rows_free.
[[nodiscard]] proven_err_t rp_msi_read_rows(const rp_msi_t *msi, size_t table, rp_msi_rows_t *rows);
void rp_msi_rows_free(const rp_msi_t *msi, rp_msi_rows_t *rows);

// Writes a table as an IDT archive file, byte for byte what MsiDatabaseExport writes
// (format notes F2 "IDT archive files"). *out is from msi->alloc (rp_mem_free).
[[nodiscard]] proven_err_t rp_msi_export_idt(const rp_msi_t *msi, size_t table, uint8_t **out, size_t *len);

// `_ForceCodepage` pseudo-table as MsiDatabaseExport writes it.
[[nodiscard]] proven_err_t rp_msi_export_codepage(const rp_msi_t *msi, uint8_t **out, size_t *len);

// Packs an MSI name into a CFB stream name (format notes F2 "Stream names"). `table` adds the
// U+4840 prefix. PROVEN_ERR_OUT_OF_BOUNDS when it does not fit in 31 units.
[[nodiscard]] proven_err_t rp_msi_stream_name(const char *utf8, bool table, uint16_t out[32], size_t *len);

// Unpacks a CFB stream name to UTF-8 into out (NUL-terminated); *table tells whether it had
// the U+4840 prefix.
[[nodiscard]] proven_err_t rp_msi_unpack_name(const uint16_t *name, size_t len, char *out, size_t cap, bool *table);

#endif // RUBRAPACK_MSI_H
