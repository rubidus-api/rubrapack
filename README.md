[한국어](README-ko.md) | **English** — **rubrapack v0.2.0** — [Linux(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.2.0/rubrapack-0.2.0-linux-x86_64) · [EXE(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.2.0/rubrapack-0.2.0-windows-x64.exe) · [PDF(ko)](https://github.com/rubidus-api/rubrapack/releases/download/v0.2.0/rubrapack-manual-0.2.0-ko.pdf) · [PDF(en)](https://github.com/rubidus-api/rubrapack/releases/download/v0.2.0/rubrapack-manual-0.2.0-en.pdf)

# rubrapack

A command-line tool that builds and signs Windows installer packages - Windows Installer (`.msi`)
and MSIX (`.msix`, `.msixbundle`) - on Windows and on Linux.

```sh
rubrapack new app                                   # writes app.rpk, a source to fill in
rubrapack build app.rpk -o app.msi                  # a Windows Installer package
rubrapack build app.rpk -o app.msix --key signer.pfx --pass-env PW --timestamp http://timestamp.digicert.com
rubrapack build app.rpk -o app.msixbundle --arch x64,x86,arm64
rubrapack lint app.msi && rubrapack verify app.msix --trust root.pem
```

## What it does

- **One source, both formats.** A declarative `.rpk` file (a strict TOML subset) describes the
  product once; the same source builds an MSI and an MSIX. What one format cannot carry is an error
  that says so, never something left out quietly.
- **MSI:** files and folders (globs, kept or removed folders), features, registry values (with
  REG_QWORD and the 32-bit view), shortcuts, file types and URL schemes, environment variables,
  INI files, services, fonts, permissions, launch conditions and searches, a program run to
  register and unregister with rollback, major upgrades with downgrade refusal, per-machine,
  per-user and dual packages, embedded or external MSZIP cabinets, built-in dialog sets
  in English with Korean or other languages added to the same package (a language page first;
  Korean chosen for Korean systems), dialog pages of your own, a remembered install folder, and a
  guard that refuses a prepared install folder not owned by administrators. x64, x86 and Arm64
  packages.
- **MSIX:** full-trust desktop applications with a virtual registry (`Registry.dat`,
  `User.dat`) and virtual file system, several applications per package, file types, protocols,
  execution aliases, startup tasks, desktop shortcuts, shared fonts; bundles of several
  architectures; unsigned test packages.
- **Signing:** Authenticode for PE files, MSI and MSIX packages and bundles, RSA or ECDSA, with
  RFC 3161 timestamps (over HTTP or its own TLS 1.3). Keys come from a PFX/PEM file, a PKCS#11
  token (`--pkcs11`), or the Windows certificate store through NCrypt (`--key-store`), so a
  code-signing key that must stay in hardware stays there.
- **Checking:** `lint` (sources and any MSI or MSIX), `inspect`, `extract`, `verify`, `keys list`.
- **Reproducible:** the same source gives the same bytes on Linux and on Windows.
- **No run-time dependencies** beyond the operating system: the compound file and MSI database,
  cabinets and deflate, ZIP/OPC and the block map, registry hives, the crypto, PKCS#12, HTTP and
  TLS are all written in this repository from public specifications and checked against Windows.

## What it is not

- Not a WiX front end and not WiX-compatible; it does not read `.wxs` files.
- No patches (`.msp`), transforms (`.mst`), merge-module authoring or bootstrapper bundles.
- The tool itself is 64-bit only (it builds x86 packages too).

## Status

Version 0.2.0. Every feature above is covered by tests on Linux and by
installing, running, repairing, upgrading and removing the packages on Windows 11 (x64). Not tested
on real hardware yet: Arm64 packages (structure only - no Arm64 machine), hardware PKCS#11 tokens
(a software token stands in), and a native build on a Windows host (the Windows binary is
cross-built with MinGW-w64 and runs on Windows).

## Documentation

- [`manual/rpk.md`](manual/rpk.md) - the user manual: writing `.rpk` sources and the command line.
- [`manual/formats/`](manual/formats/README.md) - the file format manual, for implementers: the
  compound file, the MSI database encoding, summary information, the tables that install,
  cabinets/MSZIP/deflate, deterministic identities, Authenticode and timestamps, MSIX packages,
  bundles and signatures, registry hives, and how to check your own output against Windows.
- Both manuals as one book (PDF and web): <https://rubidus-api.github.io/rubrapack/>
- Korean: [`manual-ko/`](manual-ko/README.md).

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

`nob.c` needs nothing but a C23 compiler. Options: `--sanitize` (AddressSanitizer and UBSan),
`--debug`, `clean`. `CC` and `RUBRAPACK_WIN64_CC` choose the compilers. The helper DLLs that MSI
packages carry for REG_QWORD values are in `resources/bin/` with their SHA-256 sums; they are
rebuilt from `src/` byte for byte.

## License

MIT. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
