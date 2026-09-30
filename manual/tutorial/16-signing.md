# Signing and timestamps

Goal: sign `hello.msi` so that Windows shows "Example Software" as its publisher instead of
"Unknown publisher", and so that nobody can change it unnoticed - with a key file, with a key in the
Windows certificate store, or with a hardware token.

## What a signature is

A *digital signature* is a number computed from the file's contents and a secret *private key*.
Anyone can check it with the matching *public key*, which is inside a *certificate*: a small
document, itself signed by a *certificate authority* (CA), that says "this public key belongs to
Example Software". Windows trusts a list of CAs; a certificate from one of them makes your name
appear in the User Account Control prompt. If one byte of the package changes, the signature no
longer matches. Part III explains [hashes](../basics/04-guids-and-hashes.md#hashes) and [keys, certificates and signatures](../basics/08-keys-certificates-signatures.md) step by step.

Where the certificate comes from:

- **For real users**: a *code signing certificate* bought from a CA. Since 2023 the private key must
  live in hardware - a USB token or a cloud service - and never in a file. rubrapack signs through
  such a token (PKCS#11) or through the Windows certificate store, below.
- **For learning and testing**: a certificate you make yourself. Windows does not trust it (the
  prompt still says "Unknown publisher" unless you add it to the trusted list on the test
  computer), but everything else works the same. In PowerShell on Windows:

```text
PS> New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Example Software" -CertStoreLocation Cert:\CurrentUser\My
```

It prints the certificate's *thumbprint*, 40 hexadecimal digits that name it.

## Signing with a key file

A key file holds the private key and the certificate, protected by a password: a `.pfx` (also
called `.p12`), as Windows exports it (Export-PfxCertificate with
`-CryptoAlgorithmOption AES256_SHA256`), or a PEM key. The password is never typed on the command
line (other programs could read it there): it comes from an environment variable or a file. The
options that would take one are there only to say so:

```text
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass secret
rubrapack: error[RP0004]: a password is never taken on the command line (others can see it); use --pass-env or --pass-file
```

(`--pin` for a token's PIN answers the same way.)

```text
C:\work\hello> set SIGN_PASS=...the password...
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass-env SIGN_PASS -o hello-signed.msi
rubrapack: warning[RP0011]: 'hello.msi': no --timestamp: the signature stops being valid when the certificate expires
signed hello-signed.msi
```

Without `-o` the file is signed in place. A file that is signed already is refused - sign the
unsigned file again instead. The certificate must be one for code signing, not a CA's, and valid
today. `--pass-file pass.txt` reads the password from a file instead. `--cert chain.pem` adds
certificates the key file does not hold (the CA's intermediate certificates, which Windows needs
to connect your certificate to a trusted root).

Signing while building does the same in one step:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi --key signer.pfx --pass-env SIGN_PASS
```

`sign` also signs programs (`.exe`, `.dll`) - sign `hello.exe` before you build, so the program
itself carries your name too - and MSIX packages (next chapter).

## Timestamps: signatures that outlive the certificate

A certificate is valid for a year or three. The warning above says what happens after that: the
signature no longer counts. A *timestamp* fixes it: a timestamp server (run by the CAs, free to use)
signs "this signature existed at this time", and the signature then stays valid for good.

```text
C:\work\hello> rubrapack sign hello.msi --key signer.pfx --pass-env SIGN_PASS --timestamp http://timestamp.digicert.com
```

Always timestamp what you publish. Options for unusual networks: `--proxy http://host:port`,
`--tls-trust` (certificates to trust for an `https://` timestamp server), `--system-roots` (trust
the computer's own root certificates for that connection), `--tsa-trust` (certificates to trust
for the timestamp itself).

## A key in the Windows certificate store

The self-made certificate above is in your certificate store. Sign with it by thumbprint - on
Windows only:

```text
C:\work\hello> rubrapack keys list
CurrentUser\My	thumbprint=1a2b...	RSA	publisher=CN=Example Software	until=...
C:\work\hello> rubrapack sign hello.msi --key-store 1a2b...
```

`--machine-store` looks in the computer's store (LocalMachine) instead of yours. Keys on a USB
token whose driver plugs into Windows also appear here.

## A hardware token: PKCS#11

A token's maker supplies a PKCS#11 *module* - a DLL (or `.so` on Linux) that speaks for the token.
rubrapack talks to it directly:

```text
C:\work\hello> rubrapack keys list --pkcs11 C:\token\pkcs11.dll --pin-env TOKEN_PIN
C:\work\hello> rubrapack sign hello.msi --pkcs11 C:\token\pkcs11.dll --key-label "Code Signing" --pin-env TOKEN_PIN --timestamp http://timestamp.digicert.com
```

`--key-label` picks the key by its label, `--token-label` the token when several are plugged in,
`--pin-env`/`--pin-file` give the PIN (never on the command line).

## Checking a signature: `verify`

```text
C:\work\hello> rubrapack verify hello-signed.msi --trust ca.pem
hello-signed.msi
  structure:  ok
  digest:     ok
  signature:  ok
  chain:      trusted
  revocation: not-checked
  timestamp:  not-present
```

- `structure`: the signature is where it belongs; `digest`: the contents are unchanged;
  `signature`: it was made with the certificate's key;
- `chain`: the certificate leads to a certificate you trust - given with `--trust`, or
  `--system-roots` for the computer's trusted roots. Without either, verify says it cannot tell
  (and exits with 4);
- `timestamp`: present and valid, or not.

An unsigned file says `structure: invalid ... the package is not signed`.

## External cabinets are not covered

A signature covers the `.msi` only. With `cab = "external"` (chapter 15) someone could swap the
`.cab` files unnoticed, so `sign` refuses such a package:

```text
C:\work\hello\out-x64> rubrapack sign hello-x64.msi --key signer.pfx --pass-env SIGN_PASS
rubrapack: error[RP0011]: 'hello-x64.msi': the package uses cabinets outside itself, which a signature of the .msi does not cover; build it with embedded cabinets, or sign anyway with --allow-unsigned-cabs
```

`--allow-unsigned-cabs` signs the `.msi` anyway, with a warning. Prefer embedded cabinets for
signed packages.

## What happened inside

An MSI's signature is a hidden stream, `\005DigitalSignature`, holding a PKCS#7 structure: the
hash of every other stream, your certificate chain, and the signature over it. Part IV
(Authenticode) shows how the hash is computed and where each part lies.
