# Program files: machine type and version

A package writer reads two facts from every Portable Executable (`.exe`, `.dll`, ...) it packs:
the machine type, to refuse a file built for the wrong architecture, and the version resource,
because Windows Installer treats versioned and unversioned files differently. Layout from the
Microsoft PE/COFF specification and the `VS_VERSIONINFO` documentation. [spec]

## Finding the headers [spec]

```text
offset 0      "MZ"
offset 0x3C   u32  e_lfanew: offset of the PE signature
e_lfanew      "PE\0\0"
+4            COFF header: u16 machine, u16 number of sections, ..., u16 optional header size (at +16)
+24           optional header: u16 magic 0x10B (PE32) or 0x20B (PE32+); the data directories
              start at +96 (PE32) or +112 (PE32+), preceded by their count
then          section table: 40 bytes per section (virtual size +8, virtual address +12,
              raw size +16, raw offset +20)
```

Machine values: `0x8664` x64, `0xAA64` Arm64, `0x014C` x86. A file that starts with `MZ` but has
no valid `PE\0\0` header is an ordinary data file.

## The version resource [spec]

1. Data directory 2 is the resource table (an RVA and size). Map RVAs to file offsets through
   the section table.
2. Resource directories are three levels deep: type -> name -> language. Each directory has a
   16-byte header (named and ID entry counts at +12 and +14) followed by 8-byte entries
   (u32 name or ID, u32 offset; the high bit of the offset marks a sub-directory).
3. Type 16 (`RT_VERSION`), its first name, its first language: the leaf is a data entry
   (u32 RVA, u32 size).
4. The data is `VS_VERSIONINFO`: u16 length, u16 value length, u16 type, the UTF-16 key
   `VS_VERSION_INFO` with its NUL, padding to 4 bytes, then `VS_FIXEDFILEINFO` whose first u32 is
   `0xFEEF04BD`. `dwFileVersionMS` (+8) and `dwFileVersionLS` (+12) give the version
   `HIWORD(MS).LOWORD(MS).HIWORD(LS).LOWORD(LS)`.

## What goes into the package [observed]

- Versioned file: `File.Version` = the four-part version, `File.Language` = the language
  `MsiGetFileVersion` reports, which is the first language of `VarFileInfo\Translation`
  (for example 1033, 1042); no `MsiFileHash` row. Installed files read back with the same version.
- Unversioned file: no version, a `MsiFileHash` row ([msi-package.md](msi-package.md)).
- Machine type: an x64 package takes x64 files, an Arm64 package Arm64 files, an x86 package x86
  files. A deliberate exception (a 32-bit helper in a 64-bit package) should be declared, not
  silently accepted; the component's own bitness does not change.

## Reading safely

Check every offset and size against the file length before using it, map RVAs only through a
section that holds the whole range, and stop at the three expected directory levels - crafted
files with loops or out-of-range pointers are common.
