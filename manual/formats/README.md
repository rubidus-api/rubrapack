# Package formats for implementers

Korean: [`../../manual-ko/formats/`](../../manual-ko/formats/README.md).

These pages describe what a program has to write to produce a Windows Installer package
(`.msi`) and an MSIX package or bundle that Windows accepts, and how to sign them. They are
written from public specifications and from experiments against Windows itself, so that you can
build your own tool from them without reading rubrapack's code - or check rubrapack's code
against them.

| Page | Covers |
|---|---|
| [`cfb.md`](cfb.md) | Compound File Binary: the container every `.msi` is |
| [`msi-database.md`](msi-database.md) | Streams, string pool, system tables, table encoding, IDT export |
| [`msi-summary.md`](msi-summary.md) | The summary information stream and its code page trap |
| [`msi-package.md`](msi-package.md) | The tables that install, upgrade, repair and uninstall: files, registry, shortcuts, services, fonts, dialogs, per-user packages, sequences |
| [`pe.md`](pe.md) | Program files: machine type and version resource |
| [`cab-mszip.md`](cab-mszip.md) | Cabinet files, MSZIP blocks, the deflate encoder |
| [`identity.md`](identity.md) | Deterministic GUIDs and keys, reproducible builds |
| [`authenticode.md`](authenticode.md) | Authenticode signatures for PE files and MSI packages: the digests, the CMS structure Windows accepts, ECDSA, RFC 3161 timestamps |
| [`verify.md`](verify.md) | How to check your output with Windows' own components |
| [`msix.md`](msix.md) | MSIX: the ZIP layout, block map, manifest, virtual registry and file system, extensions, bundles, signatures |
| [`registry.md`](registry.md) | Registry hive files (REGF), as MSIX `Registry.dat` and `User.dat` hold them |

## How to read the facts

Every statement is tagged with where it comes from:

- **[spec]** - a public specification: [MS-CFB], [MS-OLEPS], [MS-CAB], [MS-MCI], RFC 1951,
  RFC 1321, RFC 3161, RFC 5652, RFC 9562, the PKWARE ZIP APPNOTE, ECMA-376 Part 2 (OPC), the
  Authenticode PE specification, or the Windows Installer and MSIX references on Microsoft Learn.
- **[observed]** - measured on Windows 11: the MSI pages on 25H2 (build 26200) with Windows
  Installer 5.0.10011, by creating databases through `msi.dll` and reading them back byte for
  byte, and by installing packages; the MSIX, registry hive and signature pages on build 26100,
  against Windows' own packaging API, Offline Registry Library and signer, and by installing and
  running packages. Observations are facts about those builds; where Microsoft does not document
  a format (the MSI table encoding, the registry hive, the MSIX signature's record of hashes are
  not published), observation is the only source.

No text or code here comes from other MSI, MSIX, cabinet or registry implementations.

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

An MSIX package is simpler: the payload files (each deflated in independent 64 KiB parts),
`AppxManifest.xml`, `AppxBlockMap.xml` (a hash per part) and `[Content_Types].xml` in one ZIP64
archive, with `Registry.dat`/`User.dat` hives for registry values; a bundle stores packages side
by side; a signature adds `AppxSignature.p7x` ([msix.md](msix.md), [registry.md](registry.md)).

Each arrow is a page above. None of it needs Windows to produce: rubrapack builds the same bytes
on Linux and on Windows.
