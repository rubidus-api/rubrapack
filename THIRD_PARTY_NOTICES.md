# Third-Party Notices

## Vendored code

- `vendor/proven/` (planned): proven_c_lib, MIT License, same author. Its own `LICENSE`
  travels with the copy.

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

Code ported from lowent_lang (MIT, same author) is marked in the source file header
and listed here when it lands. Planned ports: DER, X.509, big integers, RSA verification,
P-256/P-384, ECDSA, HMAC, SHA-512, AES, GCM/AEAD, X25519, TLS 1.3 client.
