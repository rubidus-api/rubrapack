# Authenticode signatures

What a program has to write so that Windows accepts a signed PE file (`.exe`, `.dll`), and, once
rubrapack signs them, an MSI package. Facts are tagged as in [README.md](README.md): **[spec]**
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

## What Windows reports

`Get-AuthenticodeSignature` distinguishes the cases a signer needs: `Valid` (digest, signature and
chain to a trusted root), `UnknownError` with "the certificate chain ... not trusted" (digest and
signature are right, the root is not trusted on this machine), `HashMismatch` (the file changed
after signing), `NotSigned`. So a signature can be checked for correctness before any root is
trusted. [observed]
