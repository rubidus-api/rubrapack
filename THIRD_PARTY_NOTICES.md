# Third-Party Notices

## Vendored code

- `vendor/proven/`: proven_c_lib v0.1.1, MIT License, same author. Its own `LICENSE` and
  notices travel with the copy; `vendor/proven/VENDORED.md` records the snapshot.

## Prebuilt helper DLLs

`resources/bin/rubrapack_ca-{x64,x86,arm64}.dll` are built from `src/ca/` by `./nob parts` with
llvm-mingw 20260922 (LLVM, Apache-2.0 with LLVM exception; mingw-w64 runtime, permissive licenses:
its startup code and import libraries are linked into the DLLs). They import only Windows
components (kernel32, advapi32, msi and the Universal CRT). `resources/bin/SHA256SUMS` lists their
hashes; the same toolchain rebuilds them byte for byte.

## Fonts in the published manual

`docs/fonts/` (the web edition of the manual) holds subsets of Noto Serif, Noto Sans, Noto Sans
Mono, Noto Serif CJK KR, Noto Sans CJK KR (Google, Adobe) and D2Coding (NAVER), all under the SIL
Open Font License 1.1, which permits subsetting and redistribution; each subset keeps its font's
copyright and licence records, and `docs/fonts/README.md` names the sources. The PDF editions embed
the same fonts. The fonts are not part of the program.

The web edition's page design and script follow the Proven C Book's web edition (same author, MIT).

## Unicode data

`src/text/nfc_tables.c` holds normalization data generated from the Unicode Character Database
17.0.0 (`UnicodeData.txt`, `DerivedNormalizationProps.txt`) by rubrapack's own table generator;
`src/text/nfc.c`, the code that uses it, is rubrapack's own. The data is used under this license:

```text
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
```

## Clean-room statement

rubrapack is MIT-licensed and its implementation is original. The formats it reads and
writes are implemented from public specifications (Microsoft Open Specifications, Microsoft
Learn documentation, IETF RFCs, PKWARE APPNOTE, ECMA-376) and from black-box testing
against Windows.

- No code is copied, ported, or derived from GPL, LGPL, MS-RL, or otherwise incompatible
  projects. In particular, no source, schema, dialog layout, UI text, or artwork was taken
  from WiX Toolset, msitools/libmsi/wixl, gcab, libgsf, Wine, osslsigncode, libmspack,
  cabextract, ms-compress, wimlib, 7-Zip, or MSIX Hero.
- MIT-licensed projects may be read to confirm format facts. No code is copied from them;
  if that ever changes, the copied part and its license are listed in this file.
- Apache-2.0 signing tools (jsign, relic) are used, if at all, only indirectly: a written
  description of a method is made first, and the implementation is written from that
  description alone. No code, names, or comments are taken from them.

## Code from the same author's other projects

Parts of the cryptography follow lowent_lang v1.3.0 (MIT License, same author and copyright
holder); each file says so in its header:

- `src/crypto/ecdsa.c` - ported from `lib/ecdsa.low` and `lib/p256.low` (deterministic nonces,
  P-256/P-384).
- `src/crypto/hash.c`, `include/rubrapack/crypto.h` - SHA-384/512 constants and block function
  ported from lowent_lang; SHA-256 is proven_c_lib's.
- `src/crypto/bn.c` (Montgomery arithmetic), `src/crypto/x509.c`, `include/rubrapack/der.h`
  (DER reading rules), `src/net/tls.c` (TLS 1.3 client structure) - written on the design of the
  corresponding lowent_lang modules.
- `src/crypto/rsa.c` - verification written anew; lowent_lang has the same algorithm.

AES, GCM, HMAC, X25519, PKCS#8/#12 and the signing code are new. `src/crypto/x25519.c` uses the
field representation of the public-domain TweetNaCl design (no code copied).
