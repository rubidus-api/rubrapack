# Source files and the `rubrapack` command

A source file describes a package. It is a strict subset of [TOML 1.0](https://toml.io/en/v1.0.0)
and is named `*.toml`, so editors, GitHub and AI assistants treat it as TOML (the older `*.rpk`
name is read the same way). Every source is valid TOML, but rubrapack refuses TOML features
outside the subset instead of ignoring them.

## Getting started

rubrapack is a single program file. Download `rubrapack-<version>-windows-x64.exe` (rename it
`rubrapack.exe` if you like) or `rubrapack-<version>-linux-x86_64` (`chmod +x` it) from the
releases page and run it where it lies, or put it on the `PATH`. There is nothing else to install:
no runtime, no SDK, no libraries. The same program builds the same packages on Windows and on
Linux.

This section is the short version, for readers who know installers. **Part I, the tutorial**,
teaches the same from the beginning, one step at a time, and goes on to every table and option:
start with [Before you start](tutorial/01-before-you-start.md).

### A first package

Put what you ship in a folder `dist/`: the program, and whatever it needs, in `dist/files/`
(subfolders are kept). Then let rubrapack write a starting source:

```sh
rubrapack new app.toml      # asks, then writes app.toml and checks it
```

It asks for the product name, the version, the folder with the files, the main program (its
architecture is read from the file), the folder under Program Files, who it installs for, which
sub folders are optional parts, the dialogs, a license (a `LICENSE.txt`, `.md` or `.rtf` next to
it is offered), Korean dialogs, and the shortcuts. Enter takes the value in brackets. At the end
it prints the same answers as one command (`rubrapack new app.toml --dist dist --ui features ...`),
which a script or an AI assistant can run without questions. `rubrapack new` without a name asks
for the file name too, and `rubrapack new app` (no extension) writes a fixed starter instead.

Either way, `app.toml` is plain text to read and change - in any text editor, or with
`rubrapack edit app.toml`, a menu of the same questions with the current values as defaults
(version, install folder, dialogs and license, optional parts, shortcuts, and matching the file
list to the program folder after it changed). It changes only those values and keeps every other
line, comment and table as it was. For scripts: `rubrapack edit app.toml --set define.VERSION=1.1.0
--sync`. This one installs the program and its files into
`Program Files\My App`, puts it in the Start menu, and shows a dialog that lets the user change
the folder:

```toml
[package]
name = "My App"
manufacturer = "My Company"            # shown in Settings > Installed apps
version = "$(VERSION)"
arch = "x64"                           # x64, arm64 or x86
upgrade-code = "{E8C1815C-CCD7-4F3F-B914-92A4E3F3A317}"   # yours from `new`; keep it forever
ui = "installdir"

[define]
VERSION = "1.0.0"

[dir.INSTALLDIR]
path = "ProgramFiles/My App"

[file.App]
dir = "INSTALLDIR"
source = "dist/app.exe"

[files.Rest]
dir = "INSTALLDIR"
glob = "dist/files/**"

[shortcut.StartMenu]
dir = "Programs"
name = "My App"
target = "file:App"
```

```sh
rubrapack build app.toml -o app-1.0.0.msi
rubrapack build app.toml -o app-1.0.1.msi -D VERSION=1.0.1    # the next version
```

That is a complete installer. It appears in Installed apps, repairs itself, and removes everything
it installed. A higher version replaces the one installed; the same or an older one is refused
with a message. The component GUIDs, file keys, cabinet and tables are derived from the source,
so nothing but the upgrade code needs to be remembered from one version to the next.

### A license page and optional parts

Two things most installers want: the user accepts a license before installing, and some parts are
optional. `license` adds the license page (Next stays off until "I accept" is ticked), and
`ui = "features"` adds a tree in which the user picks the features to install. Here the program is
always installed and the samples are offered but not selected:

```toml
[package]
name = "My App"
manufacturer = "My Company"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{E8C1815C-CCD7-4F3F-B914-92A4E3F3A317}"
ui = "features"                        # welcome, license, folder, feature tree, ready
license = "LICENSE.txt"                # Next stays off until "I accept" is ticked

[define]
VERSION = "1.0.0"

[feature.Main]
title = "My App"
description = "The program itself."
required = true                        # always installed: the tree does not offer to leave it out

[feature.Samples]
title = "Samples"
description = "Example documents to try the program with."
level = 2                              # offered in the tree, not selected by default

[dir.INSTALLDIR]
path = "ProgramFiles/My App"
feature = "Main"

[dir.SamplesDir]
path = "INSTALLDIR/samples"
feature = "Samples"

[file.App]
dir = "INSTALLDIR"
source = "dist/app.exe"

[files.Rest]
dir = "INSTALLDIR"
glob = "dist/files/**"

[files.Samples]
dir = "SamplesDir"
glob = "dist/samples/**"

[shortcut.StartMenu]
dir = "Programs"
name = "My App"
target = "file:App"
```

Put the license text in `LICENSE.txt` (`.txt`, `.md` or `.rtf`) and the samples in
`dist/samples/`. A feature with `level = 2` is not installed unless the user ticks it; `required`
keeps the tree from offering to leave a feature out. Later the user changes the choice from
Installed apps (Change), and the command line does the same without dialogs:
`msiexec /i app.msi /qn ADDLOCAL=Samples` adds the samples, `REMOVE=Samples` takes them out
(`required` binds only the tree, not the command line). [Features](#features) and
[Conditions](#conditions-when) have the rest.

### Where the rest is explained

| To | Read |
|---|---|
| install, upgrade, remove and log with `msiexec` | [A first installer](tutorial/02-a-first-installer.md), [Versions and upgrades](tutorial/03-versions-and-upgrades.md) |
| understand an error and look inside a package (`lint`, `inspect`, `extract`, exit codes) | [Checking and looking inside](tutorial/18-checking-and-looking-inside.md), [Diagnostic codes](#diagnostic-codes) |
| sign a package | [Signing and timestamps](tutorial/16-signing.md) |
| build in a script or CI job, or with an AI assistant | [Starting fast and automating](tutorial/19-automation.md) |

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
rubrapack build example.toml -o example.msi -D VERSION=1.4.1
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
| `[feature.ID]` | **title**, description, level (1-32767), hidden, parent, required, follow-parent, when |
| `[dir.ID]` | **path** = `Base/relative/path`, feature, guard (`true`: see [Guarding the install folder](#guarding-the-install-folder)) |
| `[file.ID]` | **dir**, **source**, name, vital (default true), any-arch, feature, component-guid, keep, when |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature, keep, when |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, help (URL), about (URL), icon (`.ico`) - how the product shows in Installed apps |
| `[property.ID]` | **value**, secure, hidden - an upper-case public property |
| `[action.ID]` | **run** (`file:ID` of an `.exe` in this package), **do**, **undo**, check |
| `[registry.ID]` | **root** (`HKLM`, `HKCU`, `HKCR`, `HKMU`), **key**, name, value, type, remove, keep, view, with, feature, when |
| `[remove.ID]` | **dir**, name (`*` and `?`; omitted = the folder itself), **on** (`install`, `uninstall`, `both`), feature |
| `[ini.ID]` | **dir**, **file**, **section**, **key**, value, mode (`set`, `add`, `remove`), feature, when |
| `[require.ID]` | **condition**, **message** |
| `[search.ID]` | **property** (or a dir ID), **kind** (`registry`: root, key, name, view; `file`: path, file, min-version; `dir`: path; `component`: component-guid) |
| `[service.ID]` | **file** (`file:ID` of an `.exe`), **name**, display-name, description, start (`auto`, `demand`, `disabled`), account (`LocalSystem`, `LocalService`, `NetworkService`), args, start-on-install |
| `[assoc.ID]` | **extension** (`.ext`, lower case), **prog-id**, **target** (`file:ID` of an `.exe`), description, icon (`file:ID`), args (default `"%1"`) |
| `[protocol.ID]` | **name** (the scheme, lower case), **target** (`file:ID` of an `.exe`), description, args (default `"%1"`) |
| `[font.ID]` | **file** (`file:ID` of a file in a dir with `path = "Fonts"`), title |
| `[permission.ID]` | **target** (`dir:ID`, `file:ID`, `registry:ID`), **sddl** |
| `[env.ID]` | **name**, **value**, mode (`set`, `append`, `prepend`), keep, feature, when |
| `[copy.ID]` | **source** (`file:ID`), **dir**, name (default: the source's name) |
| `[ui]` | install-dir (a dir ID; default `INSTALLDIR`), banner (`.bmp`), launch (`file:ID`), launch-args, launch-checked, languages (added to English, e.g. `["ko"]`), license-xx, name-xx, font-xx, langid-xx - see [Several languages](#several-languages) |
| `[ui-text.ID]` | **text** or text-xx - replaces one built-in dialog text |
| `[dialog.ID]` | **after** (a built-in page or another `[dialog.*]`), title, description, title-xx, description-xx |
| `[dialog-control.ID]` | **dialog**, **type** (`text`, `checkbox`, `edit`, `radio`, `combo`), **x**, **y**, **width**, **height**, text, property, values, labels, text-xx, labels-xx |
| `[shortcut.ID]` | **dir** (a dir ID, or `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target** (`file:ID`), args, description, working-dir (a dir ID), icon (`.ico`), when |
| `[msix]` | **identity-name**, **publisher**, publisher-display-name, min-version - see [MSIX packages](#msix-packages) |
| `[msix-app.ID]` | **executable** (a `[file.*]` ID), display-name, description, logo-150, logo-44, store-logo |
| `[msix-extension.ID]` | **kind** (`alias`: **alias**; `startup-task`: task-id, display-name, enabled), app (an `[msix-app.*]` ID; default the first) - MSIX only |

`Base` in a dir path is another dir ID or one of: `ProgramFiles` (64-bit for x64/arm64, 32-bit
for x86), `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`, `CommonAppData`,
`StartMenu`, `Programs`, `Desktop`, `Startup`, `Windows`, `System`, `Fonts`, `Temp`. A path may
also be a known folder alone (`path = "Fonts"`) for files that go into that folder itself.

IDs are `[A-Za-z_][A-Za-z0-9_]*` (at most 72 characters, 38 for features) and must differ across
dirs, files and features.

### MSIX packages

The same source builds an MSIX package when the output ends in `.msix`: one desktop application
that runs with full trust, for one architecture. Two more tables say what only MSIX needs:

```toml
[msix]
identity-name = "Example.App"               # 3-50 characters: A-Z a-z 0-9 . -
publisher = "C=KR, O=Example, CN=Example"   # the signing certificate's subject, last part first
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
- The package's own folder is that of the first `[msix-app.*]`'s executable - the dir anchored in a
  known location, such as `ProgramFiles/Example App`. Files in other known locations go into the
  package's virtual file system, where the app sees them at their usual place: Program Files
  (`VFS\ProgramFilesX64`, or `X86` for an x86 package and for `ProgramFiles32`), `CommonFiles`,
  `System`, `Windows` and `CommonAppData` (ProgramData). There is none for the user's `AppData` or
  `LocalAppData`, nor for `Temp`: files there are an error (`RP1609`). A font in `Fonts` goes in
  through its `[font.*]`; the Start menu, Programs and Desktop folders take shortcuts
  (`[shortcut.*]`), and Startup a startup task (`[msix-extension.*]`).
- Several `[msix-app.*]` tables make several entries of one package (at most 100); the first one
  gives the package its logo.
- `[registry.*]` values go into the package's virtual registry, which the app sees merged into the
  real one while the machine's registry stays untouched: `HKLM` (and `HKMU`) under `Software` into
  `Registry.dat` (with `view = "32"` in the 32-bit view), `HKCU` under `Software` into `User.dat`.
  What an MSIX cannot hold is an error (`RP1612`): `HKCR` (file types and protocols are
  `[assoc.*]` and `[protocol.*]`), keys outside `Software`, `remove` and `keep`, and values with a part
  Windows Installer fills in at install time (`[INSTALLDIR]`, `[#File]`, ...; the escapes `[\[]` and
  `[\]]` are fine).
- Give the three logos or none: without them the package gets plain one-colour logos. A logo
  must have the exact size (`RP1608`).
- What an MSIX cannot do is an error, not something left out quietly (`RP1605`): custom actions,
  services, environment variables, INI files, permissions, launch conditions and searches, files
  removed or copied at install, empty folders.
  Add `msi-only = true` to such a table (or to a `[file.*]`/`[files.*]`) and the MSI keeps it while
  the MSIX is built without it. Features, properties, dialogs and `[arp]` concern the Windows
  Installer only and are not used for an MSIX.
- `--unsigned-test` adds the attribute Windows needs to install an unsigned package for testing
  (`Add-AppxPackage -AllowUnsigned`, as administrator when it contains a program); such a package
  is not for distribution and its identity differs from the signed one. `--key` signs the package
  (or the bundle and its packages) instead; see `sign` below.
- Files are compressed (`--msix-compress store` turns it off); pictures, archives and other
  already compressed files are stored. An MSIX holds no time: the same source gives the same
  bytes on Linux and Windows.
- An output ending in `.msixbundle` is a bundle: the source built once for each architecture of
  `--arch` (a list, `--arch x64,x86,arm64`; without it the source's own), each package named
  `<identity-name>_<version>_<arch>.msix` inside. Windows installs the package for its own
  architecture from it (an x64 machine takes x64 before x86). `$(ARCH)` gives each build its own
  programs: `source = "bin/$(ARCH)/app.exe"`. The bundle's version is the packages' version.

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
user. The arguments are passed as written (they are not formatted strings). `check` is accepted
but not used by this version.

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

In an MSIX a shortcut starts an application (its target must be an `[msix-app.*]` executable): one
in `Programs` or `StartMenu` is that application's own Start menu entry (no `args`); one on the
`Desktop` is written into the manifest and needs `min-version = "10.0.19645.0"` or later
(`RP1614`). Other folders, `working-dir` and `[...]` in `args` are errors there (`RP1613`); for
`Startup` use a startup task.

### File types and links: `[assoc.ID]`, `[protocol.ID]`

```toml
[assoc.Doc]
extension = ".exdoc"
prog-id = "Example.Document"      # tables sharing it describe one kind of document
description = "Example document"
target = "file:MainExe"
args = "--open \"%1\""            # the default is "%1"

[protocol.Link]
name = "example"                  # example:... opens the program
target = "file:MainExe"
```

In an MSI they are registry values under `HKEY_CLASSES_ROOT`, in the program's component: the
extension's default value is the prog-id, the prog-id has the description, `DefaultIcon` (`icon`, or
the program's first icon) and `shell\open\command`; a scheme gets `URL Protocol`. `HKEY_CLASSES_ROOT`
follows the installation: per machine they land in `HKLM\Software\Classes`, per user in
`HKCU\Software\Classes`, and removal takes them away. A type that only this program claims opens
in it directly; where the user has chosen another program, Windows keeps that choice.

In an MSIX they go into the manifest of the application whose executable is the target (a file
type association, a protocol), and `args` must be plain text. `icon` is not used there: the
application's logo stands for the file type.

### MSIX only: `[msix-extension.ID]`

```toml
[msix-extension.Cli]
kind = "alias"
alias = "example.exe"             # typed in a console, it starts the application

[msix-extension.Boot]
kind = "startup-task"             # starts with Windows once the application has run once
display-name = "Example"          # the name in Task Manager; task-id defaults to the table ID
enabled = true
```

An MSI build leaves these out. Fonts (`[font.*]`) in an MSIX are shared with other applications
(`uap4:SharedFonts`) from the package's `Fonts` folder; `title` is not used there.

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

A `registry` or `dir` search may name a dir instead of a property: the folder it finds becomes that
dir's default. This is how a package remembers the folder the user chose: write the folder down,
and look it up in the next version. A registry value is used only when that folder still exists; a
folder given on the command line (`msiexec /i app.msi INSTALLDIR=D:\Apps\Example\`) still wins.

```toml
[registry.RememberDir]
root = "HKLM"
key = "Software\\Example"
name = "InstallDir"
value = "[INSTALLDIR]"

[search.PreviousDir]
property = "INSTALLDIR"           # a dir: its default, when the registry holds an existing folder
kind = "registry"
root = "HKLM"
key = "Software\\Example"
name = "InstallDir"
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
`<name>.cab` (or `<name>-1.cab`, `<name>-2.cab`, ...), which must travel with it; without
`cab-max-size` external cabinets are split before 2 GiB each. Windows Installer cannot open a
package of 2 GiB or more, so an embedded cabinet that large is an error (`RP1516`) that asks for
`cab = "external"`. rubrapack never overwrites an existing cabinet and writes the package last:
it is built in `<out>.rp-map` (the package's bytes go straight into that file, not into memory)
and renamed to its name only when everything succeeded, so a failed build leaves no package.
Compression runs on every processor (`--jobs N` to use fewer); the bytes are the same either way. Every package also carries the
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

### Installing from the command line

Everything the dialogs choose can be given to `msiexec` instead:

| Property | What it does |
|---|---|
| `INSTALLDIR=D:\Apps\Example\` | the install folder (any dir with an upper-case ID) |
| `ADDLOCAL=Core,Extra` | install these features (`ADDLOCAL=ALL`: every feature) |
| `REMOVE=Extra` | remove these features from an installed product (`REMOVE=ALL`: everything) |
| `INSTALLLEVEL=3` | install every feature whose `level` is at most 3 |
| `RPLANGUAGE=ko` | the dialogs' language (with `[ui] languages`) |
| `ALLUSERS=1 MSIINSTALLPERUSER=""` | a dual package for everyone (default: just the current user) |
| `DESK=1`, `APP_MODE=server` | your own properties: `when` conditions, dialog values |

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
`OutOfDiskText`, `MaintTitle`, `MaintText`, `Repair`, `RepairText`, `Remove`, `RemoveText`,
`LanguageTitle`, `LanguageText`, `DirGuardText` (the guard's message below). In button texts `&` marks the access key (`&Next` is Alt+N).

### Several languages

The dialogs are in English. `[ui] languages` adds other languages to the same package; then the
first page asks for the language, and every page after it - welcome, license, folder, your own
pages, ready, progress, finished, and the cancel, error, files-in-use, disk-space and maintenance
pages - speaks the one chosen. The choice made for the user in advance is the first added language
whose LANGIDs hold the user's regional format (`UserLanguageID`), then the system locale
(`SystemLanguageID`), else English. `RPLANGUAGE=ko` on the command line chooses directly; a silent
installation (`/qn`) shows nothing and needs no choice. With English alone there is no language
page.

```toml
[ui]
languages = ["ko"]                # English is always there, and the default
license-ko = "LICENSE-ko.txt"     # a license per language (default: [package] license)

[ui-text.WelcomeText]
text-ko = "[ProductName]을(를) 설치합니다."     # text = every language, text-xx = one

[dialog.Options]
after = "RpInstallDirDlg"
title = "Options"
title-ko = "선택 사항"

[dialog-control.Mode]
# ...
labels = ["&Typical", "&Portable"]
labels-ko = ["표준(&T)", "휴대용(&P)"]
```

- The built-in texts exist in English and Korean (`ko`). Another language (`ja`, `de`, ...) gives
  every built-in text as `text-xx` (lint names the ones missing), and may give `name-xx` (its name
  on the language page), `font-xx` (the face of its dialogs) and `langid-xx` (the LANGIDs that
  choose it: one number or an array). Common languages have these built in: `ja`, `zh`, `de`,
  `fr`, `es`, `it`, `pt`, `nl`, `pl`, `ru`, `uk`, `tr`, `vi`, `th`.
- Korean dialogs use Malgun Gothic, English ones Segoe UI.
- A text that uses a property other than `[ProductName]`, `[Manufacturer]` and `[ProductVersion]`
  is formatted when the language is chosen, and must fit 255 characters.
- Windows Installer's own texts - the feature tree's menu, sizes, the time left, and its error
  messages - are not the package's: the first stay English, the error messages follow Windows.
- `[package] language` sets only the package's language (`ProductLanguage`, the summary). Since
  0.2 it no longer makes the dialogs Korean: add `languages = ["ko"]` (lint warns, `RP1317`).

### Icon, the finished page, and the scope page

- `[arp] icon = "app.ico"` is the product's icon in Installed apps; `[shortcut] icon` gives a
  shortcut its own `.ico` (without it, the program's own icon). Both are read when the package is
  built and stored in it.
- `[ui] launch = "file:App"` puts "Launch [ProductName]" on the finished page (ticked unless
  `launch-checked = false`; `launch-args` are its arguments). Finish starts the program after a
  first installation or an upgrade, as the user who ran the setup, not with the installer's
  rights; not after a repair or removal, and never at `/qn`.
- `scope = "dual"` with dialogs adds a page after the license: "Just me" (the default) or
  "Everyone on this computer", which needs administrator rights; the install folder moves to
  `%LOCALAPPDATA%\Programs` or Program Files accordingly.

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
make installing one architecture remove the other. An MSIX has no upgrade code, so `.msix` and
`.msixbundle` builds do not need these.

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
a `level` above 1 is not installed by default. With `ui = "features"` the user picks features in a
tree, and changes them later from Installed apps (Change on the maintenance page).

- `required = true`: the tree offers no "will be unavailable" for it.
- `follow-parent = true`: installed wherever its parent is (on this computer, or not at all).
- `when = "<condition>"`: the feature is off - not installed and not shown - unless the condition
  holds (see below).

### Conditions: `when`

`when` takes a Windows Installer condition (`VersionNT64`, `DESK = "1"`, `NOT OLDVERSION`) on a
feature, a file or file group, a registry value (not one `with` a file: put it on the file), a
shortcut, an environment variable or an INI value. What it guards is installed only when the
condition holds; properties set in your own dialog pages, on the command line or by a search can
be tested. The condition is evaluated when the item is first installed, a major upgrade included
(a repair keeps what is
there); an MSIX installs everything and refuses `when`.

```toml
[dialog-control.Desk]             # a check box in your own page (see below)
dialog = "Options"
type = "checkbox"
x = 20
y = 60
width = 300
height = 16
text = "Create a &desktop shortcut"
property = "DESK"

[shortcut.Desk]
dir = "Desktop"
name = "Example"
target = "file:App"
when = "DESK"
```

### Variables

`$(NAME)` inside a string value is replaced once, from `-D NAME=value` on the command line first,
then `[define]`. The result is not read again (a value containing `$(X)` stays literal).
`$$` is a literal `$`. An undefined name is an error. `$(ARCH)` is built in, unless `-D` or
`[define]` defines it: the architecture being built (`x64`, `arm64` or `x86`) - so one source can
name each architecture's program, as `source = "bin/$(ARCH)/app.exe"`.

### Program files

Files that are Portable Executables (`.exe`, `.dll`, ...) are checked: their machine type must
match `arch` (an x86 helper in an x64 package needs `any-arch = true`), and their version resource
becomes the file's version in the package, which is how Windows Installer decides whether to
replace an installed file.

### Leaving a file at removal: `keep`

`keep = true` on a file (or a file group) leaves it in place when the product is removed - for
settings the user may have changed. A repair or a later version does not overwrite such a file once
it has changed (Windows Installer's rule for files without a version). An MSIX removes all its files
and refuses `keep`.

### Guarding the install folder

A program whose files are loaded by other programs - an input method, a shell extension, a
service - must not be installed into a folder that someone else prepared: files already there
stay, and a DLL planted beside the program would be loaded with its rights. `guard = true` on a dir
makes a first installation stop before any file is placed when that folder

- already exists and its owner is not SYSTEM, Administrators or TrustedInstaller, or
- is reached through a junction or another reparse point (the folder itself or any folder above it).

A folder that does not exist yet passes (the installer creates it; `[permission.*]` can lock it).
Repair, upgrade-in-place and removal are not checked; a major upgrade is a first installation of
the new version and is checked like one (the folder the earlier version created passes). The
installation ends with 1603 after the message `DirGuardText` (`[1]` is the folder; in the language
chosen in the dialogs), also at `/qn`, and the log names the owner found. The check is done by
rubrapack's helper DLL, which the package then carries (as for `REG_QWORD` values). What is
checked is the owner: a folder owned by Administrators whose permissions let anyone write is not
refused (lock it with `[permission.*]`).

```toml
[dir.INSTALLDIR]
path = "ProgramFiles/Example"
guard = true
```

### Paths

Source paths are relative to the source file and use `/`. Absolute paths, `\`, symbolic links and
missing files are errors. Target names may use any Unicode text except what Windows forbids
(`< > : " / \ | ? *`, control characters, trailing dot or space, device names such as `CON`), and two
names in one folder may not differ only by letter case. Windows Installer stores a name together
with its 8.3 short name in 255 UTF-16 units, so a long name may use only what the short name
leaves: 242 units for `name.ext` with a three-letter extension, 246 for a name without one
(NTFS would accept 255). A longer name is refused with RP1514.

## Command line

```text
rubrapack build <src.toml> -o <out.msi|out.msix|out.msixbundle> [-D NAME=VALUE]...
                [--arch x64|arm64|x86 | --arch <list> (.msixbundle)]
                [--compress none|mszip|mszip:N] [--jobs N] [--nfc] [--reproducible]
                [<key> [--cert <chain.pem>]
                 [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                  [--proxy <URL>]] [--allow-unsigned-cabs]]
                [--unsigned-test] [--msix-compress deflate|store]      (.msix, .msixbundle)
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack inspect <file.msix|file.msixbundle> [--files | --manifest]
rubrapack inspect <file.cab>
rubrapack new [msi] <name>
rubrapack new [<file>.toml] [-i]
rubrapack new <name> [--dist <folder>] [--name ...] [--ui ...] [--optional ...] ...
rubrapack edit <file>.toml [--set <table>.<key>=<value>]... [--unset <table>.<key>]... [--sync]
rubrapack guid [--from <text>]
rubrapack lint <src.toml> [-D NAME=VALUE]... [--arch x64|arm64|x86] [--target msi|msix] [--nfc] [--strict]
rubrapack lint <file.msi> [--previous <old.msi>] [--strict]
rubrapack lint <file.msix|file.msixbundle> [--strict]
rubrapack extract <file.msi|file.msix|file.msixbundle|file.cab> -d <new dir> [--limit-entries N] [--limit-bytes N]
rubrapack sign <file.exe|.dll|.msi|.msix|.msixbundle> <key> [--cert <chain.pem>]
               [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]
rubrapack keys list [--pkcs11 <module> [--token-label <label>] [--pin-env VAR | --pin-file FILE]]
rubrapack verify <file.exe|.dll|.msi|.msix|.msixbundle> [--trust <certificates>]... [--system-roots] [--tsa-trust <certificates>]...
rubrapack version | help [command]

<key> is one of:
  --key <key.pfx|.pem> [--pass-env VAR | --pass-file FILE]                   a key file
  --pkcs11 <module> --key-label <label> [--token-label <label>]
           [--pin-env VAR | --pin-file FILE]                                  a key in a PKCS#11 token
  --key-store <SHA-1 thumbprint> [--machine-store]                            a key in the Windows store
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
  For a bundle, the identity and the packages (each opened and checked as a package), and its
  `AppxBundleManifest.xml`; `extract` writes the packages out.
- `new <name>` writes `<name>.toml`, a source that builds as soon as the program's files are in
  `dist/`, with a fresh `upgrade-code`. `new <file>.toml`, `new` without a name (which asks for the file name
  too), or `new <name> -i` asks the questions of
  [A first package](#a-first-package) instead (on stderr; answers from stdin, one per line, so they
  can be piped; the input ending before the last answer writes nothing), and `new <name>` with
  options takes the same answers without asking: `--name`, `--manufacturer`, `--version`,
  `--dist` (default `dist`), `--main <file>|-`, `--arch`, `--install-dir`, `--scope`, `--optional
  <folder,...>|-`, `--ui`, `--license <file>|-`, `--languages ko|-`, `--shortcuts
  start,desktop|none`; what is left out takes the default the question would offer. The source
  lists the folder's top-level files one by one (a shortcut names a file, and a glob cannot leave
  one out) and each sub folder that holds files as a glob; optional sub folders become features at
  `level = 2` next to a required `Main`. It is checked like `lint` after it is written (exit 1 if
  that finds a problem). It never replaces an existing file; `new app.toml` over an existing
  `app.toml` says to use `edit`.
- `edit <file>.toml` changes a source in place. Without options it shows a menu: 1 name,
  manufacturer, version (in `[define] VERSION` when `version = "$(VERSION)"`) and architecture;
  2 the folder under Program Files and the scope; 3 dialogs, license and Korean dialogs; 4 which
  sub folders are optional parts (the first one adds a required `Main` feature and gives it to
  everything that then needs a feature); 5 the Start menu and desktop shortcuts and the program
  they open; 6 the file list against the program folder - files and sub folders that are gone are
  removed, new ones added; 7 any `table.key`; `v` shows the text, `l` checks it like `lint`, `s`
  saves and checks, `q` leaves (asking first when something changed). Each question offers the
  current value. Only the values changed are rewritten - comments, order and tables written by
  hand stay; a change that would not parse is not made. With options it asks nothing, applies them
  in order and saves: `--set package.version=1.2.0` (a value that is TOML - `"text"`, a number,
  `true`, `["ko"]` - is used as it is, anything else as a string; a table that is not there is
  added), `--unset package.license`, `--sync` (6 without questions). Sources in UTF-16 are
  refused: save them as UTF-8 first.
- `guid` prints a random GUID (version 4). `guid --from <text>` prints the GUID rubrapack derives
  from a text, the same way on every machine: SHA-256 over the 32-bit little-endian length of
  `guid` and those 4 bytes, the 32-bit little-endian number 1, and the 32-bit little-endian length
  of the text and its UTF-8 bytes; the first 16 bytes of the hash, with the version nibble set to 8
  and the variant bits to `10` (RFC 9562 UUIDv8), written as `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`
  in upper case. For example `guid --from hello` gives `{B52FE8EF-B68D-84B5-91AF-E6E01BEC2773}`.
- Diagnostics look like `example.toml:12:3: error[RP1201]: unknown key 'nmae' in [package] (did you
  mean 'name'?)`.
- Before writing, `build` checks the finished tables (`RP20xx` diagnostics, exit code 5); these
  checks guard rubrapack itself, so a source that passes the `RP1xxx` checks should never meet them.
- `lint <src.toml>` runs every check `build` runs and writes nothing. It checks the source for an
  MSI; `--target msix` checks it for an MSIX instead (the `[msix]` tables, and what an MSIX
  cannot carry, such as `keep` or `when`). `lint <file.msi>` checks a
  package made by any tool with the same table rules: what stops an installation is an error
  (exit code 5) - a value that does not fit its column, a missing referenced row, a duplicate key,
  a broken dialog tab order (`RP2101`), a dialog's first/default/cancel control that is not there
  (`RP2102`), a files-in-use dialog without a `ListBox` table (`RP2103`), an error dialog without
  `ErrorText`/`ErrorIcon` (`RP2104`) - and everything else is a warning. `--strict` makes warnings
  fail too. A package in another code page than 65001 gets a note (`RP2100`): its text cannot be
  checked as UTF-8. The last line on stdout counts errors and warnings. `lint <file.msix>` checks the package the way Windows reads
  it - the ZIP, the block map, every block's hash - and that the manifest has an identity and
  names files that are in the package (`RP2201`, `RP2202`). For a bundle: its manifest's block
  map, and for each package where the manifest says it lies, its size, its identity and the
  package itself; one package per architecture.
- `lint new.msi --previous old.msi` also checks that `new.msi` upgrades `old.msi` cleanly. Errors:
  another UpgradeCode (`RP2301`: the new package does not replace the old one), a version that is
  not higher in its first three fields (`RP2302`: Windows compares only those). Warnings: the same
  ProductCode (`RP2303`: an upgrade needs a new one), a component whose GUID stays but whose key
  path (file, folder or registry value) changed (`RP2304`), or whose 64-bit flag changed
  (`RP2305`), a component that is gone (`RP2306`: its resources are removed with the old version),
  a feature that is gone (`RP2307`: a patch or a change of the installed set loses it). The last
  line names the previous package.
- `extract` unpacks a package the way it installs: folders by their long names under the
  Directory tree (a standard folder such as `ProgramFiles64Folder` keeps its name), files from the
  embedded or external cabinets, or from the source folders next to an uncompressed package.
  Every file is checked against its size and `MsiFileHash`. The target must be new or empty, and
  nothing is written until the whole package has passed: names with `..`, `/`, `\`, `:`, a drive,
  a reserved device name (`CON`, `COM1`, ...), a trailing dot or space, or two paths that differ
  only in case are refused. The defaults allow 100,000 entries and 16 GiB. A `.cab` unpacks by the
  names inside it. An `.msix` unpacks its files (not the ZIP's own
  `[Content_Types].xml` and `AppxBlockMap.xml`) after every block has matched its hash.
- `sign` adds an Authenticode signature (SHA-256; RSA, or ECDSA P-256/P-384) to a PE file, an MSI
  package, an MSIX package or a bundle (whose packages are signed first), in place or
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
  the command line. A file that is signed already is refused. An MSIX's `[msix] publisher` must be
  the certificate's subject as Windows writes it - its parts from last to first, as
  `O=Example Ltd, CN=Example` - or signing stops and prints the subject to use; `--unsigned-test`
  and a key do not go together.
- A key that must not leave its hardware - required for publicly trusted code-signing
  certificates since 2023 - stays there: rubrapack computes the hashes, the CMS structure and the
  timestamp request itself and asks only for the one signature. `--pkcs11 <module>` loads the
  token's PKCS#11 library (the only library rubrapack ever loads, and only this one you name),
  logs in with the PIN from `--pin-env` or `--pin-file` (never the command line), and uses the
  private key labelled `--key-label` with the certificate stored under the same ID on the token
  (`--cert` supplies it, or the rest of the chain, when the token has none); with several tokens
  present, `--token-label` picks one. On Windows, `--key-store <thumbprint>` uses a certificate
  in the current user's personal store (`--machine-store`: the local machine's) whose private key
  is reachable through NCrypt - a smart card, a token or TPM through its key storage provider, or a
  software key - and takes the chain Windows builds for it. RSA and ECDSA keys both work. Every
  signature is checked with the certificate before it is written.
- `keys list` shows the keys that can sign: with `--pkcs11`, each private key on the token(s) with
  its label, ID and certificate; on Windows without it, the certificates with a private key in the
  personal stores, with their thumbprints. For each it prints the MSIX publisher that certificate
  signs for and when it expires; nothing secret.
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

## Diagnostic codes

Every problem rubrapack reports has a code, `error[RPnnnn]` or `warning[RPnnnn]`, after the file,
line and column it is about. The first two digits say what kind of problem it is:

| Codes | What went wrong | Where to look |
|---|---|---|
| RP00xx | the command line: an unknown command or option, a file that cannot be read or written | `rubrapack help <command>` |
| RP10xx | the source file's encoding: not UTF-8 (or UTF-16 with a BOM), stray carriage returns | save the file as UTF-8 |
| RP11xx | TOML outside the subset rubrapack reads: multi-line strings, inline tables, a table defined twice | [The TOML subset](#the-toml-subset) |
| RP12xx | tables and keys: an unknown table or key (with a suggestion), a required key or table missing, something without a feature once features exist | [Tables](#tables) |
| RP13xx | values: IDs (unique across all tables, not reserved), GUIDs, versions, numbers out of range, references to things that do not exist | the table's section |
| RP14xx | variables: `$(NAME)` without a value, `$(` not closed | [Variables](#variables) |
| RP15xx | the files to install: not found, a directory, a link, a glob without a match, a program for another architecture, names too long, a file that changed during the build, a package too large | [Paths](#paths), [Program files](#program-files) |
| RP16xx | MSIX: what an MSIX package needs, and what it cannot carry | [MSIX packages](#msix-packages) |
| RP19xx | a feature this rubrapack does not provide | - |
| RP20xx | the finished tables break a Windows Installer rule. A source that passes the RP1xxx checks should never meet these: please report it | - |
| RP21xx | `lint` of a package: dialogs, code page, text normalisation | `lint` under [Command line](#command-line) |
| RP22xx | `lint` of an MSIX package or bundle | `lint` under [Command line](#command-line) |
| RP23xx | `lint --previous`: this package would not upgrade the previous one cleanly | [Versions and upgrades](tutorial/03-versions-and-upgrades.md) |

The message says what to change; the codes above are the ones to search the manual for.
