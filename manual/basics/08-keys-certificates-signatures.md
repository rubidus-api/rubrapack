# Keys, certificates and signatures

Tutorial chapter 16 signs `hello.msi`. This chapter explains what that means: how a signature
proves who made a file and that nobody changed it, what a certificate is, and why a timestamp keeps
a signature valid after the certificate has expired.

## Two keys instead of one

The oldest kind of secret writing uses one key for both directions: whoever can lock can also
unlock. **Public-key cryptography** (1970s) uses a **key pair** instead: two numbers that belong
together, made at the same time.

- The **private key** stays secret with its owner - in a file protected by a password, or better
  inside a hardware token that never lets it out.
- The **public key** is given to everybody.

What one key does, only the other can check. For signatures that means: only the private key can
*make* a signature, and anybody with the public key can *check* it. Knowing the public key does not
help to find the private one - for today's sizes, not in the lifetime of the universe.

Two methods are in use for signing code:

| Method | Typical key size | Notes |
|---|---|---|
| RSA | 2048 or 3072 bits | the classic; rubrapack's test certificate uses RSA 2048 |
| ECDSA | 256 or 384 bits (P-256, P-384) | elliptic curves: much smaller keys for the same strength |

## Signing and checking

A signature is not made over the whole file - that would be slow for a 200 MB package - but over
its **hash** (chapter 4):

```text
  signing (the publisher):
    file ---hash---> digest ---private key---> signature      (stored in the file)

  checking (any user's computer):
    file ---hash---> digest'
    signature ---public key---> digest
    digest' = digest ?  yes: unchanged and signed by that key.  no: changed, or another key.
```

Because the hash changes completely when one bit of the file changes, no edit survives the check;
and because only the private key makes signatures that the public key accepts, nobody else can
sign a changed file in the publisher's name.

## Certificates: whose key is this?

A signature checks out against a public key - but whose key? Anyone can make a key pair and call
it "Example Software". A **certificate** answers that: a small document that states "this public
key belongs to *this* name", signed by a **certificate authority** (CA) that checked the name.

Here is a test certificate made for rubrapack's own tests, as `openssl x509 -text` shows it:

| Field | Value |
|---|---|
| Subject (who) | `CN=rubrapack test signer, O=rubrapack tests` |
| Issuer (who vouches) | `CN=rubrapack test root` |
| Valid from / until | 30 Sep 2026 / 27 Sep 2036 |
| Serial number | `71AB698C19EACB47D6A00D71A711CF7A0ACDB236` |
| Public key | RSA, 2048 bits |
| Basic constraints | `CA:FALSE` - it may not vouch for others |
| Key usage | Digital Signature |
| Extended key usage | Code Signing |
| Signature | sha256WithRSAEncryption, made with the issuer's private key |

rubrapack checks these before signing: the certificate must be for code signing, must not be a
CA's, and must be valid today (tutorial chapter 16).

### Names: CN, O, C

A subject is a list of parts, each a type and a value: `CN` (common name), `O` (organization),
`OU` (unit), `L` (city), `S` (state), `C` (country, two letters). The certificate stores them in an
order, and different programs write them in different orders. This one is stored `CN` first, then
`O`; Windows writes the parts from the last to the first:

```text
  as stored:          CN=rubrapack test signer, O=rubrapack tests
  as Windows writes:  O=rubrapack tests, CN=rubrapack test signer
```

An MSIX's `publisher` must be the Windows form, exactly (tutorial chapter 17) - which is why
rubrapack prints it for you to copy when they differ.

## Chains and trust

The test certificate was issued by "rubrapack test root", whose own certificate is signed by
itself - it is a **root**. Real certificates form a **chain**:

```text
  your code signing certificate    issued by   ->  an intermediate CA certificate
  the intermediate certificate     issued by   ->  a root CA certificate
  the root certificate             issued by   ->  itself
```

Windows keeps a list of root certificates it trusts, the **trusted root store**, maintained by
Microsoft. A signature is trusted when its chain ends at one of them. The intermediate certificates
travel inside the signature (`--cert` adds them), so Windows can build the chain without asking
anyone.

`rubrapack verify --trust ca.pem` checks against a root you name; `--system-roots` against the
computer's own list. A self-made certificate (tutorial chapter 16) has a chain that ends nowhere
Windows trusts - which is why it is fine for tests and useless for users.

### Revocation

If a private key is stolen, its CA **revokes** the certificate: it publishes the serial number in a
list (CRL) or answers queries about it (OCSP). Windows checks this when it can reach the network.
`rubrapack verify` reports `revocation: not-checked`: it does not go online by itself.

## Timestamps

A certificate expires - code signing certificates today after at most 39 months. What about all
the files signed with it? Without help, their signatures stop counting on that day.

A **timestamp** fixes this. At signing, rubrapack sends the signature's hash to a **timestamp
authority** (TSA), a server run by a CA. The TSA signs "this hash existed at this time" with its own
certificate and sends that back; it goes into the file next to the signature (RFC 3161). Later,
Windows checks: was the signing certificate valid *at the time the timestamp shows*? If yes, the
signature stays valid for good, long after the certificate expired.

```text
  signed 2026-10-01, certificate valid until 2029-10-01
  checked 2031-05-12:
    without a timestamp: certificate expired -> signature not valid
    with a timestamp:    valid on 2026-10-01 (the TSA says so) -> signature valid
```

The TSA never sees the file, only its hash.

## Where keys live: files and tokens

| Form | What it is |
|---|---|
| `.pfx` / `.p12` (PKCS #12) | one file with the private key, the certificate and often the chain, protected by a password. What Windows exports. |
| `.pem` | text: base64 of the binary form between lines `-----BEGIN CERTIFICATE-----` / `-----END ...-----` (or `PRIVATE KEY`). What Linux tools use. |
| `.der`, `.cer` | the same, in binary |
| Windows certificate store | keys kept by Windows (`certmgr.msc`), used by thumbprint (`--key-store`) |
| hardware token, cloud HSM | the private key never leaves the device; programs ask the device to sign (PKCS #11, `--pkcs11`) |

Since 2023 a CA issues code signing certificates only for keys in hardware, so for publishing you
will use the last two rows.

A certificate in binary (DER) is itself a nest of length-prefixed fields. The test certificate
begins `30 82 03 d8`: `30` means "a sequence follows", `82` "its length takes the next 2 bytes",
and `03 d8` is that length, 984 - big-endian this time (chapter 2). 984 bytes plus these four make
the whole 988-byte certificate.

### The thumbprint

A certificate's **thumbprint** is the SHA-1 hash (chapter 4) of its binary form - here
`2E AB FE 60 19 0A F6 6C 52 E4 29 31 AF A6 01 BE C0 EF 86 0D`. It is not a secret; it is a short
name for one certificate, which is how Windows' store and `rubrapack keys list` name them.

## Where this is used

- Tutorial chapter [16](../tutorial/16-signing.md): `sign`, `verify`, keys, timestamps.
- Tutorial chapter [17](../tutorial/17-msix-and-bundles.md): the MSIX publisher.
- Part IV: [Authenticode](../formats/authenticode.md) (what exactly is hashed and signed, and the
  structure Windows accepts), [checking against Windows](../formats/verify.md).
