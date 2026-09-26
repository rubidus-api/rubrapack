# rubrapack

A command-line tool that builds Windows installer packages: Windows Installer (`.msi`)
and MSIX (`.msix`). It runs on Windows and on Linux.

## What it is

- One declarative source file in, one `.msi` (with an embedded CAB) or `.msix` out.
  Target architectures: x64, Arm64, x86. Several MSIX architectures can go into one `.msixbundle`.
- Covers everyday installer work: install and remove files, create and remove folders,
  registry values, shortcuts, environment variables, services, file associations, upgrades,
  launch conditions, custom actions, built-in installer dialogs.
- Compression choices (none, MSZIP levels; deflate or stored for MSIX) and code signing
  with RFC 3161 timestamps.
- Built-in inspection: `inspect`, `extract`, `lint`, `verify`, so results can be checked
  without other tools.
- No run-time dependencies beyond the operating system: formats, compression, signing
  crypto, HTTP and TLS are all implemented in this repository.

## What it is not

- Not a WiX front end and not WiX-compatible. It does not read `.wxs` files.
- The tool itself is 64-bit only (it still builds x86 packages).
- No patches (`.msp`), transforms (`.mst`), merge-module authoring, or bootstrapper bundles.

## Stack

C23 on proven_c_lib (vendored). Win32 API on Windows,
POSIX on Linux. The tool is 64-bit only. Every file format (compound file, MSI database, CAB, deflate,
ZIP/OPC, block map) is implemented in this repository from public specifications.

## Status

Early implementation. `rubrapack build app.rpk -o app.msi` builds an installable MSI from a
source file (files, folders, features, major upgrades with downgrade refusal; the cabinet is
stored uncompressed for now), and `rubrapack inspect <file.msi> [table|--summary|--files|--streams]`
prints any MSI's tables in the IDT archive format. The same source gives the same bytes on Linux
and Windows with `--reproducible`. Commands that are not built yet report "not implemented yet"
and exit with status 2.

## Text encoding

Package text is Unicode end to end: MSI databases are written with code page 65001 (UTF-8),
which Windows Installer converts to UTF-16 when it installs. File names, folders, registry
values, and shortcuts can use any script.

## Build

```sh
# Linux (gcc 14+ or clang 18+): builds build/native/rubrapack
cc -std=c23 -o nob nob.c && ./nob

# Windows (MinGW-w64 UCRT): builds build/native/rubrapack.exe
gcc -std=c23 -o nob.exe nob.c && nob.exe

# Linux -> Windows x64 cross build: builds build/win64/rubrapack.exe
./nob --target=win64
```

`nob.c` needs nothing but a C23 compiler. The Linux build and the Linux -> Windows cross build are
tested; a native build on a Windows host is not yet. Options: `--sanitize` (AddressSanitizer and
UBSan), `--debug`, `clean`. `CC` and `RUBRAPACK_WIN64_CC` choose the compilers.

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
