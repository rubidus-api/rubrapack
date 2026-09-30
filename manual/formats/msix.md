# MSIX packages

What a program has to write so that Windows reads, installs and runs an MSIX package. Facts are
tagged as in [README.md](README.md): **[spec]** for Microsoft Learn (package manifest and block
map schemas) and ECMA-376 Part 2 (Open Packaging Conventions); **[observed]** for packages written
by Windows' own packaging API (`IAppxFactory`/`IAppxPackageWriter` in AppxPackaging.dll, part of
Windows) and for rubrapack's packages read back through `IAppxPackageReader` and installed with
`Add-AppxPackage` on Windows 11. rubrapack writes what is described here.

## The ZIP archive

- The payload files first, then `AppxManifest.xml`, `AppxBlockMap.xml` and `[Content_Types].xml`,
  in that order. [observed]
- Every entry is ZIP64, whatever its size: the local header has version needed 4.5, flag 0x0008
  (data descriptor), CRC and sizes 0 and no extra field; the data is followed by a ZIP64 data
  descriptor (`PK\7\8`, CRC-32, compressed and plain size as 8 bytes each). The central directory
  entry has made-by and needed 4.5 (MS-DOS), sizes and offset 0xFFFFFFFF and a ZIP64 extra field
  (0x0001, 24 bytes: plain size, compressed size, local header offset). The archive ends with a
  ZIP64 end record (size 44), its locator, and an end record whose counts and offsets are all
  0xFFFF / 0xFFFFFFFF. [observed]
- Methods: stored (0) and deflate (8). `AppxManifest.xml`, `AppxBlockMap.xml` and
  `[Content_Types].xml` are deflated even when the payload is stored. [observed]
- Entry names are OPC part names: `/` between folders and every byte outside `A-Z a-z 0-9 - . _ ~`
  percent-encoded, UTF-8 bytes included - `data\<U+C790> %#(1).txt` (a Hangul syllable, a space,
  `%`, `#` and parentheses) is stored as `data/%EC%9E%90%20%25%23%281%29.txt` - and the UTF-8
  name flag is not set.
  `[Content_Types].xml` keeps its brackets. [observed]
- The dates in the headers are not read; Windows writes the time of writing, rubrapack
  1980-01-01 00:00 so that a package does not change from one build to the next. [observed]

## The block map (`AppxBlockMap.xml`)

```xml
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap"
          xmlns:b4="http://schemas.microsoft.com/appx/2021/blockmap" IgnorableNamespaces="b4"
          HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">
  <File Name="data\text.txt" Size="218890" LfhSize="43">
    <Block Hash="(base64 SHA-256 of 65536 plain bytes)" Size="(compressed bytes of this block)"/>
    ...
    <b4:FileHash Hash="(base64 SHA-256 of the whole file)"/>
  </File>
  ...
</BlockMap>
```

