# Checking and looking inside

Goal: catch mistakes before users do - check a source or a finished package, look at every table,
file and stream in it, unpack it without installing, and check its signature. These commands also
work on packages that other tools made.

This chapter uses the `hello.toml` of chapter 17 and the `hello.msi` built from it.

## Checking a source: `lint`

`lint` runs every check `build` runs, and writes nothing:

```text
C:\work\hello> rubrapack lint hello.toml
C:\work\hello> rubrapack lint hello.toml --target msix
C:\work\hello> rubrapack lint hello.toml --arch x86 -D VERSION=2.2.0
```

No output and exit code 0 means no problem. `--target msix` checks the source for an MSIX
instead of an MSI; `--arch` and `-D` check it as `build` with the same options would build it.

Warnings do not stop a build: something is probably not what you meant, but the package works.
`--strict` makes them fail too, which is what an automatic build wants. Suppose the product's name
is Korean and there is no `summary-name` (chapter 5):

```text
C:\work\hello> rubrapack lint ko.toml
ko.toml:1:1: warning[RP1203]: name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'
C:\work\hello> echo %errorlevel%
0
C:\work\hello> rubrapack lint ko.toml --strict
ko.toml:1:1: warning[RP1203]: name is not ASCII and summary-name is missing; the summary Subject will read 'rubrapack package'
C:\work\hello> echo %errorlevel%
5
```

`%errorlevel%` is how `cmd` shows the exit code of the last command; in PowerShell it is
`$LASTEXITCODE`. The exit codes:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | an error in the source |
| 2 | a command line rubrapack does not understand |
| 3 | a file could not be read or written |
| 4 | signing or a signature |
| 5 | `lint` found a problem (or a warning, with `--strict`) |
| 6 | the network (a timestamp server) |

## Checking a package: `lint <file>`

```text
C:\work\hello> rubrapack lint hello.msi
hello.msi: 0 errors, 0 warnings
C:\work\hello> rubrapack lint hello.msix
hello.msix: 7 files, 0 errors, 0 warnings
C:\work\hello> rubrapack lint hello.msixbundle
hello.msixbundle: 4 files, 0 errors, 0 warnings
```

