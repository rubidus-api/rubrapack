# `.rpk` sources and the `rubrapack` command

An `.rpk` file describes a package. It is a strict subset of [TOML 1.0](https://toml.io/en/v1.0.0):
every `.rpk` file is valid TOML, but rubrapack refuses TOML features outside the subset instead of
ignoring them.

## Example

```toml
[define]
VERSION = "1.4.0"

[package]
name = "Example App"
manufacturer = "Example"
version = "$(VERSION)"
arch = "x64"                                        # x64, arm64 or x86 - no default
upgrade-code = "{0B9A6C1E-3D2F-4A5B-8C7D-6E5F4A3B2C1D}" # generate your own once, keep it forever
language = "en-US"                                  # or "ko-KR"

[dir.INSTALLDIR]
path = "ProgramFiles/Example App"

[dir.Docs]
path = "INSTALLDIR/docs"

[file.MainExe]
dir = "INSTALLDIR"
source = "dist/app.txt"

[file.Guide]
dir = "Docs"
source = "dist/guide.txt"
name = "User guide.txt"
```

```sh
rubrapack build example.rpk -o example.msi -D VERSION=1.4.1
rubrapack inspect example.msi File
```

## The TOML subset

Accepted: tables `[kind]` and `[kind.ID]`, bare keys (`A-Z a-z 0-9 _ -`), basic strings `"..."`
with TOML escapes, literal strings `'...'` (no escapes - use them for backslashes and quotes:
`'SOFTWARE\Example'`), decimal and `0x` integers, `true`/`false`, one-type arrays, `#` comments.
UTF-8 with or without a BOM, or UTF-16LE with a BOM; LF or CRLF.

Refused with an error: multi-line strings, inline tables, arrays of tables, dotted and quoted
keys, table names with more than two parts, floats, dates, `_` in numbers, octal/binary numbers,
empty or mixed arrays, keys before the first table. Table and key order never matters.

## Tables

| Table | Keys (required in bold) |
|---|---|
| `[package]` | **name**, **manufacturer**, **version** (`a.b.c` or `a.b.c.d`), **arch**, **upgrade-code**, product-code, summary-name (ASCII), language, scope (`machine`), ui (`none`), reboot (`suppress`/`allow`), downgrade-message, compress (`none`, `mszip`, `mszip:0`..`mszip:9`; default `mszip:6`), cab (`embed`) |
| `[define]` | variables: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level (1-32767), hidden, parent |
| `[dir.ID]` | **path** = `Base/relative/path`, feature |
| `[file.ID]` | **dir**, **source**, name, vital (default true), any-arch, feature, component-guid |

`Base` in a dir path is another dir ID or one of: `ProgramFiles` (64-bit for x64/arm64, 32-bit
for x86), `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`, `CommonAppData`,
`StartMenu`, `Programs`, `Desktop`, `Windows`, `System`, `Fonts`, `Temp`.

IDs are `[A-Za-z_][A-Za-z0-9_]*` (at most 72 characters, 38 for features) and must differ across
dirs, files and features. Registry, shortcuts, services, dialogs and the rest are planned; their
tables are refused with "not supported yet" until they are implemented.

### Features

Without any `[feature.*]` table everything goes into one hidden feature. As soon as one feature is
declared, every file needs one: its own `feature` key, or the `feature` of its dir.

### Variables

`$(NAME)` inside a string value is replaced once, from `-D NAME=value` on the command line first,
then `[define]`. The result is not read again (a value containing `$(X)` stays literal).
`$$` is a literal `$`. An undefined name is an error.

### Program files

Files that are Portable Executables (`.exe`, `.dll`, ...) are checked: their machine type must
match `arch` (an x86 helper in an x64 package needs `any-arch = true`), and their version resource
becomes the file's version in the package, which is how Windows Installer decides whether to
replace an installed file.

### Paths

Source paths are relative to the `.rpk` file and use `/`. Absolute paths, `\`, symbolic links and
missing files are errors. Target names may use any Unicode text except what Windows forbids
(`< > : " / \ | ? *`, control characters, trailing dot or space, device names such as `CON`), and two
names in one folder may not differ only by letter case.

## Command line

```text
rubrapack build <src.rpk> -o <out.msi> [-D NAME=VALUE]... [--arch x64|arm64|x86]
                [--compress none|mszip|mszip:N] [--reproducible]
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack version | help [command]
```

- Command-line options override the source.
- `--reproducible` derives the package code from the content: the same source gives the same
  bytes, on Linux and on Windows. Without it the package code is random, as Windows Installer
  expects for different package files.
- `inspect <file.msi> <table>` prints the table in Windows Installer's IDT format;
  `--files` lists every file with its installed path.
- Diagnostics look like `example.rpk:12:3: error[RP1201]: unknown key 'nmae' in [package] (did you
  mean 'name'?)`.
- Exit codes: 0 success, 1 error in the source, 2 usage, 3 input/output, 4 signing, 5 lint,
  6 network.