- One `File` per payload file and one for `AppxManifest.xml`; the block map and
  `[Content_Types].xml` are not listed. `Name` is the path as written, with `\`, not
  percent-encoded; `Size` is the plain size; `LfhSize` the local header's size (30 + the ZIP
  name's length). An empty file has no `Block`. [spec] [observed]
- One `Block` per 65536 bytes of plain data; `Hash` is the base64 SHA-256 of that plain block.
  [spec]
- A deflated file is one independent raw deflate part per block: each part decodes with a fresh
  decoder (nothing refers back into an earlier block) and ends with an empty stored block
  (`00 00 FF FF`), and after the last part comes an empty final block (`03 00`). The block's
  `Size` counts the bytes of its part, so the compressed size of the file is the sum of the
  `Size`s plus 2 (an empty file: 2 bytes, no block). Stored files have no `Size` on their
  blocks. Incompressible data is deflated as well (into stored blocks). [observed]
- `b4:FileHash` appears only for files of more than one block. [observed]
- Windows' reader checks every block's hash as the file is read, and refuses a package whose
  file differs from its block map, both when reading and when installing. [observed]

## `[Content_Types].xml`

One line: a `Default` per file extension in order of first use (lower case), `xml` as
`application/vnd.ms-appx.manifest+xml`, and `Override PartName="/AppxBlockMap.xml"` as
`application/vnd.ms-appx.blockmap+xml`. rubrapack adds an `Override` for each file without an
extension, and one for `/AppxManifest.xml` when a payload `.xml` file took the `xml` default.
[observed]

## The manifest (`AppxManifest.xml`)

The smallest desktop application that Windows installs and starts (namespaces `foundation/windows10`,
`uap/windows10`, `restrictedcapabilities`): [spec] [observed]

- `Identity`: `Name` (3-50 characters of `A-Z a-z 0-9 . -`), `Publisher` (the signing
  certificate's subject), `Version` (four parts, each at most 65535), `ProcessorArchitecture`
  (`x64`, `arm64`, `x86`).
- `Properties`: `DisplayName`, `PublisherDisplayName`, `Logo` (a PNG in the package).
- `Dependencies/TargetDeviceFamily Name="Windows.Desktop"` with `MinVersion` and
  `MaxVersionTested`.
- `Resources/Resource Language`.
- `Application Id Executable EntryPoint="Windows.FullTrustApplication"` with
  `uap:VisualElements` (`DisplayName`, `Description`, `BackgroundColor`, `Square150x150Logo`,
  `Square44x44Logo`), and the restricted capability `runFullTrust`.
- Paths in the manifest use `\` and name files of the package.

## The virtual registry (`Registry.dat`, `User.dat`)

Registry hives at the package root (the REGF format: [registry.md](registry.md)). Measured with a
packaged app on Windows 11 (26100): [observed]

- `Registry.dat`: its `REGISTRY\MACHINE\SOFTWARE` key stands for `HKLM\Software` (the hive's root
  itself does not); `REGISTRY\MACHINE\SOFTWARE\WOW6432Node\...` is what the app sees in the 32-bit
  view. Microsoft's description ("registry.dat serves as the logical equivalent of HKLM\Software")
  is about what the app sees, not about the hive's layout.
- `User.dat`: its root stands for `HKCU` (`Software\...` beneath it is `HKCU\Software\...`).
- The app reads the package's keys merged into the real registry; nothing is written to the
  machine's registry, and nothing is left after removal.
- Windows' packaging tools write these hives with the Offline Registry Library (its mark "OfRg" is
  in the base block).

## The virtual file system (`VFS\...`)

Files under `VFS\<folder>` appear to the app at the real location; the real folder does not
change. Measured: `ProgramFilesX64` (%ProgramFiles%), `SystemX64` (System32), `Common AppData`
(%ProgramData%) - and none for `AppData` or `Local AppData`, as Microsoft Learn says. The other names
rubrapack uses (`ProgramFilesX86`, `ProgramFilesCommonX64`/`X86`, `SystemX86`, `Windows`) are those
Microsoft Learn lists. [spec] [observed]

## Extensions

What rubrapack writes into an application's `<Extensions>` (after `uap:VisualElements`), and only
this: a source feature with no row here is an error, and one whose Windows build is above the
package's `MinVersion` asks for `min-version` to be raised. [spec] Microsoft Learn, "Integrate your
desktop app with Windows using packaging extensions" and the element pages. The last column is
what was checked by installing a package on Windows 11 (26100). [observed]

| Source | Element (category) | Namespace | Min build | Capability | Checked on Windows 11 |
|---|---|---|---|---|---|
| `[assoc]` | `uap:Extension` `windows.fileTypeAssociation` > `uap3:FileTypeAssociation` (Name, Parameters) > `uap:DisplayName`, `uap:SupportedFileTypes` > `uap:FileType` | uap, uap3 | 14393 | runFullTrust | opening a `.rpx`/`.rpy` file starts the program with the file |
| `[protocol]` | `uap3:Extension` `windows.protocol` > `uap3:Protocol` (Name, Parameters) | uap3 | 14393 | runFullTrust | starting a `scheme:` URI starts the program with it |
| `[msix-extension]` alias | `uap3:Extension` `windows.appExecutionAlias` (Executable, EntryPoint) > `uap3:AppExecutionAlias` > `desktop:ExecutionAlias` (Alias) | uap3, desktop | 14393 | runFullTrust | the alias appears in `%LOCALAPPDATA%\Microsoft\WindowsApps` and starts the program |
| `[msix-extension]` startup task | `desktop:Extension` `windows.startupTask` (Executable, EntryPoint) > `desktop:StartupTask` (TaskId, Enabled, DisplayName) | desktop | 14393 | runFullTrust | registered after the first start |
| `[shortcut]` Desktop | `desktop7:Extension` `windows.shortcut` > `desktop7:Shortcut` (File `$(Desktop)\<name>.lnk`, Icon, Arguments, Description) | desktop7 | 19645 | runFullTrust | the shortcut is on the user's desktop and starts the program |
| `[shortcut]` Programs, StartMenu | none: the application's own Start entry | - | - | - | the Start menu lists the application |
| `[font]` | `uap4:Extension` `windows.sharedFonts` > `uap4:SharedFonts` > `uap4:Font` (File `Fonts\<name>`), in the first application | uap4 | 15063 | - | other programs see the font while the package is installed, and not after |

- The namespaces are declared on `Package`, and made ignorable, only when used, so a package
  without extensions keeps the manifest described above.
- `Parameters`, `Arguments`: literal text (MSI's `[...]` is refused); Windows puts the file or URI
  where `%1` is.
- `FileTypeAssociation` `Name`: the prog-id in lower case; `[assoc]` tables that share a prog-id
  become one association with several `FileType`s.
- Everything goes into the application whose executable the source names as target; a target
  that is not an application's executable is an error.

## Bundles (`.msixbundle`)

A ZIP archive laid out like a package (ZIP64 entries with data descriptors), as Windows' bundle
writer (`IAppxBundleWriter`) makes it: [observed]

- The packages first, stored (method 0) under their file names, in the order they were added;
  then `AppxMetadata/AppxBundleManifest.xml`, `AppxBlockMap.xml` and `[Content_Types].xml`,
  deflated.
- The block map lists the bundle manifest only (`AppxMetadata\AppxBundleManifest.xml`); the
  packages carry their own block maps.
- `[Content_Types].xml`: `Default` `msix` = `application/vnd.ms-appx`, `Default` `xml` =
  `application/vnd.ms-appx.bundlemanifest+xml`, and an `Override` for `/AppxBlockMap.xml`.
- The bundle manifest, CRLF line ends, tab indents, no line end after the last tag:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<Bundle xmlns="http://schemas.microsoft.com/appx/2013/bundle" SchemaVersion="5.0" xmlns:b4="http://schemas.microsoft.com/appx/2018/bundle" xmlns:b5="http://schemas.microsoft.com/appx/2019/bundle" IgnorableNamespaces="b4 b5">
	<Identity Name="Example.App" Publisher="CN=Example" Version="1.0.0.0"/>
	<Packages>
		<Package Type="application" Version="1.0.0.0" Architecture="x64" FileName="Example.App_1.0.0.0_x64.msix" Offset="66" Size="61340">
			<Resources>
				<Resource Language="en-US"/>
			</Resources>
			<b4:Dependencies>
				<b4:TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0"/>
			</b4:Dependencies>
		</Package>
	</Packages>
</Bundle>
```

