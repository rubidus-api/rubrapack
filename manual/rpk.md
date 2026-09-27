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
| `[package]` | **name**, **manufacturer**, **version** (`a.b.c` or `a.b.c.d`), **arch**, **upgrade-code**, upgrade-code-x64 / -arm64 / -x86, product-code, summary-name (ASCII), language, scope (`machine`, `user`, `dual`), ui (`none`, `basic`, `minimal`, `installdir`, `features`), license (`.txt`, `.md`, `.rtf`), reboot (`suppress`/`allow`), downgrade-message, compress (`none`, `mszip`, `mszip:0`..`mszip:9`; default `mszip:6`), cab (`embed` or `external`), cab-max-size (MiB), refuse-upgrade-below, refuse-upgrade-message |
| `[define]` | variables: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level (1-32767), hidden, parent |
| `[dir.ID]` | **path** = `Base/relative/path`, feature |
| `[file.ID]` | **dir**, **source**, name, vital (default true), any-arch, feature, component-guid |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, help (URL), about (URL) - how the product shows in Installed apps |
| `[property.ID]` | **value**, secure, hidden - an upper-case public property |
| `[action.ID]` | **run** (`file:ID` of an `.exe` in this package), **do**, **undo**, check |
| `[registry.ID]` | **root** (`HKLM`, `HKCU`, `HKCR`, `HKMU`), **key**, name, value, type, remove, keep, view, with, feature |
| `[remove.ID]` | **dir**, name (`*` and `?`; omitted = the folder itself), **on** (`install`, `uninstall`, `both`), feature |
| `[ini.ID]` | **dir**, **file**, **section**, **key**, value, mode (`set`, `add`, `remove`), feature |
| `[require.ID]` | **condition**, **message** |
| `[search.ID]` | **property**, **kind** (`registry`: root, key, name, view; `file`: path, file, min-version; `dir`: path; `component`: component-guid) |
| `[service.ID]` | **file** (`file:ID` of an `.exe`), **name**, display-name, description, start (`auto`, `demand`, `disabled`), account (`LocalSystem`, `LocalService`, `NetworkService`), args, start-on-install |
| `[font.ID]` | **file** (`file:ID` of a file in a dir with `path = "Fonts"`), title |
| `[permission.ID]` | **target** (`dir:ID`, `file:ID`, `registry:ID`), **sddl** |
| `[env.ID]` | **name**, **value**, mode (`set`, `append`, `prepend`), keep, feature |
| `[copy.ID]` | **source** (`file:ID`), **dir**, name (default: the source's name) |
| `[ui]` | install-dir (a dir ID; default `INSTALLDIR`), banner (`.bmp`) |
| `[ui-text.ID]` | **text** - replaces one built-in dialog text |
| `[dialog.ID]` | **after** (a built-in page or another `[dialog.*]`), title, description |
| `[dialog-control.ID]` | **dialog**, **type** (`text`, `checkbox`, `edit`, `radio`, `combo`), **x**, **y**, **width**, **height**, text, property, values, labels |
| `[shortcut.ID]` | **dir** (a dir ID, or `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target** (`file:ID`), args, description, working-dir (a dir ID) |
| `[msix]` | **identity-name**, **publisher**, publisher-display-name, min-version - see [MSIX packages](#msix-packages) |
| `[msix-app.ID]` | **executable** (a `[file.*]` ID), display-name, description, logo-150, logo-44, store-logo |

`Base` in a dir path is another dir ID or one of: `ProgramFiles` (64-bit for x64/arm64, 32-bit
for x86), `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`, `CommonAppData`,
`StartMenu`, `Programs`, `Desktop`, `Startup`, `Windows`, `System`, `Fonts`, `Temp`. A path may
also be a known folder alone (`path = "Fonts"`) for files that go into that folder itself.

IDs are `[A-Za-z_][A-Za-z0-9_]*` (at most 72 characters, 38 for features) and must differ across
dirs, files and features. Tables that are planned but not implemented yet (`[assoc.*]`,
`[protocol.*]`, `[msix-extension.*]`) are refused with "not supported yet".

### MSIX packages

The same source builds an MSIX package when the output ends in `.msix`: one desktop application
that runs with full trust, for one architecture. Two more tables say what only MSIX needs:

```toml
[msix]
identity-name = "Example.App"               # 3-50 characters: A-Z a-z 0-9 . -
publisher = "CN=Example, O=Example, C=KR"   # the subject of the certificate that will sign it
publisher-display-name = "Example"          # default: [package] manufacturer
min-version = "10.0.17763.0"                # the oldest Windows it installs on (this is the default)

[msix-app.Main]
executable = "MainExe"                      # the [file.*] that starts the app
display-name = "Example App"                # default: [package] name
description = "An example"                  # default: the display name
logo-150 = "assets/Square150x150.png"       # PNG, 150x150
logo-44 = "assets/Square44x44.png"          # PNG, 44x44
store-logo = "assets/StoreLogo.png"         # PNG, 50x50
```

- The version is `[package] version` with four parts (`1.2.3` becomes `1.2.3.0`), the
  architecture `[package] arch`, the language `[package] language`.
- The package holds the folder of the executable - the dir anchored in a known location, such as
  `ProgramFiles/Example App` - and everything below it. A file anywhere else is an error
  (`RP1609`); files in other locations need the package's virtual file system, which comes later.
- Give the three logos or none: without them the package gets plain one-colour logos. A logo
  must have the exact size (`RP1608`).
- What an MSIX cannot do is an error, not something left out quietly (`RP1605`): custom actions,
  services, environment variables, INI files, permissions, launch conditions and searches, files
  removed or copied at install, empty folders; registry values, shortcuts and fonts come later.
  Add `msi-only = true` to such a table (or to a `[file.*]`/`[files.*]`) and the MSI keeps it while
  the MSIX is built without it. Features, properties, dialogs and `[arp]` concern the Windows
  Installer only and are not used for an MSIX.
- `--unsigned-test` adds the attribute Windows needs to install an unsigned package for testing
  (`Add-AppxPackage -AllowUnsigned`, as administrator when it contains a program); such a package
  is not for distribution and its identity differs from the signed one. Signing MSIX packages
  comes later.
- Files are compressed (`--msix-compress store` turns it off); pictures, archives and other
  already compressed files are stored. An MSIX holds no time: the same source gives the same
  bytes on Linux and Windows.

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

`type` is `string` (default), `expand`, `dword` (an integer, 0 to 0xFFFFFFFF), `qword` (an
integer, or `"0x"` and up to 16 hex digits), `binary` (hex digits) or `multi` (an array of strings).
Windows Installer cannot write REG_QWORD itself; rubrapack adds its small helper DLL to the package
for it, which also restores the previous value if the installation fails. Each value is its own component, removed at uninstall
(`keep = true` leaves it); `with = "file:ID"` puts it in that file's component instead. In a 64-bit
package values go to the 64-bit registry view; `view = "32"` writes to the 32-bit view.
`remove = true` (without `value`) deletes the named value - or the whole key when `name` is
omitted - during installation.

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

### Removing and copying: `[remove.ID]`, `[copy.ID]`

`[remove.ID]` deletes files matching `name` in `dir` - for example `*.log` an older version left, at
`on = "install"`, or files the program writes at run time, at `on = "uninstall"`. Without `name` it
removes the folder itself when it is empty. A failed installation puts removed files back.
`[copy.ID]` installs a second copy of a file of this package into another folder; it goes and comes
with its source. Folders that existed before the installation are never removed.

### Environment variables: `[env.ID]`

A system (machine-wide) variable. `mode = "set"` (default) replaces it, `append`/`prepend` add
`;value` to the end or `value;` to the front of what is there. Uninstall undoes exactly that: a set
variable is deleted, an appended part is taken out and the rest kept. `keep = true` leaves it.

### INI files: `[ini.ID]`

`mode = "set"` (default) writes `key=value` in `[section]` of the file, `add` appends the value to a
comma-separated list (`a` becomes `a,b`), `remove` deletes the key during installation. Uninstall
takes out what `set` and `add` wrote. `remove` runs before files are installed, so it is for INI
files an older version left behind.

### Searching and requiring: `[search.ID]`, `[require.ID]`

A search runs before anything else and puts what it found into a public property (empty when
nothing is found): a registry value's data, the full path of a file (`path` = a known folder and a
relative path, like `System` or `ProgramFiles/Example`; `min-version` for program files), a folder,
or the key file of another product's component. A requirement stops a first installation with its
message when its `condition` is false (repair and removal are never blocked); conditions use Windows Installer's syntax
(`VersionNT >= 603`, `FOUND_TOOL`, `NOT OLDSETTING`) and may test search results.

```toml
[search.Tool]
property = "FOUND_TOOL"
kind = "file"
path = "System"
file = "tool.exe"

[require.Tool]
condition = "FOUND_TOOL"
message = "[ProductName] needs tool.exe."
```

### Services, fonts and permissions

`[service.ID]` installs a service run by an `.exe` of the package: it is stopped before its files
change and at uninstall, deleted at uninstall, and started after installation with
`start-on-install = true`. `[font.ID]` registers a font file that the package installs into the
Fonts folder; without `title` Windows reads the name from the TrueType/OpenType file.
`[permission.ID]` sets an SDDL security descriptor on a folder the package creates, one of its
files, or a registry value it writes (this raises the package to Windows Installer 5.0).

### Per-user and dual packages: `scope`

`scope = "machine"` (default) installs for everyone and needs administrator rights. `scope = "user"`
installs for the current user without elevation: `ProgramFiles` becomes
`%LOCALAPPDATA%\Programs`, `Programs` and `Desktop` are the user's, registry values go to `HKCU`
(or `HKMU`), environment variables are the user's, and do/undo actions run as the user; asking for
a per-machine installation is refused. `scope = "dual"` installs per user by default and per
machine from an elevated prompt with `msiexec /i x.msi ALLUSERS=1 MSIINSTALLPERUSER=""`; its
registry values use `HKMU`, which is `HKLM` or `HKCU` as installed. Services, fonts, permissions
and the machine folders (`Windows`, `System`, `Fonts`, `CommonAppData`) need `scope = "machine"`.

### Cabinets

Files are compressed into one cabinet embedded in the package. `cab-max-size = N` starts a new
cabinet after N MiB of files; `cab = "external"` writes the cabinets next to the package as
`<name>.cab` (or `<name>-1.cab`, `<name>-2.cab`, ...), which must travel with it. rubrapack never
overwrites an existing cabinet and writes the package last. Every package also carries the
administrative (`msiexec /a`, an uncompressed network image) and advertisement (`msiexec /jm`)
sequences.

Only these fields are MSI formatted strings: registry `value` (and multi items), shortcut `args`,
environment `value`, INI `value`, requirement `message`, service `args`. Everywhere else rubrapack
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

### Dialogs: `ui`

`ui` in `[package]` picks one of the built-in dialog sets. Without it (`none`) the package shows
only Windows Installer's own progress bar.

| `ui` | Installing | Already installed |
|---|---|---|
| `basic` | progress, then finished (or error) | progress, finished |
| `minimal` | welcome, the license if there is one, progress, finished | repair or remove |
| `installdir` | welcome, license, install folder (with a folder browser), ready, progress, finished | repair or remove |
| `features` | as `installdir`, plus a feature tree and the disk space it needs | repair or remove |

Every set also has the cancel question, the error dialog, the files-in-use list and the
out-of-disk-space warning. The dialogs only collect values that all have defaults, so `/qn` still
installs without a window.

```toml
[package]
ui = "installdir"
license = "LICENSE.txt"           # the Install/Next button stays disabled until it is accepted
language = "ko-KR"                # the built-in texts are Korean for ko-KR, English otherwise

[ui]
install-dir = "APPDIR"            # the dir the user may change (default INSTALLDIR)
banner = "banner.bmp"             # the strip at the top; about 493 x 58 pixels

[ui-text.WelcomeText]
text = "This will install [ProductName]. Close other programs first."
```

A `.txt` or `.md` license is shown as plain text (Korean, emoji and any other characters are kept);
an `.rtf` license is used as it is. Without `banner` the strip is plain white. `[ui-text.ID]`
replaces one text; the texts are MSI formatted strings, so `[ProductName]` is replaced and a literal
`[` is written `[\[]`. The IDs are: `Back`, `Next`, `Cancel`, `Install`, `Finish`, `OK`, `Yes`, `No`,
`Retry`, `Ignore`, `Abort`, `Exit`, `Browse`, `WelcomeTitle`, `WelcomeText`, `LicenseTitle`,
`LicenseText`, `LicenseAccept`, `DirTitle`, `DirText`, `DirLabel`, `BrowseTitle`, `BrowseText`,
`BrowseLookIn`, `BrowseFolder`, `BrowseUp`, `BrowseNew`, `CustomizeTitle`, `CustomizeText`,
`Reset`, `DiskCost`, `DiskCostTitle`, `DiskCostText`, `ReadyTitle`, `ReadyText`, `ProgressTitle`,
`ProgressText`, `ProgressStatus`, `ExitTitle`, `ExitText`, `UserExitTitle`, `UserExitText`,
`FatalTitle`, `FatalText`, `CancelText`, `FilesInUseTitle`, `FilesInUseText`, `OutOfDiskTitle`,
`OutOfDiskText`, `MaintTitle`, `MaintText`, `Repair`, `RepairText`, `Remove`, `RemoveText`. In
button texts `&` marks the access key (`&Next` is Alt+N).

### Your own dialog pages: `[dialog.ID]`, `[dialog-control.ID]`

With `minimal`, `installdir` or `features` you can add pages to the built-in flow. A page gets the
same banner and Back / Next / Cancel buttons as the others; you place the controls in its body.

```toml
[dialog.Options]
title = "Options"                 # the banner heading; [ProductName] when omitted
description = "Choose how to install."
after = "RpInstallDirDlg"         # shown right after this page

[dialog-control.ModeLabel]
dialog = "Options"
type = "text"
x = 20
y = 55
width = 330
height = 12
text = "Installation &mode:"

[dialog-control.Mode]
dialog = "Options"
type = "radio"
x = 20
y = 70
width = 200
height = 42                       # at least 12 per value
property = "APP_MODE"
values = ["typical", "portable", "server"]
labels = ["&Typical", "&Portable", "&Server"]

[property.APP_MODE]
value = "typical"                 # the default, also for a silent installation
```

- `after` is a page of the chosen set - `RpWelcomeDlg`, `RpLicenseDlg` (with a license),
  `RpInstallDirDlg` (installdir, features), `RpCustomizeDlg` (features) - or another of your pages.
  Several pages after the same page follow in ID order. In `minimal` the last page's button is
  Install; in the other sets the ready page always comes last.
- Coordinates are dialog units on a 370 x 270 page; controls go in the body, between y = 45 and
  y = 234. The Tab key moves through the controls from top to bottom, then left to right.
- `text` shows a label (`&` marks an access key; on a text control it moves to the next control).
  `checkbox` sets its property to `1` when ticked and removes it when clear. `edit` lets the user
  type the property's value. `radio` and `combo` choose one of `values` (1 to 32 strings of at most
  64 bytes), shown as `labels` (default: the values themselves), in the order written.
- A `radio` or `combo` needs a `[property.*]` whose value is one of its values: the dialogs only
  collect values, and a silent installation (`/qn`) uses the defaults. Any property can also be
  set on the command line: `msiexec /i example.msi /qn APP_MODE=server`.
- The properties are public names of your own (upper case), one control each, and reach the
  elevated part of the installation, so `[registry.*]`, `[ini.*]`, `[env.*]`, conditions and the
  rest can use them as `[APP_MODE]`.
- Page IDs starting with `Rp` and the control IDs of the frame (`Banner`, `Title`, `Description`,
  `BannerLine`, `BottomLine`, `Back`, `Next`, `Cancel`) are reserved; a page holds up to 64
  controls.

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
rubrapack build <src.rpk> -o <out.msi|out.msix> [-D NAME=VALUE]... [--arch x64|arm64|x86]
                [--compress none|mszip|mszip:N] [--nfc] [--reproducible]
                [--key <key.pfx|.pem> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE]
                 [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                  [--proxy <URL>]] [--allow-unsigned-cabs]]
                [--unsigned-test] [--msix-compress deflate|store]                   (.msix)
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack inspect <file.msix> [--files | --manifest]
rubrapack inspect <file.cab>
rubrapack new [msi] <name>
rubrapack guid [--from <text>]
rubrapack lint <src.rpk> [-D NAME=VALUE]... [--arch x64|arm64|x86] [--nfc] [--strict]
rubrapack lint <file.msi|file.msix> [--strict]
rubrapack extract <file.msi|file.msix|file.cab> -d <new dir> [--limit-entries N] [--limit-bytes N]
rubrapack sign <file.exe|.dll|.msi> --key <key.pfx|.pem> [--cert <chain.pem>] [--pass-env VAR | --pass-file FILE]
               [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]
rubrapack verify <file.exe|.dll|.msi> [--trust <certificates>]... [--system-roots] [--tsa-trust <certificates>]...
rubrapack version | help [command]
```

- Command-line options override the source.
- `--reproducible` derives the package code from the content: the same source gives the same
  bytes, on Linux and on Windows, from any folder, with the options in any order. Without it the
  package code is random, as Windows Installer expects for different package files. An MSI holds
  no time at all (no creation or save dates in its summary; cabinet entries are dated
  1980-01-01), so `SOURCE_DATE_EPOCH` does not change it; nor does it hold the source's path or
  the name of the machine or user that built it. Every source file is read once, and those bytes
  are hashed, versioned and packed; a file that changes while the package is being built stops the
  build (`RP1515`).
- `--nfc` puts the names the package gives to folders, files and shortcuts in Unicode NFC
  (composed). A file from macOS often has a decomposed name - a Hangul syllable such as U+D55C as the three jamo U+1112 U+1161 U+11AB - and Windows
  installs names exactly as they are, so the same word can become two different files. The source
  files keep their names; texts and registry values stay as written. `lint` warns (`RP2105`) about
  any text that is not in NFC, and a name that becomes the same as another one is refused
  (`RP1511`). The normalization is rubrapack's own, from Unicode 17.0 data.
- `inspect <file.msi> <table>` prints the table in Windows Installer's IDT format. `--files`
  prints one tab-separated line per file: installed path, size, File key, component, version,
  language, MD5 (from `MsiFileHash`; empty when the package has none). `--streams` lists the
  streams and their sizes. `inspect <file.cab>` lists the files in a cabinet. `inspect <file.msix>`
  shows the identity, the executable and the files (after checking every block's hash); `--files`
  lists path, size and whether each file is compressed, `--manifest` prints `AppxManifest.xml`.
- `new <name>` writes `<name>.rpk`, a source that builds as soon as the program's files are in
  `dist/`, with a fresh `upgrade-code`. It never replaces an existing file.
- `guid` prints a random GUID (version 4). `guid --from <text>` prints the GUID rubrapack derives
  from a text, the same way on every machine: SHA-256 over the 32-bit little-endian length of
  `guid` and those 4 bytes, the 32-bit little-endian number 1, and the 32-bit little-endian length
  of the text and its UTF-8 bytes; the first 16 bytes of the hash, with the version nibble set to 8
  and the variant bits to `10` (RFC 9562 UUIDv8), written as `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`
  in upper case. For example `guid --from hello` gives `{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}`.
- Diagnostics look like `example.rpk:12:3: error[RP1201]: unknown key 'nmae' in [package] (did you
  mean 'name'?)`.
- Before writing, `build` checks the finished tables (`RP20xx` diagnostics, exit code 5); these
  checks guard rubrapack itself, so a source that passes the `RP1xxx` checks should never meet them.
- `lint <src.rpk>` runs every check `build` runs and writes nothing. `lint <file.msi>` checks a
  package made by any tool with the same table rules: what stops an installation is an error
  (exit code 5) - a value that does not fit its column, a missing referenced row, a duplicate key,
  a broken dialog tab order (`RP2101`), a dialog's first/default/cancel control that is not there
  (`RP2102`), a files-in-use dialog without a `ListBox` table (`RP2103`), an error dialog without
  `ErrorText`/`ErrorIcon` (`RP2104`) - and everything else is a warning. `--strict` makes warnings
  fail too. A package in another code page than 65001 gets a note (`RP2100`): its text cannot be
  checked as UTF-8. The last line on stdout counts errors and warnings. `lint <file.msix>` checks the package the way Windows reads
  it - the ZIP, the block map, every block's hash - and that the manifest has an identity and
  names files that are in the package (`RP2201`, `RP2202`).
- `extract` unpacks a package the way it installs: folders by their long names under the
  Directory tree (a standard folder such as `ProgramFiles64Folder` keeps its name), files from the
  embedded or external cabinets, or from the source folders next to an uncompressed package.
  Every file is checked against its size and `MsiFileHash`. The target must be new or empty, and
  nothing is written until the whole package has passed: names with `..`, `/`, `\`, `:`, a drive,
  a reserved device name (`CON`, `COM1`, ...), a trailing dot or space, or two paths that differ
  only in case are refused. The defaults allow 100,000 entries and 16 GiB. A `.cab` unpacks by the
  names inside it. An `.msix` unpacks its files (not the ZIP's own
  `[Content_Types].xml` and `AppxBlockMap.xml`) after every block has matched its hash.
- `sign` adds an Authenticode signature (SHA-256, RSA) to a PE file or an MSI package, in place or
  to `-o`; `build --key` signs the package as it is built (the same code), and a signed
  `--reproducible` build still gives the same bytes every time (the signature holds no time) - unless
  it is timestamped: a timestamp carries the server's time and serial number, so it differs on every
  run, and only the package inside the signature stays the same. An MSI
  whose cabinets lie outside it is refused: its signature would not cover them - use embedded
  cabinets, or `--allow-unsigned-cabs` to sign the `.msi` alone with a warning. The key
  file is a PKCS#12 file (`.pfx`/`.p12`: AES with PBKDF2-HMAC-SHA256, SHA-256 MAC - what current
  Windows and OpenSSL export) or a PKCS#8 PEM/DER key, plain or encrypted with PBES2; `--cert` adds
  certificates the key file does not hold. The certificate must be for code signing, must not be a
  CA and must be valid now. The password comes from an environment variable or a file, never from
  the command line. A file that is signed already is refused.
- `--timestamp <URL>` asks an RFC 3161 time-stamping server to countersign the signature, so that
  it stays valid after the certificate expires; without it `sign` and `build --key` warn that it
  will not. There is no default server and nothing goes over the network unless you name one.
  Public servers include `http://timestamp.digicert.com`, `http://timestamp.sectigo.com` and
  `http://time.certum.pl`; `http` is fine, because the answer is itself signed and checked (the
  nonce, the hash of the signature, the server's signature and its time-stamping certificate).
  `--tsa-trust` also requires the server's certificate path to end in one of the given
  certificates. For an `https` server rubrapack speaks TLS 1.3 itself (AES-GCM, x25519 or P-256,
  RSA-PSS or ECDSA server keys; not TLS 1.2) and checks the server's certificate - its path to a
  root given with `--tls-trust` or found in the operating system's store with `--system-roots`, its
  dates, that it names the server (subjectAltName; no wildcard across dots) and that it is for TLS
  servers; nothing turns these checks off. The three trusts stay apart: `--tls-trust` and
  `--system-roots` say whom to believe about the connection, `--tsa-trust` whom to believe about
  the time; neither is used for the other. On Linux `--system-roots` reads `$SSL_CERT_FILE` or the
  distribution's CA bundle; on Windows the ROOT store, which Windows fills with some roots only
  when something first needs them, so a root may be missing there until then. `--proxy
  http://host:port` sends the request through a proxy (an `https` server through `CONNECT`, so the
  proxy sees only the server's name); without it `https_proxy` or `HTTPS_PROXY` (for an `https`
  server) or `http_proxy` (for `http`; the upper-case `HTTP_PROXY` is not read, since a CGI
  environment can set it from a request header) are used unless `no_proxy` or `NO_PROXY` - names,
  each also covering the names under it, or `*` - lists the server. A proxy that asks for a
  password is not supported (a clear error), nor an `https://` proxy. When the timestamp fails - no answer,
  a refusal, a bad answer, a time outside the signing certificate's validity - nothing is signed
  and nothing is written (exit code 6); the signature never quietly goes out without it. The
  limits: 10 s to connect, 60 s in all, a 4 MiB answer, 3 redirects, 2 retries after a network
  error or a 5xx answer.
- `verify` prints what it checked - structure, digest, signature, the path to a certificate given
  with `--trust`, revocation (never looked up: `not-checked`) and timestamp - and succeeds only
  when all of them hold and the path ends in a trusted certificate. Without `--trust` it does not
  call anything trusted (exit code 4); `--system-roots` adds the operating system's roots to
  `--trust` for the signer's path (not for the timestamp's). It says nothing about Windows' own
  reputation checks. The
  timestamp line is `not-present`, `<time>, TSA not-checked (give --tsa-trust)`,
  `<time>, TSA trusted`, `<time>, TSA untrusted`, `invalid` or `unsupported` (the older
  Authenticode timestamp that `Set-AuthenticodeSignature -TimestampServer` writes, which rubrapack
  does not check); the last three fail. Only a timestamp whose server is trusted through
  `--tsa-trust` - `--trust` does not count for it - moves the time at which the signer's
  certificate must have been valid from now to the stamped time. (For tests, the environment
  variable `RUBRAPACK_TEST_NOW` - seconds since 1970 - replaces "now" in `verify`'s checks, so
  that an expired certificate can be checked without changing a clock; it widens nothing that a
  clock set to that time would not.)
- Exit codes: 0 success, 1 error in the source, 2 usage, 3 input/output, 4 signing, 5 lint,
  6 network.
