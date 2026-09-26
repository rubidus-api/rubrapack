# MSIX (planned - not verified yet)

rubrapack will write MSIX packages later. Until the writer exists and its output has been
installed on Windows, this page only lists the parts an MSIX package consists of, from Microsoft
Learn and ECMA-376 Part 2. Nothing here has been tested by rubrapack yet.

- A ZIP archive (PKWARE APPNOTE) following the Open Packaging Conventions (ECMA-376 Part 2):
  file names in UTF-8 with the language-encoding flag.
- `AppxManifest.xml` - package identity (name, publisher, four-part version, processor
  architecture), properties, dependencies (target device family and minimum version), resources,
  applications, capabilities.
- `AppxBlockMap.xml` - every file split into 64 KiB blocks, each block's SHA-256 in base64 and,
  for deflated files, its compressed size.
- `[Content_Types].xml` - media types by extension and override.
- `AppxSignature.p7x` when signed.
- Optionally `Registry.dat` (a registry hive) for virtual registry entries, and files under
  `VFS/...` for well-known folders.
- `.msixbundle`: several architecture packages plus `AppxMetadata/AppxBundleManifest.xml`.

The details - exact ZIP layout rules, block map hashing of deflated files, hive format - will be
documented here with the same [spec]/[observed] tags once they are implemented and verified.
