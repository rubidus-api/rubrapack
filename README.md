[한국어](README-ko.md) | **English** — **rubrapack v0.37.0** — [Linux(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.37.0/rubrapack-0.37.0-linux-x86_64) · [EXE(x64)](https://github.com/rubidus-api/rubrapack/releases/download/v0.37.0/rubrapack-0.37.0-windows-x64.exe) · [PDF(ko)](https://github.com/rubidus-api/rubrapack/releases/download/v0.37.0/rubrapack-manual-0.37.0-ko.pdf) · [PDF(en)](https://github.com/rubidus-api/rubrapack/releases/download/v0.37.0/rubrapack-manual-0.37.0-en.pdf) · [HTML(ko)](https://rubidus-api.github.io/rubrapack/ko/) · [HTML(en)](https://rubidus-api.github.io/rubrapack/en/)

# rubrapack

**Windows installers from one small text file.** rubrapack turns a short, readable description
of your program into a Windows Installer package (`.msi`) or an MSIX package (`.msix`,
`.msixbundle`), signs it, and checks it - on Windows or on Linux, with one self-contained program
and nothing else to install.

```sh
rubrapack new app.toml                    # asks a few questions, writes app.toml and checks it
rubrapack build app.toml -o app.msi       # a real installer: uninstall entry, upgrades, repair
```

## Why rubrapack

- **MSI packaging made simple.** You write what to install - files, folders, shortcuts, registry
  values, services - in a short `.toml` file (plain TOML), and rubrapack writes the database
  tables, component GUIDs, cabinets, upgrade rules and dialogs that Windows Installer needs -
  a license page to accept, an install folder to choose, a tree of optional components, in
  English and other languages. No XML, no table editor, no GUID bookkeeping: only the upgrade
  code is yours to keep.
- **Easy to automate - and easy for an AI assistant.** The source is plain text, the tool is one
  command, and every problem is reported as `file:line:column: error[RPnnnn]: message`, often with
  a suggestion (`unknown key 'glb' in [files.App] (did you mean 'glob'?)`). Exit codes are fixed,
  `lint` checks without writing anything, `inspect` prints the finished tables as text, and the
  same source always gives the same bytes. So a script, a CI job or an AI coding assistant can
  write the source, check it, read the errors, fix them and build - and verify its own result.
  Ask an assistant to package your program with rubrapack and it can also turn the steps into a
  release script for you.
- **Nothing to install.** Download one file - about 1.3 MB for Windows, 1.2 MB for Linux - and run
  it. No .NET, no Python or other interpreter, no SDK, no libraries. The Linux program needs only
  the C library; the Windows program uses only DLLs that are part of Windows (it does not even use
  `msi.dll`). Build Windows packages on a Linux server or CI runner without a Windows machine.
- **Plain C, small and easy to build.** About 37,000 lines of C23 plus a small vendored base
  library, with no third-party dependencies. Building it needs a C compiler and nothing else:
  `cc -std=c23 -o nob nob.c && ./nob` (under a minute on an ordinary PC; tested with GCC 14,
  Clang 19 and MinGW-w64 GCC 16). Everything - the compound file and MSI database, cabinets and
  deflate, ZIP and MSIX, registry hives, the cryptography, PKCS#12, HTTP and TLS - is written
  here from public specifications and checked against Windows.
- **Output you can trust.** Packages are reproducible byte for byte on Linux and Windows. Every
  feature is tested by installing, running, repairing, upgrading and removing packages on
  Windows 11. `lint` checks any MSI - also one made by another tool - and `lint new.msi --previous
  old.msi` catches the mistakes that break an upgrade before your users meet them.

## Where it fits

- You ship a desktop program - written in C, C++, Rust, Go, .NET, Python, Electron or anything
  else - and want a proper Windows installer: Program Files, Start menu, an entry in Installed
  apps, clean removal and upgrades.
- You build releases on Linux (a CI runner, a container, a build server) and want the Windows
  installer, signed, from the same pipeline.
- You sign with a key that lives on a hardware token (PKCS#11) or in the Windows certificate store.
- You want an MSIX package, or a bundle for several architectures, from the same source as the MSI.
- You need to look inside, check or unpack MSI and MSIX packages, whoever made them.

## Quick look

```sh
rubrapack new app.toml                               # asks, then writes app.toml (or: new app.toml --dist dist ...)
rubrapack edit app.toml                              # change it later: a menu, or --set define.VERSION=1.1.0 --sync
rubrapack build app.toml -o app.msi                  # a Windows Installer package
rubrapack build app.toml -o app.msix --key signer.pfx --pass-env PW --timestamp http://timestamp.digicert.com
rubrapack build app.toml -o app.msixbundle --arch x64,x86,arm64
rubrapack lint app.msi && rubrapack verify app.msix --trust root.pem
```

A first package, step by step, is the first chapter of the manual:
[Getting started](manual/rpk.md#getting-started).

## Features in detail

- **One source, both formats.** A declarative `.toml` file (a strict TOML subset; the older
  `.rpk` name works too) describes the product once; the same source builds an MSI and an MSIX.
  What one format cannot carry is an error that says so, never something left out quietly.
- **MSI:** files and folders (globs, kept or removed folders), features, registry values (with
  REG_QWORD and the 32-bit view), shortcuts, file types and URL schemes, environment variables,
  INI files, services, fonts, permissions, launch conditions and searches, a program run to
  register and unregister with rollback, major upgrades with downgrade refusal, per-machine,
  per-user and dual packages, embedded or external MSZIP or LZX cabinets, merge modules (`.msm`)
  merged in or written (`[module]`), built-in dialog sets
  in English with Korean or other languages added to the same package (a language page first;
  Korean chosen for Korean systems), dialog pages of your own, a remembered install folder, and a
  guard that refuses a prepared install folder not owned by administrators. The user's choices:
  a feature tree with required features and Change after installation, conditions (`when`) on
  features, files, shortcuts and settings, "Just me / Everyone" for dual packages, and a program
  started from the finished page; icons for Installed apps and shortcuts; files kept at removal.
  x64, x86 and Arm64 packages. Windows' folders and environment variables by their own names
  (`$(ProgramFiles)`, `$(LOCALAPPDATA)`, `$(USERNAME)`).
- **Updates and suites:** transforms (`.mst`, `rubrapack transform`) and patches (`.msp`,
  `rubrapack patch`) made from two versions of a package; a `setup.exe` that installs several
  packages in order, elevating once (`[chain]`).
- **MSIX:** full-trust desktop applications with a virtual registry (`Registry.dat`,
  `User.dat`) and virtual file system, several applications per package, file types, protocols,
  execution aliases, startup tasks, firewall rules, COM servers, toast activation, context menus,
  desktop shortcuts, shared fonts; names in several languages and logos in several scales
  (`resources.pri`); `.appinstaller` files for automatic updates; bundles of several
  architectures; signed packages with `CodeIntegrity.cat` as Windows' signer makes them; unsigned
  test packages.
- **Signing:** Authenticode for PE files, MSI and MSIX packages and bundles, RSA or ECDSA, with
  RFC 3161 timestamps (over HTTP or its own TLS 1.3). Keys come from a PFX/PEM file, a PKCS#11
  token (`--pkcs11`), or the Windows certificate store through NCrypt (`--key-store`), so a
  code-signing key that must stay in hardware stays there.
- **Checking:** `lint` (sources and any MSI or MSIX, and an MSI against the version before it),
  `inspect`, `extract`, `verify`, `keys list`.
- **Reproducible and fast:** the same source gives the same bytes on Linux and on Windows, whatever
  the number of threads compressing it (every processor by default).

## What it is not

- Not a WiX front end and not WiX-compatible; it does not read `.wxs` files.
- Patches carry changed files whole (no binary deltas); a transform carries no files.
- The `setup.exe` chain installs the `.msi` packages it carries; it does not download anything,
  run other installers, or show pages of its own beyond Windows Installer's.
- The tool itself is 64-bit only (it builds x86 packages too).

## Status

The version is the one in the top line. Every feature above is covered by tests on Linux and by
installing, running, repairing, upgrading and removing the packages on Windows 11 (x64). Not tested
on real hardware yet: Arm64 packages (structure only - no Arm64 machine), hardware PKCS#11 tokens
(a software token stands in), and a native build on a Windows host (the Windows binary is
cross-built with MinGW-w64 and runs on Windows).

## Documentation

- [`manual/rpk.md`](manual/rpk.md) - the user manual: writing sources (`.toml`) and the command line.
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
# Linux (tested with GCC 14 and Clang 19): builds build/native/rubrapack
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
