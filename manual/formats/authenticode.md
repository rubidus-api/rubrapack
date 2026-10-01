# Authenticode signatures

What a program has to write so that Windows accepts a signed PE file (`.exe`, `.dll`) or MSI
package. Facts are tagged as in [README.md](README.md): **[spec]**
for Microsoft's "Windows Authenticode Portable Executable Signature Format", the PE/COFF
specification and RFC 5652 (CMS); **[observed]** for files signed by Windows' own signer
(PowerShell `Set-AuthenticodeSignature`, which uses `mssign32.dll`) and checked with
`Get-AuthenticodeSignature` / `WinVerifyTrust` on Windows 11.

## Where the signature lives in a PE file

- The certificate table: data directory entry 4 (its entry is 8 bytes: file offset, size) of the
  optional header. The directory starts at optional header + 96 for PE32 (magic 0x10B) and
  + 112 for PE32+ (magic 0x20B); `NumberOfRvaAndSizes` must be at least 5. Unlike every other
  directory entry, its "address" is a **file offset**, not an RVA. [spec]
- At that offset, at the end of the file: `WIN_CERTIFICATE { u32 dwLength, u16 wRevision = 0x0200,
  u16 wCertificateType = 0x0002 (PKCS_SIGNED_DATA), bCertificate[] }`, where `bCertificate` is a
  DER CMS `ContentInfo`. `dwLength` includes the 8-byte header and is rounded up to 8; the extra
  bytes are zero. The directory entry's size equals `dwLength`. [spec] [observed]
- Before the table is appended, the file is padded with zeros to a multiple of 8. The padding is
  hashed (it is "data after the sections", below). [spec]
- The optional header's `CheckSum` (optional header + 64) is updated. Windows' signer adjusts the
  existing value rather than recomputing it: for a file whose checksum was not the PE algorithm's
  value, Windows keeps the difference, a fresh computation does not. `CheckSum` is outside the
  digest, so both verify. [observed]

## The PE digest

In this order, with SHA-256: [spec]

1. The headers, from 0 to `SizeOfHeaders`, without `CheckSum` (4 bytes) and without the
   certificate table directory entry (8 bytes).
2. Every section with `SizeOfRawData > 0`, in increasing `PointerToRawData` order, `SizeOfRawData`
   bytes from `PointerToRawData`.
3. The data after them: from `SizeOfHeaders + sum(SizeOfRawData)` to the end of the file minus
   the certificate table.

Hashing the file linearly with the two holes gives the same result only when the sections follow
each other without gaps; the steps above are the definition.

## The CMS SignedData

Windows' signer writes, and accepts: [observed]

```text
ContentInfo { signedData (1.2.840.113549.1.7.2), [0] SignedData {
  version 1,
  digestAlgorithms SET { sha256 + NULL },
  encapContentInfo { SPC_INDIRECT_DATA (1.3.6.1.4.1.311.2.1.4),
                     [0] SpcIndirectDataContent }          -- the SEQUENCE itself, no OCTET STRING
  certificates [0] { the signer's certificate, then intermediates; no self-signed root },
  signerInfos SET { SignerInfo {
    version 1, IssuerAndSerialNumber of the signer,
    digestAlgorithm sha256 + NULL,
    signedAttrs [0] { SpcSpOpusInfo, contentType, SpcStatementType, messageDigest },
    signatureAlgorithm rsaEncryption + NULL,
    signature (RSA PKCS#1 v1.5) } } } }
```

- `SpcIndirectDataContent ::= SEQUENCE { data, messageDigest DigestInfo }`. For a PE file `data` is
  `SEQUENCE { SPC_PE_IMAGE_DATA (1.3.6.1.4.1.311.2.1.15), SpcPeImageData }` and Windows writes
  `SpcPeImageData` as `30 09 03 01 00 A0 04 A2 02 80 00`: no flags, and a file link that is an
  empty Unicode string. `messageDigest` holds the PE digest. [observed]
- The **messageDigest attribute** is the hash of the `SpcIndirectDataContent` contents **without**
  its SEQUENCE tag and length - not of the whole element, and not of the PE file. [observed]
