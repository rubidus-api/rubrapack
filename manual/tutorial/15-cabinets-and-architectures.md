# Cabinets, compression, big packages, and several architectures

Goal: control how the files are packed - compression, one package file or package plus cabinets -
build Hello for x64, x86 and Arm64 from one source, and make builds byte-for-byte repeatable.

## The folder

The program now exists in three builds, one per processor type:

```text
dist\
    x64\hello.exe
    x86\hello.exe
    arm64\hello.exe
    docs\guide.txt
```

## The source

```toml
# tutorial 15: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
upgrade-code-x86 = "{7D1C2B3A-4E5F-4061-9728-3A4B5C6D7E8F}"
upgrade-code-arm64 = "{0E9F8D7C-6B5A-4948-8372-6150F4E3D2C1}"
compress = "mszip:9"
cab = "external"
cab-max-size = 500

[define]
VERSION = "2.0.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/$(ARCH)/hello.exe"

[files.DocFiles]
dir = "INSTALLDIR"
glob = "dist/docs/**"
```

## Cabinets and compression

Inside an MSI the files are not stored one by one: they are packed into a *cabinet* (`.cab`), the
compressed archive format Windows has used since the 1990s. Keys in `[package]`:

| Key | Values | Effect |
|---|---|---|
| `compress` | `mszip:6` (default), `mszip:0` .. `mszip:9`, `mszip` (= 6), `lzx:15` .. `lzx:21`, `lzx` (= 21), `none` | how to compress. For MSZIP 9 is smallest and slowest, 1 fastest; LZX is smaller still, with a window of 2^15 to 2^21 bytes; `none` stores the files as they are |
| `cab` | `embed` (default), `external` | the cabinet inside the `.msi`, or next to it as `hello-x64.cab` |
| `cab-max-size` | a number of MiB | start a new cabinet after that much data (`-1.cab`, `-2.cab`, ...) |

For most programs the defaults are right: one `.msi` holding everything. External cabinets are for
very large products: **an MSI of 2 GiB or more cannot be opened by Windows Installer**, so a product
that big must use `cab = "external"` (rubrapack refuses the embedded form with `RP1516`), and the
`.msi` and its `.cab` files must then be kept together - in the same folder, on the same share.
Without `cab-max-size`, external cabinets are split below 2 GiB each.

MSZIP compression runs on every processor of your computer; `--jobs 2` uses at most two. On a
200 MB program with 16 processors, `mszip:6` takes about 5 seconds and makes 105 MB. LZX finds
repeats up to 2 MiB back (MSZIP: 32 KiB) and codes them more tightly: typically 5% smaller for
programs and 20% for text, but it works on one processor, at about 4 MB a second - try
`--compress lzx` for a release build and keep MSZIP for everyday ones.

## Several architectures: `--arch`

`arch = "x64"` is the source's own architecture. `--arch x86` builds the same source for another
one:

```text
C:\work\hello> rubrapack build hello.toml -o out-x64\hello-x64.msi --arch x64
C:\work\hello> rubrapack build hello.toml -o out-x86\hello-x86.msi --arch x86
C:\work\hello> rubrapack build hello.toml -o out-arm64\hello-arm64.msi --arch arm64
```

- `$(ARCH)` is a built-in variable: `x64`, `x86` or `arm64`, the architecture being built - here it
  picks `dist/x86/hello.exe` for the x86 package. rubrapack checks each program against it.
- `ProgramFiles` follows: `C:\Program Files` for x64 and Arm64, `C:\Program Files (x86)` for x86.
- **Each architecture needs its own upgrade code** - `upgrade-code-x86`, `upgrade-code-arm64` (and
  `upgrade-code-x64` if the source's own `arch` were another). Windows' upgrade logic cannot tell
  architectures apart: with one shared code, installing the x86 package would remove the x64 one as
  "an older version". Without them, `--arch` is refused.

Each output goes to its own folder here because external cabinets are named after the package and
must not collide.

## The same bytes every time: `--reproducible`

A normal build gives every package a new random *package code* (a GUID in the file's summary),
so two builds of the same source differ. `--reproducible` derives it from the content instead:
the same source and files give the same `.msi`, byte for byte, on any computer, with any number of
processors:

```text
C:\work\hello> rubrapack build hello.toml -o r1.msi --reproducible --jobs 1
C:\work\hello> rubrapack build hello.toml -o r2\r1.msi --reproducible
C:\work\hello> fc /b r1.msi r2\r1.msi
FC: no differences encountered
```

Anyone can then check that a published package was built from the published source. The package
code is shown by `rubrapack inspect hello.msi --summary` (property `9`).

## Names from a Mac: `--nfc`

Unicode can write a character like `한` or `é` in two ways - one code point, or letters plus
combining marks - and macOS file systems store names in the second form. `--nfc` converts file and
folder names to the first form (NFC), which Windows uses; `lint` warns (`RP2105`) about names that
are not NFC. Part III explains [Unicode normalization](../basics/03-text.md#unicode-normalization).

## Variables from the command line: `-D`

`-D NAME=VALUE` sets any `[define]` variable, as often as needed:
`-D VERSION=2.0.1 -D EDITION=Pro`. `$$(` writes a literal `$(`.

## Somebody else's part: `[merge.ID]`

Some libraries come with a merge module (`.msm`) instead of loose files. Put it beside the source
and name it; its files go under the folder you give:

```toml
[merge.Runtime]
source = "runtime.msm"
dir = "INSTALLDIR"
```

The module's cabinet becomes a second cabinet in the package, and `inspect hello.msi Media` shows
it. [Merge modules](../rpk.md#merge-modules-mergeid) in the reference lists what cannot be merged.

## What happened inside

```text
C:\work\hello> rubrapack inspect out-x64\hello-x64.msi Media
1	2		hello-x64.cab
C:\work\hello> rubrapack inspect out-x86\hello-x86.msi --summary
...
7	Intel;1033
```

The `Media` table lists the cabinets: disk 1 holds files 1 to 2, in `hello-x64.cab` (an embedded one
is written `#cab1.cab`, the `#` meaning "inside this file"). The summary's *template* (property 7)
names the platform (`x64`, `Arm64`, or `Intel` for x86) and the language. Part IV shows the cabinet
format - CFHEADER, CFFOLDER, CFDATA - and the MSZIP compression byte by byte.
