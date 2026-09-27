# MSIX packages

What a program has to write so that Windows reads, installs and runs an MSIX package. Facts are
tagged as in [README.md](README.md): **[spec]** for Microsoft Learn (package manifest and block
map schemas) and ECMA-376 Part 2 (Open Packaging Conventions); **[observed]** for packages written
by Windows' own packaging API (`IAppxFactory`/`IAppxPackageWriter` in AppxPackaging.dll, part of
Windows) and for rubrapack's packages read back through `IAppxPackageReader` and installed with
`Add-AppxPackage` on Windows 11. rubrapack writes what is described here; signing and extensions
are not covered yet.

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

## Installing an unsigned package

- `Publisher` must end in `OID.2.25.311729368913984317654407730594956997722=1`; then
  `Add-AppxPackage -AllowUnsigned` installs it on Windows 11. [spec]
- A package with an executable needs an elevated process (otherwise 0x80073D2B: an unsigned
  package cannot hold an executable activation). Developer mode is not needed. [observed]
- `Add-AppxPackage` from a network logon (an SSH session) fails at "PLM initialization" with
  0x80070005; it works in an interactive session. [observed]