- The signed attributes are stored with tag `[0]` (0xA0) but the signature is computed over the
  same bytes with tag SET (0x31), sorted as a DER SET OF. The four attributes Windows writes:
  `SpcSpOpusInfo` (1.3.6.1.4.1.311.2.1.12) = `SEQUENCE { [0] { [0] "" }, [1] { [0] "" } }`,
  `contentType` = SPC_INDIRECT_DATA, `SpcStatementType` (1.3.6.1.4.1.311.2.1.11) =
  `SEQUENCE { individual code signing (1.3.6.1.4.1.311.2.1.21) }`, `messageDigest`. No signing
  time. [observed]
- RSA PKCS#1 v1.5 is deterministic: a program that writes exactly this structure with the same
  key produces the same bytes as Windows. rubrapack does, except the `CheckSum` difference above.
  [observed]

## MSI packages

Microsoft does not publish how an MSI package is hashed. The rules below were worked out by
signing packages with Windows' signer and comparing: rubrapack's signature and Ex streams came out
byte-identical to Windows' for the same package and key, and packages whose directory entries were
given non-zero state bits, times and CLSIDs told the fields apart. [observed]

- The signature is the stream `\005DigitalSignature` at the root of the compound file: the same
  CMS `ContentInfo` as for a PE file, with `data` = `SEQUENCE { SPC_SIPINFO (1.3.6.1.4.1.311.2.1.30),
  SpcSipInfo }`, where Windows writes `SpcSipInfo` as `SEQUENCE { 2, OCTET STRING <the MSI SIP GUID
  {000C10F1-0000-0000-C000-000000000046} in its little-endian byte order>, 0, 0, 0, 0, 0 }`.
- Windows also writes `\005MsiDigitalSignatureEx`: 32 bytes, the SHA-256 of a "prehash" of the
  compound file's directory:
  - the root entry's CLSID (16 bytes) and state bits (4 bytes, little-endian);
  - then, for every stream at the root except the two signature streams, in the order of their
    names compared as UTF-16LE bytes: the name (UTF-16LE, no terminating NUL), the stream size as
    8 bytes little-endian, and the entry's creation and modification times as stored (8 + 8
    bytes). A stream's CLSID and state bits are not part of it.
  The root's own times are not part of it (Windows sets the root's modification time when it
  saves the signed file).
- The digest in `SpcIndirectDataContent` is the hash of: the 32-byte `MsiDigitalSignatureEx`
  value (when that stream is written), then every stream's contents in the same name order
  (again without the two signature streams), then the root CLSID.
