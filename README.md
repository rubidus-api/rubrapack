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

Planning. The design is written; implementation has not started.

## Text encoding

Package text is Unicode end to end: MSI databases are written with code page 65001 (UTF-8),
which Windows Installer converts to UTF-16 when it installs. File names, folders, registry
values, and shortcuts can use any script.

## Build (planned)

```sh
# Linux (gcc 14+ or clang 18+)
cc -std=c23 -o nob nob.c && ./nob

# Windows (MinGW-w64 UCRT)
gcc -std=c23 -o nob.exe nob.c && nob.exe
```

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
