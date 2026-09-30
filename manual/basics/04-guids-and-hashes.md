# GUIDs and hashes

Two kinds of 16-to-32-byte numbers appear everywhere in packages: **GUIDs**, which name things so
that no two names ever collide, and **hashes**, which sum up a file's contents so that any change
shows. rubrapack also joins the two: it makes GUIDs *from* hashes.

## GUIDs

A **GUID** (globally unique identifier; the standards call it a UUID) is a 128-bit number - 16
bytes - written as 32 hex digits in five groups, usually in braces:

```text
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}
 8 digits  4    4    4    12 digits
```

Windows uses GUIDs to name things that must never be confused with anything else, anywhere: a
product (the product code), a product family across versions (the upgrade code), a component, a
package build (the package code), a kind of COM object. There is no registry of GUIDs; nobody hands
them out. They are unique because there are so many: 2^122 possible random ones, about 5 x 10^36.
A computer making a billion random GUIDs every second would need about 86 years before the chance
that any two of them were equal reached one half.

### Version and variant

Not all 128 bits are free. Two places in the text say what kind of GUID it is:

```text
{98AE4FED-BF0B-4C77-B712-E0649EB47178}      rubrapack guid          (random)
               ^    ^
               |    variant: 8, 9, A or B (the bits 10xx)
               version: 4 = random

{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}      rubrapack guid --from hello   (derived)
               ^    ^
               |    variant: 9
               version: 8 = made by a method of the maker's own choosing
```

A **version 4** GUID is 122 random bits: `rubrapack guid` makes one for your upgrade code. A
**version 8** GUID is 122 bits computed in a way its maker defines - rubrapack computes them from
names, below.

### In text and in bytes

In text, a GUID is its 32 digits in order. In a binary structure (a Compound File's directory, a
program file), Windows stores the first three groups as little-endian numbers (chapter 2) and the
last two as plain bytes:

```text
  text:   B52FE8EF - B68D - 84B5 - 91AF - E6E01BEC2773
  bytes:  ef e8 2f b5  8d b6  b5 84  91 af  e6 e0 1b ec 27 73
          (reversed)  (rev.) (rev.) (as written)
```

MSI tables keep GUIDs as text, in upper case, with braces. The mixed order matters only in Part IV.

## Hashes

A **hash function** reads any amount of data and produces a short, fixed-size number from it, the
**hash** (or *digest*). A good one has three properties:

1. **Same input, same hash** - on every computer, every time.
2. **Any change changes everything.** Change one bit of the input and about half the bits of the
   hash change, with no visible pattern.
3. **One-way.** From a hash you cannot find an input that gives it, except by trying inputs
   forever.

Take the tutorial's `guide.txt`, six bytes `47 75 69 64 65 0a` ("Guide" and a new line), and the
same with a lower-case `g`, one bit different (`47` = `0100 0111`, `67` = `0110 0111`):

| Input | MD5 (16 bytes) |
|---|---|
| `Guide\n` | `2b 67 39 28 1f 93 72 44 2d f3 0e 78 4b 7d bd c7` |
| `guide\n` | `e5 24 11 d5 57 3e b8 9b 5c 31 26 97 79 50 d3 ef` |

| Input | SHA-256 (32 bytes) |
|---|---|
| `Guide\n` | `32 74 fc ad 88 6c de 4e 2c a8 6b 11 d3 0f d7 c4 48 58 ea df 1c 43 7a 95 83 f3 1e 78 15 db 1a f6` |
| `guide\n` | `90 c3 90 ec 1d e8 06 bf 94 58 85 cd 0a f5 1e 90 c3 cd 8c da 0d 0f f6 76 05 1a 56 c2 08 48 c9 0f` |

Try it in PowerShell: `Get-FileHash -Algorithm MD5 dist\docs\guide.txt`.

The common hash functions:

| Name | Size | Use today |
|---|---|---|
| MD5 | 128 bits | only to notice changes; it is broken for security (different inputs with the same MD5 can be made on purpose) |
| SHA-1 | 160 bits | the same; still used as a certificate's *thumbprint* (chapter 8) |
| SHA-256 | 256 bits | the standard for signatures, and what rubrapack uses to derive GUIDs |

