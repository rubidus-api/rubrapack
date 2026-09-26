# Package formats for implementers

These pages describe what a program has to write to produce a Windows Installer package
(`.msi`) that Windows accepts, and (later) an MSIX package. They are written from public
specifications and from experiments against Windows itself, so that you can build your own
tool from them without reading rubrapack's code - or check rubrapack's code against them.

| Page | Covers |
|---|---|
| [`cfb.md`](cfb.md) | Compound File Binary: the container every `.msi` is |
| [`msi-database.md`](msi-database.md) | Streams, string pool, system tables, table encoding, IDT export |
| [`msi-summary.md`](msi-summary.md) | The summary information stream and its code page trap |
| [`msi-package.md`](msi-package.md) | The smallest table set that installs, upgrades, repairs and uninstalls |
| [`cab-mszip.md`](cab-mszip.md) | Cabinet files, MSZIP blocks, the deflate encoder |
| [`identity.md`](identity.md) | Deterministic GUIDs and keys, reproducible builds |
| [`verify.md`](verify.md) | How to check your output with Windows' own components |
| [`msix.md`](msix.md) | MSIX - planned, not verified yet |

## How to read the facts

Every statement is tagged with where it comes from:

- **[spec]** - a public specification: [MS-CFB], [MS-OLEPS], [MS-CAB], [MS-MCI], RFC 1951,
  RFC 1321, RFC 9562, or the Windows Installer reference on Microsoft Learn.
- **[observed]** - measured on Windows 11 25H2 (build 26200) with Windows Installer 5.0.10011,
  by creating databases through `msi.dll` and reading them back byte for byte, and by installing
  packages. Observations are facts about that Windows build; where Microsoft does not document
  a format (the MSI table encoding is not published), observation is the only source.

No text or code here comes from other MSI or cabinet implementations.

## Conventions

- All integers are little-endian unless stated otherwise.
- `u16`, `u32`, `u64` are unsigned; `i16`, `i32` signed two's complement.
- Offsets are in bytes from the start of the structure named.
- Hex byte strings are written in file order: `D0 CF 11 E0`.

## The pipeline, in one picture

```text
source description --> tables (rows of strings/integers/streams)
                   --> MSI database encoding (string pool + one stream per table)
                   --> files packed into a cabinet (stored or MSZIP) --> one more stream
                   --> summary information stream
                   --> all streams written into one compound file  = the .msi
```

Each arrow is a page above. None of it needs Windows to produce: rubrapack builds the same bytes
on Linux and on Windows.