For an MSI it checks the tables with the rules Windows Installer applies: what would stop an
installation is an error, anything else a warning. A package made by another tool may well have
warnings. Here a product name was typed in a program that stores accented letters in two pieces
(see [Unicode normalization](../basics/03-text.md#unicode-normalization) in Part III):

```text
C:\work\hello> rubrapack lint cafe.msi
cafe.msi: warning[RP2105]: lint: Feature row 'Main': column Title is not in NFC (1 row); build --nfc puts folder, file and shortcut names in NFC
cafe.msi: warning[RP2105]: lint: Property row 'ProductName': column Value is not in NFC (1 row); build --nfc puts folder, file and shortcut names in NFC
cafe.msi: 0 errors, 2 warnings
```

For an MSIX it checks what Windows checks before installing: the ZIP archive, the block map, the
hash of every block, and that the manifest names files that are there. `lint new.msi --previous
old.msi` checks an upgrade (chapter 3).

## Looking at the tables: `inspect`

An MSI is a small database. `inspect` with a table's name prints that table:

```text
C:\work\hello> rubrapack inspect hello.msi File
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
F_6d1508f52f6615459567	C_a10be66379e69940a2e0	guide.txt	6			512	1
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	2
```

This is Windows Installer's own text format for tables (*IDT*), which its tools read too. The
first three lines are the header:

1. the column names;
2. each column's type: `s72` a string of up to 72 characters, `l255` a *localizable* string, `i4`
   a 4-byte integer, `i2` a 2-byte one; a capital letter (`S72`, `I2`) means the column may be empty;
3. the table's name, then its key columns - the ones that make a row unique.

Every row after it is one file. `hello.exe` has a version and a language (1033 is English (United
States)) because it is a program with a version resource; `guide.txt` has none. Attributes `512`
is the bit "vital" (chapter 4: the installation fails if the file cannot be written). Part IV
explains every type and how the rows are stored.

Without a table name, `inspect` lists every table with its row and column counts, and the
*summary information*, the few facts Windows shows in a file's properties:

```text
C:\work\hello> rubrapack inspect hello.msi
code page: 65001
strings: 156
tables: 19
  AdminExecuteSequence  rows=8 columns=3
  ...
  File  rows=2 columns=8
  ...
summary:
  2 = Installation Database
  3 = Hello
  4 = Example Software
  5 = Installer
  7 = x64;1033
  9 = {7FC806A2-F359-4941-A159-26A77B78F839}
  14 = 200
  15 = 2
  18 = rubrapack 0.7.1
```

The numbers of the summary are property IDs: 2 title, 3 subject, 4 author, 5 keywords, 7 the
platform and languages, 9 the *package code* (a new GUID for every build), 14 the Windows Installer
version needed (200 = 2.0), 15 word flags (2 = compressed), 18 the program that made it.
`--summary` prints only them.

## Files and streams

```text
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\guide.txt	6	F_6d1508f52f6615459567	C_a10be66379e69940a2e0			2b6739281f9372442df30e784b7dbdc7
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello	C_185f8db32271fe25f561	1.2.3.4	1033
```

One line per file: where it installs, size, file ID, component, version, language, and the MD5 hash
Windows uses to decide whether a file without a version needs replacing (a program is compared by
its version instead, so it has none).

```text
C:\work\hello> rubrapack inspect hello.msi --streams
table  40	File
stream 6821	cab1.cab
table  14	Media
table  728	_Columns
...
stream 332	!SummaryInformation
```

A *stream* is a file inside the package (Part IV: the package is a file system in a file). Each
table is one stream; `cab1.cab` holds the compressed files; `!SummaryInformation` the summary.

A cabinet by itself can be inspected too - the external one of chapter 15:

```text
C:\work\hello> rubrapack inspect out-x64\hello-x64.cab
6	F_6d1508f52f6615459567
17920	Hello
```

## Unpacking without installing: `extract`

```text
C:\work\hello> rubrapack extract hello.msi -d unpacked
unpacked: 2 files, 17926 bytes
```

The files come out as they would install, below the names of the standard folders:

```text
unpacked\ProgramFiles64Folder\Hello\guide.txt
unpacked\ProgramFiles64Folder\Hello\hello.exe
```

`extract` works on `.msi`, `.cab`, `.msix` and `.msixbundle`, and is safe on packages from
anywhere: the target folder must be new or empty; every file is checked against its size and hash;
names that would write outside the folder (`..`, a drive, `CON`) are refused; and nothing is
written until the whole package has passed. Limits guard against a package made to fill your
disk:

```text
C:\work\hello> rubrapack extract hello.msi -d small --limit-entries 2
rubrapack: error[RP0008]: 'hello.msi': 4 entries, more than the limit 2 (--limit-entries); nothing was extracted
C:\work\hello> rubrapack extract hello.msi -d small --limit-bytes 1000
rubrapack: error[RP0008]: 'hello.msi': more than 1000 bytes to write (--limit-bytes); nothing was extracted
```

The defaults are 100,000 entries and 16 GiB. (The four entries are the two files and the two
folders, `ProgramFiles64Folder` and `Hello`.)

## Signatures: `verify`

Chapter 16 showed `verify`. It is the check to run on what you are about to publish:

```text
C:\work\hello> rubrapack verify hello.msi --system-roots
```

## MSIX packages

```text
C:\work\hello> rubrapack inspect hello.msix
C:\work\hello> rubrapack inspect hello.msix --files
C:\work\hello> rubrapack inspect hello.msix --manifest
```

The first shows the identity, the executable and the files, after checking every block's hash.
Chapter 17 showed the other two.

## Faster test builds: `--compress none`

Compression takes most of a build's time. While you try things out, leave it out:

```text
C:\work\hello> rubrapack build hello.toml -o fast.msi --compress none
```

The package is bigger (here 45056 bytes instead of 32768) and works the same.

## GUIDs: `guid`

```text
C:\work\hello> rubrapack guid
{98AE4FED-BF0B-4C77-B712-E0649EB47178}
C:\work\hello> rubrapack guid --from hello
{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}
```

The first is random - new every time - for an upgrade code. The second is the GUID rubrapack
*derives* from a text; it is the same on every computer, every time. rubrapack derives component
GUIDs this way, which is why they stay the same from version to version. Part III [shows the calculation](../basics/04-guids-and-hashes.md#guids-from-hashes-guid---from).

## Help on the command line

```text
C:\work\hello> rubrapack help
rubrapack 0.7.1 - build Windows Installer (.msi) and MSIX (.msix) packages

usage: rubrapack <command> [arguments]

commands:
  build    build a package from a source file
  sign     sign a PE file, an MSI package, an MSIX package or bundle
  ...
C:\work\hello> rubrapack help sign
usage: rubrapack sign <file.exe|.dll|.msi|.msix|.msixbundle> (--key <key.pfx|.pem> ...
C:\work\hello> rubrapack version
rubrapack 0.7.1 (proven_c_lib-v0.1.1)
```

`rubrapack --help` is the same as `rubrapack help`. `help <command>` prints that command's options
- the short form of Part II, which has them all with their meaning.