- `Offset` is where the package's bytes start in the bundle (after its local header), `Size`
  their length. `Resources` and `Dependencies` repeat the package manifest's `Resource` and
  `TargetDeviceFamily` elements.
- The bundle's `Version` is the writer's argument (a 64-bit number, 16 bits per part);
  rubrapack gives the packages' version. Every package must have the bundle's `Name` and
  `Publisher`, one version, and an architecture of its own.
- rubrapack's bundle of the same packages has the same entries at the same offsets and the same
  manifest, block map and content types, byte for byte; only the ZIP dates differ (1980 in
  rubrapack's). Windows installs from it the package for its own architecture: x64 on an x64
  machine even when x86 and arm64 are there, x86 from a bundle of x86 alone. [observed]

## Signatures (`AppxSignature.p7x`)

Worked out against Windows' signer (`mssign32!SignerSignEx2` with `APPX_SIP_CLIENT_DATA`; the
PowerShell cmdlet cannot sign a package) and checked by installing rubrapack's signed packages.
[observed]

- `Publisher` in the manifest must be the signing certificate's subject written the way Windows
  displays it: the RDNs from last to first, `A=value` joined by `, ` - a certificate made with
  `/CN=Example/O=Example Ltd` needs `O=Example Ltd, CN=Example`. Otherwise signing fails with
  0x8007000B.
- Signing rewrites the archive the way the signer does: the payload's local records, the manifest
  and the block map stay as they were; `[Content_Types].xml` gets
  `<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>`;
  the signature is added last, deflated, with its sizes in the local header (version needed 2.0, no
  data descriptor); the central directory is written the ZIP32 way (no ZIP64 extra fields) where
  sizes and offsets allow, and the end record's disk numbers are 0.
