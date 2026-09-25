# rubrapack

A command-line tool that builds Windows installer packages: Windows Installer (`.msi`)
and MSIX (`.msix`). It runs on Windows and on Linux.

## What it is

- One declarative source file in, one x64 `.msi` (with an embedded CAB) or `.msix` out.
- Covers everyday installer work: install and remove files, create and remove folders,
  registry values, shortcuts, environment variables, services, file associations, upgrades,
  launch conditions, custom actions, built-in installer dialogs.
- Compression choices (none, MSZIP levels; deflate or stored for MSIX) and code signing
  with RFC 3161 timestamps.
- Built-in inspection: `inspect`, `extract`, `lint`, `verify`, so results can be checked
  without other tools.

## What it is not

- Not a WiX front end and not WiX-compatible. It does not read `.wxs` files.
- Not a 32-bit tool, and it does not produce 32-bit packages.
- No patches (`.msp`), transforms (`.mst`), merge-module authoring, or bootstrapper bundles.

## Stack

C23 on proven_c_lib (vendored). Win32 API on Windows,
POSIX on Linux. 64-bit only. Every file format (compound file, MSI database, CAB, deflate,
ZIP/OPC, block map) is implemented in this repository from public specifications.

## Status

Planning. The design is written; implementation has not started.

## Build (planned)

```sh
# Linux (gcc 14+ or clang 18+)
cc -std=c23 -o nob nob.c && ./nob

# Windows (MinGW-w64 UCRT)
gcc -std=c23 -o nob.exe nob.c && nob.exe
```

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
