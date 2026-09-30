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
    uint16_t refcount;      // 0 = free id (low 15 bits of the pool word)
    bool     non_ascii;     // pool word bit 15: the string has a byte >= 0x80
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

// ---- writer ----------------------------------------------------------------------------------

typedef struct {
    uint8_t        kind;    // RP_MSI_NULL, RP_MSI_INT, RP_MSI_STR, RP_MSI_BINARY
    int32_t        i;       // RP_MSI_INT
    const uint8_t *bytes;   // RP_MSI_STR: string bytes in the pool code page; RP_MSI_BINARY: stream bytes
    size_t         len;     // an empty string is stored as null, as msi.dll does
} rp_msi_cell_t;

typedef struct {
    const char *name;       // column name (identifier)
    uint16_t    type;       // _Columns.Type bits
} rp_msi_wcolumn_t;

typedef struct {
    const char             *name;
    const rp_msi_wcolumn_t *columns;
    size_t                  column_count;
    const rp_msi_cell_t    *cells;      // row-major, row_count * column_count
    size_t                  row_count;
} rp_msi_wtable_t;

typedef struct {
    const char    *name;    // stream name before packing (e.g. "cab1.cab"); not a table
    const uint8_t *data;
    size_t         len;
} rp_msi_wstream_t;

typedef struct {
    uint32_t               codepage;    // string pool code page (65001)
    const rp_msi_wtable_t *tables;
    size_t                 table_count;
    const uint8_t         *summary;     // optional `\005SummaryInformation` stream bytes
    size_t                 summary_len;
    const rp_msi_wstream_t *streams;    // other streams (embedded cabinets)
    size_t                 stream_count;
} rp_msi_wdb_t;

// The _Validation table for these tables and itself (src/msi/validation.c, ICE03): one row per
// column. PROVEN_ERR_NOT_FOUND when a column has no rule; *missing then names its table. Free
// out->cells with the allocator.
[[nodiscard]] proven_err_t rp_msi_validation(proven_allocator_t alloc, const rp_msi_wtable_t *tables, size_t count,
                                             rp_msi_wtable_t *out, const char **missing);

// Writes a database (format notes F2; DECISIONS 2026-09-26 "P1b writer"): string ids sorted by
// UTF-8 bytes, reference words counted, rows sorted by stored key values, one stream per
// non-empty table and per binary cell (`Table.key`). Deterministic for the same input.
[[nodiscard]] proven_err_t rp_msi_write(proven_allocator_t alloc, const rp_msi_wdb_t *db, unsigned sector_shift,
                                        const rp_limits_t *limits, uint8_t **out, size_t *len);
// rp_msi_write into the sink's buffer (rubrapack/cfb.h; NULL: the heap).
[[nodiscard]] proven_err_t rp_msi_write_to(proven_allocator_t alloc, const rp_msi_wdb_t *db, unsigned sector_shift,
                                           const rp_limits_t *limits, const rp_out_sink_t *sink, uint8_t **out, size_t *len);

// ---- view ------------------------------------------------------------------------------------

// A database read with rp_msi_open, seen as writer tables so the lint rules (rubrapack/lint.h)
// run on any MSI (RFC-0006 1). Strings point into `msi`, which must stay open; binary cells
// carry no bytes. `summary` (may be NULL) is borrowed. Free with rp_msi_view_free.
typedef struct {
    rp_msi_wdb_t db;
    void        *priv;
} rp_msi_view_t;

[[nodiscard]] proven_err_t rp_msi_view(const rp_msi_t *msi, const uint8_t *summary, size_t summary_len, rp_msi_view_t *view);
void rp_msi_view_free(const rp_msi_t *msi, rp_msi_view_t *view);

#endif // RUBRAPACK_MSI_H
