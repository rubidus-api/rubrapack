#ifndef RUBRAPACK_PE_H
#define RUBRAPACK_PE_H

// include/rubrapack/pe.h - Portable Executable facts a package needs (RFC-0001 F7): the machine
// type (checked against the package architecture) and the version resource (File.Version and
// File.Language instead of a file hash).

#include <stddef.h>
#include <stdint.h>

#include "proven/types.h"

enum {
    RP_PE_I386 = 0x014C,
    RP_PE_AMD64 = 0x8664,
    RP_PE_ARM64 = 0xAA64,
};

typedef struct {
    bool     is_pe;             // "MZ" + a valid "PE\0\0" header
    uint16_t machine;           // COFF machine
    bool     is_dll;            // COFF characteristics IMAGE_FILE_DLL (0x2000)
    bool     has_version;       // VS_FIXEDFILEINFO found
    uint16_t version[4];        // file version a.b.c.d
    uint16_t language;          // first Translation language, else the resource language, else 0
    uint16_t subsystem;         // optional header Subsystem: 2 GUI, 3 console (0 when not read)
} rp_pe_info_t;

// Reads what it can. A file that is not PE gives is_pe = false and PROVEN_OK; a PE file whose
// headers or resources point outside the file gives PROVEN_ERR_INVALID_FORMAT.
[[nodiscard]] proven_err_t rp_pe_read(const uint8_t *data, size_t len, rp_pe_info_t *info);

// A .NET assembly's identity (RFC-0031 B9), from its CLI header and metadata (ECMA-335 Partition
// II: the metadata root, the #~ tables, the #Strings and #Blob heaps): the Assembly table's name,
// version and culture, the public key token (the last 8 bytes of the SHA-1 of the public key,
// reversed) and the processor architecture (from the CLI flags and the machine: "MSIL", "x86",
// "AMD64", "ARM64"), as the GAC and MsiAssemblyName write them.
typedef struct {
    bool     is_assembly;       // a CLI header and an Assembly row
    char     name[256];
    uint16_t version[4];
    char     culture[64];       // "" for neutral
    bool     has_token;         // strong-named
    uint8_t  token[8];
    char     arch[8];
} rp_pe_assembly_t;

// Reads it. A file that is not PE or not .NET gives is_assembly = false and PROVEN_OK; metadata
// that points outside the file or that it does not understand gives PROVEN_ERR_INVALID_FORMAT.
[[nodiscard]] proven_err_t rp_pe_assembly(const uint8_t *data, size_t len, rp_pe_assembly_t *out);

#endif // RUBRAPACK_PE_H