### Hashes in the tutorial's packages

**`MsiFileHash`**: for each file without a version, Windows Installer keeps its MD5 to decide at
a repair or upgrade whether the file on disk still is the one the package installed. `inspect
--files` prints it as hex (tutorial chapter 18). The table itself stores it as four 4-byte
numbers - the sixteen bytes read four at a time, little-endian and signed (chapter 2):

```text
C:\work\hello> rubrapack inspect hello.msi MsiFileHash
File_	Options	HashPart1	HashPart2	HashPart3	HashPart4
...
F_6d1508f52f6615459567	0	674850603	1148359455	2014245677	-943882933

2b 67 39 28 -> 0x2839672B =  674850603
1f 93 72 44 -> 0x4472931F = 1148359455
2d f3 0e 78 -> 0x780EF32D = 2014245677
4b 7d bd c7 -> 0xC7BD7D4B = 3351084363, top bit set, minus 2^32 = -943882933
```

- **MSIX block map** (tutorial chapter 17): the SHA-256 of every 64 KiB block of every file.
- **Signatures** (tutorial chapter 16): what is signed is a hash of the package, not the package.
- **`--reproducible`** (tutorial chapter 15): the package code is derived from a hash of the
  content.

## GUIDs from hashes: `guid --from`

A component's GUID must stay the same from one version to the next - the upgrade relies on it -
but nobody should have to write hundreds of them down. So rubrapack *derives* each one: it hashes a
text that names the thing (the product and the file's place) and turns the hash into a GUID. The
same text always gives the same GUID; different texts give different GUIDs.

`rubrapack guid --from <text>` shows the calculation's result for any text. Step by step, for
`hello`:

**1. Build the input bytes.** Each part is its length as a 4-byte little-endian number, then its
bytes: the word `guid`, the number 1 (the method's version), and the text in UTF-8.

```text
04 00 00 00  67 75 69 64  01 00 00 00  05 00 00 00  68 65 6c 6c 6f
length 4     "guid"       1            length 5     "hello"
```

Writing the lengths makes the parts unambiguous: `guid` + `hello` cannot be confused with
`guidh` + `ello`.

**2. Hash them with SHA-256:**

```text
b5 2f e8 ef b6 8d f4 b5 d1 af e6 e0 1b ec 27 73 a2 8a 56 80 0f 9f 53 1b cd dc ee 7c f5 c8 59 90
```

**3. Keep the first 16 bytes:** `b5 2f e8 ef b6 8d f4 b5 d1 af e6 e0 1b ec 27 73`.

**4. Mark the version and variant.** The top four bits of byte 6 become `1000` (version 8): `f4`
becomes `84`. The top two bits of byte 8 become `10` (the variant): `d1` = `1101 0001` becomes
`1001 0001` = `91`.

```text
b5 2f e8 ef b6 8d 84 b5 91 af e6 e0 1b ec 27 73
                  ^^    ^^
```

**5. Write it as a GUID**, the bytes in order, upper case:

```text
C:\work\hello> rubrapack guid --from hello
{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}
```

Any program can repeat this and get the same GUID; Part IV,
[Deterministic identities](../formats/identity.md), lists the exact texts rubrapack hashes for each
kind of thing.

## Where this is used

- Tutorial chapter [2](../tutorial/02-a-first-installer.md): the upgrade code, made with
  `rubrapack guid`.
- Tutorial chapters [4](../tutorial/04-files-and-folders.md) and
  [15](../tutorial/15-cabinets-and-architectures.md): component GUIDs and the package code, derived.
- Tutorial chapters [16](../tutorial/16-signing.md), [17](../tutorial/17-msix-and-bundles.md),
  [18](../tutorial/18-checking-and-looking-inside.md): signatures, block maps, file hashes.
- Part IV: [Deterministic identities](../formats/identity.md),
  [Authenticode](../formats/authenticode.md), [MSIX](../formats/msix.md).
