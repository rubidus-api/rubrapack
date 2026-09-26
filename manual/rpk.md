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
| `[package]` | **name**, **manufacturer**, **version** (`a.b.c` or `a.b.c.d`), **arch**, **upgrade-code**, upgrade-code-x64 / -arm64 / -x86, product-code, summary-name (ASCII), language, scope (`machine`), ui (`none`), reboot (`suppress`/`allow`), downgrade-message, compress (`none`, `mszip`, `mszip:0`..`mszip:9`; default `mszip:6`), cab (`embed`), refuse-upgrade-below, refuse-upgrade-message |
| `[define]` | variables: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level (1-32767), hidden, parent |
| `[dir.ID]` | **path** = `Base/relative/path`, feature |
| `[file.ID]` | **dir**, **source**, name, vital (default true), any-arch, feature, component-guid |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, help (URL), about (URL) - how the product shows in Installed apps |
| `[property.ID]` | **value**, secure, hidden - an upper-case public property |
| `[action.ID]` | **run** (`file:ID` of an `.exe` in this package), **do**, **undo**, check |
| `[registry.ID]` | **root** (`HKLM`, `HKCR`), **key**, name, value, type, remove, keep, view, with, feature |
| `[shortcut.ID]` | **dir** (a dir ID, or `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target** (`file:ID`), args, description, working-dir (a dir ID) |

`Base` in a dir path is another dir ID or one of: `ProgramFiles` (64-bit for x64/arm64, 32-bit
for x86), `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`, `CommonAppData`,
`StartMenu`, `Programs`, `Desktop`, `Windows`, `System`, `Fonts`, `Temp`.

IDs are `[A-Za-z_][A-Za-z0-9_]*` (at most 72 characters, 38 for features) and must differ across
dirs, files and features. Registry, shortcuts, services, dialogs and the rest are planned; their
tables are refused with "not supported yet" until they are implemented.

### Registering with an installed program: `[action.ID]`

```toml
[action.Tip]
run = "file:MainExe"      # an .exe this package installs
do = "--register"         # run after the files are installed, and again on repair
undo = "--unregister"     # run on removal, before the files are removed
```

rubrapack turns the pair into deferred, elevated actions with their rollback twins, so removal and
upgrade are all-or-nothing: if anything fails later (or `do`/`undo` itself exits non-zero), the
files come back and the other command restores the registration. Both commands must be safe to run
twice and must finish without asking anything - they run without a window, and nothing waits for a
user. `check` names an optional command that exits 0 when the registration is in place.

### Registry values: `[registry.ID]`

```toml
[registry.InstallDir]
root = "HKLM"
key = 'SOFTWARE\Example'          # literal strings keep the backslashes
name = "InstallDir"               # omit it for the key's default value
value = "[INSTALLDIR]"            # an MSI formatted string: [PROPERTY], [#FileID], [\[] for "["
```

`type` is `string` (default), `expand`, `dword` (an integer, 0 to 0xFFFFFFFF), `binary` (hex
digits) or `multi` (an array of strings). Each value is its own component, removed at uninstall
(`keep = true` leaves it); `with = "file:ID"` puts it in that file's component instead. In a 64-bit
package values go to the 64-bit registry view; `view = "32"` writes to the 32-bit view.
`remove = true` (without `value`) deletes the named value - or the whole key when `name` is
omitted - during installation. `HKCU` waits for per-user packages; `qword` for a later helper.

### Shortcuts: `[shortcut.ID]`

```toml
[dir.Menu]
path = "Programs/Example"         # Start menu > Example

[shortcut.Settings]
dir = "Menu"
name = "Example settings"         # ".lnk" is added
target = "file:MainExe"
args = "--settings \"[INSTALLDIR]\""
```

A shortcut belongs to its target file (and its feature); folders created for it are removed at
uninstall. In a per-machine package `Programs` and `Desktop` are the all-users Start menu and the
Public Desktop.

Only these fields are MSI formatted strings: registry `value` (and multi items), shortcut `args`,
and later environment and INI values, service arguments, and conditions. Everywhere else rubrapack
writes the text exactly as given.

### Installed apps entry and properties

`[arp]` sets how the product appears in Settings > Installed apps (`no-modify`, `no-repair`,
`help`, `about`). `[property.NAME]` adds a public property; `secure = true` lets it reach the
elevated part of the installation, `hidden = true` keeps its value out of logs. Names the installer
or rubrapack set themselves (`ARP*`, `MSI*`, `RP_*`, `ALLUSERS`, `REBOOT`, ...) are refused.

### Installing without a window

Every package rubrapack builds installs, repairs, upgrades and uninstalls from the command line with
no user interface:

```text
msiexec /i example.msi /qn /l*v install.log
msiexec /x {ProductCode} /qn
```

Exit code 0 is success, 3010 success with a restart needed (rubrapack never restarts the machine
itself), anything else a failure after which the machine is as before.

### Architectures and upgrade families

`arch` is the source's own architecture and `upgrade-code` its upgrade family. To build the same
source for another architecture (`--arch`), give that architecture its own family with
`upgrade-code-x86`, `upgrade-code-arm64` or `upgrade-code-x64`; without it the build is refused.
Windows Installer's upgrade detection cannot tell architectures apart, so sharing one code would
make installing one architecture remove the other.

### Versions that must be removed first

`refuse-upgrade-below = "1.0.0"` refuses to upgrade an installed version below 1.0.0 and tells
the user to remove it first; the message (`refuse-upgrade-message`, or a default one) always ends
with the command that does it: `msiexec /x {ProductCode} /qn MSIRESTARTMANAGERCONTROL=Disable`.
Use it when older versions were built by another tool without `MSIRESTARTMANAGERCONTROL=Disable`:
removing such a version inside an upgrade would try to close every program that has its files
loaded (see `formats/msi-package.md`, "Files in use").

### Wildcards: `[files.ID]`

`glob` is a source path with `*` (any characters within one folder), `?` (one character) and
`**` (any number of folders). Matches are sorted by name, so the result never depends on the
file system. Folders below the first wildcard are recreated under `dir`:
`glob = "dist/layouts/**/*.jmt"` installs `dist/layouts/de/x.jmt` as `<dir>/de/x.jmt`. No match,
a symbolic link, or the output file itself among the matches is an error.

### Empty folders: `[folder.ID]`

Creates the folder `name` inside `dir` even when no file goes there. With `keep = true` the folder
stays after uninstall (for data the program writes).

### Features

Without any `[feature.*]` table everything goes into one hidden feature. As soon as one feature is
declared, every file needs one: its own `feature` key, or the `feature` of its dir. A feature with
a `level` above 1 is not installed by default.

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
names in one folder may not differ only by letter case. Windows Installer stores a name together
with its 8.3 short name in 255 UTF-16 units, so a long name may use only what the short name
leaves: 242 units for `name.ext` with a three-letter extension, 246 for a name without one
(NTFS would accept 255). A longer name is refused with RP1514.

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
- Before writing, `build` checks the finished tables (`RP20xx` diagnostics, exit code 5); these
  checks guard rubrapack itself, so a source that passes the `RP1xxx` checks should never meet them.
- Exit codes: 0 success, 1 error in the source, 2 usage, 3 input/output, 4 signing, 5 lint,
  6 network.