- `AppxSignature.p7x` is `PKCX` followed by a CMS SignedData as in [authenticode.md](authenticode.md),
  with these differences: `data` is `SEQUENCE { SpcSipInfo (1.3.6.1.4.1.311.2.1.30), SEQUENCE {
  INTEGER 0x01010000, OCTET STRING <SIP GUID>, INTEGER 0, INTEGER 0, INTEGER 0, INTEGER 0,
  INTEGER 0 } }` with the package SIP GUID bytes `4B DF C5 0A 07 CE E2 4D B7 6E 23 C8 39 A0 9F D1`
  or the bundle SIP GUID bytes `B3 58 5F 0F DE AA 9A 4B A4 34 95 74 2D 92 EC EB`; the signed
  attributes are `contentType` and `messageDigest` only.
- The DigestInfo's digest is not one hash but `APPX` followed by records of a 4-byte tag and a
  SHA-256:
  - `AXPC`: the archive from its start to the signature entry's local header;
  - `AXCD`: the central directory without the signature entry, then the ZIP64 end record, its
    locator and the end record, all as if the signature entry did not exist (the central directory
    starting where the signature's local header is). Windows reads the signature entry's place
    from its ZIP32 fields: a signature entry described the ZIP64 way (0xFFFFFFFF there) is a
    `HashMismatch` - so is the package whose central directory is rewritten that way after signing;
  - `AXCT`: `[Content_Types].xml` (the plain bytes); `AXBM`: `AppxBlockMap.xml`;
  - `AXCI`: `AppxMetadata/CodeIntegrity.cat`, only when the package has one.
- Windows' signer also adds `AppxMetadata/CodeIntegrity.cat` - a catalog of the package's program
  files, signed by the same key - when the package holds any. It is optional: a package signed
  without it installs and runs. rubrapack does not write one; it checks `AXCI` when it is there.
- A bundle: every package inside is signed first (each with its own `AppxSignature.p7x`), then the
  bundle is written around them and signed with the bundle SIP GUID, without `AXCI`.
- An unsigned package that needs `-AllowUnsigned` and the publisher OID (below) is not signed:
  signing refuses a publisher with that OID.

## Installing an unsigned package

- `Publisher` must end in `OID.2.25.311729368913984317654407730594956997722=1`; then
  `Add-AppxPackage -AllowUnsigned` installs it on Windows 11. [spec]
- A package with an executable needs an elevated process (otherwise 0x80073D2B: an unsigned
  package cannot hold an executable activation). Developer mode is not needed. [observed]
- `Add-AppxPackage` from a network logon (an SSH session) fails at "PLM initialization" with
  0x80070005; it works in an interactive session. [observed]
- A package with file types leaves, after removal, an empty `OpenWithProgids` key under
  `HKCU\Software\Classes\.<ext>` - Windows' own doing, not the package's. [observed]

## Worked example: the tutorial's hello.msix

The MSIX of tutorial chapter 17 (`--unsigned-test`, x64) is 11009 bytes. It begins with the local
header of its first file, `hello.exe`:

| Offset | Bytes | Field | Value |
|---|---|---|---|
| `0x00` | `50 4b 03 04` | signature | `PK\3\4` |
| `0x04` | `2d 00` | version needed | 45 (4.5, ZIP64) |
| `0x06` | `08 00` | flags | 0x0008: sizes follow the data |
| `0x08` | `08 00` | method | 8 (deflate) |
| `0x0A` | `00 00 21 00` | time, date | 1980-01-01 00:00 |
| `0x0E` | `00 00 00 00 00 00 00 00 00 00 00 00` | CRC, sizes | 0 (in the data descriptor) |
| `0x1A` | `09 00 00 00` | name, extra length | 9, 0 |
| `0x1E` | `68 65 6c 6c 6f 2e 65 78 65` | name | `hello.exe` |

The 6706 deflated bytes of `hello.exe` follow, then its data descriptor: `50 4b 07 08 ac a3 c6 2e 32
1a 00 00 00 00 00 00 00 46 00 00 00 00 00 00` - `PK\7\8`, the CRC-32 `2EC6A3AC`, and the compressed
and plain sizes as 8 bytes each (6706, 17920).

Its entry in `AppxBlockMap.xml`:

```xml
<File Name="hello.exe" Size="17920" LfhSize="39">
  <Block Hash="K7HbHzXLkhbwTo3dcUr0vs35DlYX27qMOexDOTR8e2k=" Size="6704"/>
```

The file is smaller than 64 KiB, so it is one block. Its `Hash` is the base64 of the SHA-256 of the
17920 plain bytes (`K7HbHzXLkhbwTo3dcUr0vs35DlYX27qMOexDOTR8e2k=` computed here), `Size` the 6704
bytes of its deflate part - the compressed size 6706 minus the 2-byte final block - and `LfhSize`
the local header's 39 bytes (30 + the 9-byte name).