- A storage (a patch's transforms are storages; so are embedded transforms and sub-databases)
  takes part at its place in the same name order, compared with the streams around it:
  - in the prehash, its name, its CLSID (16 bytes), its state bits (4 bytes) and its two times
    (16 bytes) - no size - followed at once by the prehash entries of its own children, in their
    name order, the same way down;
  - in the digest, the contents of its children in their name order, the same way down, then its
    CLSID - as the root's CLSID ends the whole.
  Worked out against patches signed by Windows' signtool (one written by rubrapack, one by
  Microsoft's MsiMsp.exe, whose storages carry non-zero times): this is the one layout among the
  candidates tried that gives both stored `MsiDigitalSignatureEx` values, and with it both digests
  match. Only storages directly at the root were in those patches.
- A signature covers what is inside the `.msi` only: cabinets outside it (Media table `Cabinet`
  values that do not start with `#`) are not part of the digest.

## Timestamps

A signature without a timestamp stops being valid when the signing certificate expires. With an
RFC 3161 timestamp, Windows checks the certificate at the stamped time instead. What
`mssign32!SignerTimeStampEx2` with `SIGNER_TIMESTAMP_RFC3161` writes: [observed]

- The request's message imprint is the hash (here SHA-256) of the SignerInfo's `signature` value:
  the OCTET STRING's contents, without its tag and length.
- The server's `TimeStampToken` (a CMS SignedData over a `TSTInfo`, RFC 3161) goes into the
  SignerInfo as its unsigned attributes, after the signature:
  `[1] { SEQUENCE { 1.3.6.1.4.1.311.3.3.1, SET { TimeStampToken } } }`.
- Nothing else in the signature changes: the signed attributes, the signature and the outer
  certificates are the same bytes as before; the server's certificates travel inside the token.
  For an MSI package the `\005DigitalSignature` stream grows accordingly (from the mini stream to
  regular sectors, for a token of a few kilobytes); `\005MsiDigitalSignatureEx` and the digest do
  not change, since the signature streams are not part of them.
- rubrapack writes exactly this; with the same key and token its PE file is byte-identical to
  Windows', and so is the MSI signature stream.

`Set-AuthenticodeSignature -TimestampServer` writes an older form instead: a PKCS#9
counterSignature attribute (1.2.840.113549.1.9.6) and the server's certificates added to the outer
certificates. Windows accepts both. [observed]

## ECDSA

An ECDSA P-256 signer works for PE files, MSI packages and MSIX packages alike (Windows 11 26100
calls all three `Valid` with the root trusted). Windows' signer writes the SignerInfo's
`signatureAlgorithm` as `id-ecPublicKey` (1.2.840.10045.2.1) with NULL parameters - not
`ecdsa-with-SHA256` - and the signature value as the DER `SEQUENCE { INTEGER r, INTEGER s }`.
rubrapack writes the same, with deterministic nonces (RFC 6979), and reads either OID. [observed]

## What Windows reports

`Get-AuthenticodeSignature` distinguishes the cases a signer needs (and names the time-stamping
certificate in `TimeStamperCertificate` when there is a timestamp): `Valid` (digest, signature and
chain to a trusted root), `UnknownError` with "the certificate chain ... not trusted" (digest and
signature are right, the root is not trusted on this machine), `HashMismatch` (the file changed
after signing), `NotSigned`. So a signature can be checked for correctness before any root is
trusted. [observed]

## Worked example: the tutorial's hello.msi

The digest of the tutorial's first package, computed here from the rules above without any key - a
signature made with any key carries these same values. The streams in order of their UTF-16LE names:

| # | name (UTF-16LE) | Name | Size |
|---|---|---|---|
| 1 | `05 00 53 00 75 00` ... | `\005SummaryInformation` | 336 |
| 2 | `26 41 65 38 be 41` ... | `cab1.cab` | 6776 |
| 3 | `40 48 0c 46 f6 45` ... | `CustomAction` | 8 |
| 4 | `40 48 0d 43 35 42` ... | `Directory` | 18 |
| 5 | `40 48 0f 42 e4 45` ... | `FeatureComponents` | 4 |
| 6 | `40 48 0f 42 e4 45` ... | `Feature` | 16 |
| 7 | `40 48 0f 43 2f 42` | `File` | 20 |
| 8 | `40 48 16 42 27 43` ... | `Media` | 14 |
| 9 | `40 48 3f 3b f2 43` ... | `_Columns` | 632 |
| 10 | `40 48 3f 3f 77 45` ... | `_StringData` | 1544 |
| 11 | `40 48 3f 3f 77 45` ... | `_StringPool` | 532 |
| 12 | `40 48 52 44 f6 45` ... | `InstallExecuteSequence` | 120 |
| 13 | `40 48 52 44 f6 45` ... | `InstallUISequence` | 42 |
| 14 | `40 48 59 45 f2 44` ... | `Property` | 40 |
| 15 | `40 48 7f 3f 64 41` ... | `_Tables` | 34 |
| 16 | `40 48 8c 44 f0 44` ... | `Component` | 12 |
| 17 | `40 48 ca 41 30 43` ... | `AdminExecuteSequence` | 48 |
| 18 | `40 48 ca 41 30 43` ... | `AdminUISequence` | 24 |
| 19 | `40 48 ca 41 f9 45` ... | `AdvtExecuteSequence` | 42 |
| 20 | `40 48 de 44 6a 45` ... | `Upgrade` | 32 |
| 21 | `40 48 ff 3f e4 43` ... | `_Validation` | 1896 |

The prehash is the root's CLSID and state bits (20 bytes) and, per stream, its name, 8-byte size and
two zero times: 840 bytes in all. Its SHA-256, the `\005MsiDigitalSignatureEx` value:

```text
7b c2 c3 9e de 9d 47 98 32 4e 65 02 11 52 57 d4 8b b9 ad d8 49 66 86 d8 bd 57 c9 56 a3 ae d9 b8
```

The digest in `SpcIndirectDataContent` - SHA-256 over that value, every stream's bytes in the same
order, and the root CLSID:

```text
6b df 8f 15 39 b2 dd 24 63 b7 d7 e4 53 cb 0b dd 9d eb 54 da 13 aa b9 db cb d8 a9 cf 49 5e 22 90
```

