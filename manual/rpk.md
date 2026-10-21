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
format = 1

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
path = "$(ProgramFiles)/My App"

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
format = 1

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
path = "$(ProgramFiles)/My App"
feature = "Main"

[dir.SamplesDir]
path = "$(INSTALLDIR)/samples"
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
format = 1

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
path = "$(ProgramFiles)/Example App"

[dir.Docs]
path = "$(INSTALLDIR)/docs"

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

## Source format

The first key of a source, before any table, is `format = 1`: the version of the source format,
one whole number. It goes up only when sources must be written differently; new tables and keys
do not change it. A source without it is taken for one of rubrapack 0.18 or earlier and draws a
warning (`RP1108`); what changed since is then refused where it is written - format 1 starts paths
with `$(...)` (see [Paths](#paths)). A source with a higher number needs a newer rubrapack.

## The TOML subset

Accepted: tables `[kind]` and `[kind.ID]`, bare keys (`A-Z a-z 0-9 _ -`), basic strings `"..."`
with TOML escapes, literal strings `'...'` (no escapes - use them for backslashes and quotes:
`'SOFTWARE\Example'`), decimal and `0x` integers, `true`/`false`, one-type arrays, `#` comments.
UTF-8 with or without a BOM, or UTF-16LE with a BOM; LF or CRLF.

Refused with an error: multi-line strings, inline tables, arrays of tables, dotted and quoted
keys, table names with more than two parts, floats, dates, `_` in numbers, octal/binary numbers,
empty or mixed arrays, keys before the first table other than `format`. Table and key order never matters.

## Tables

| Table | Keys (required in bold) |
|---|---|
| `[package]` | **name**, **manufacturer**, **version** (`a.b.c` or `a.b.c.d`), **arch**, **upgrade-code**, upgrade-code-x64 / -arm64 / -x86, product-code, summary-name (ASCII), language, scope (`machine`, `user`, `dual`), ui (`none`, `basic`, `minimal`, `installdir`, `features`), license (`.txt`, `.md`, `.rtf`), reboot (`suppress`/`allow`), cleanup (`false`: no cleanup task), preflight (`false`: no checks before it goes on), close-programs (`ask`, `always`, `never`), extra-files (`ask`, `keep`, `delete`), parent (an add-on: the main product's upgrade-code), remove-addons (`true`: removing this product removes its add-ons), replaces (upgrade codes of products this package takes the place of), downgrade-message, compress (`none`, `mszip`, `mszip:0`..`mszip:9`, `lzx`, `lzx:15`..`lzx:21`; default `mszip:6`), cab (`embed` or `external`), cab-max-size (MiB), refuse-upgrade-below, refuse-upgrade-message, instances (N >= 2: N-1 named instances, machine only) |
| `[define]` | variables: `NAME = "value"` |
| `[feature.ID]` | **title**, description, level (1-32767), hidden, parent, required, follow-parent, when, default-when, advertise (`allow` default, `disallow`) |
| `[dir.ID]` | **path** = `$(Base)/relative/path`, feature, guard (`true`: see [Guarding the install folder](#guarding-the-install-folder)) |
| `[file.ID]` | **dir**, **source**, name, vital (default true), any-arch, feature, component-guid, keep, when, isolate-for (`file:ID` of an `.exe`: a private copy of this DLL beside that program, MSI only) |
| `[files.ID]` | **dir**, **glob**, vital, any-arch, feature, keep, when |
| `[folder.ID]` | **dir**, **name**, keep, feature |
| `[arp]` | no-modify, no-repair, no-remove, help (URL), about (URL), icon (`.ico`), logo (a `.png`: 8-bit RGB or RGBA, at most 1024 pixels a side; the menu package's logos) - how the product shows in Installed apps |
| `[property.ID]` | **value**, secure, hidden - an upper-case public property |
| `[action.ID]` | a do/undo pair: **run** (`file:ID` of an `.exe`), **do**, **undo**, check (exits 0 when what `do` made is there; for the report after a failed rollback). Or one action: **run** with args, or **dll** (`file:ID` of an installed `.dll`) or **binary** (a `.dll` in the package) with **entry**; phase (`immediate`, `deferred` default, `commit`, `rollback`), as (`system`, `user`), when, data, before / after (a standard action) or sequence, ignore-exit. See [Running a program or DLL](#running-a-program-or-dll-actionid) |
| `[registry.ID]` | **root** (`HKLM`, `HKCU`, `HKCR`, `HKMU`), **key**, name, value, type, remove, on (with `remove`: `install`, or `uninstall` - the whole key at removal), users (`all`: every user's, see [Every user's](#every-users-users--all)), keep, view, with, feature, when |
| `[remove.ID]` | **dir**, name (`*` and `?`; omitted = the folder itself), **on** (`install`, `uninstall`, `both`), upgrade (`false`: not when an upgrade removes this version), recursive (`true`: the folder with everything in it, at removal), users (`all`: every user's, see [Every user's](#every-users-users--all)), feature |
| `[ini.ID]` | **dir**, **file**, **section**, **key**, value, mode (`set`, `add`, `remove`), feature, when |
| `[require.ID]` | **condition**, **message** |
| `[search.ID]` | **property** (or a dir ID), **kind** (`registry`: root, key, name, view; `file`: path, file, min-version; `dir`: path; `component`: component-guid) |
| `[service.ID]` | **file** (`file:ID` of an `.exe`), **name**, display-name, description, start (`auto`, `demand`, `disabled`), account (`LocalSystem`, `LocalService`, `NetworkService`), args, start-on-install |
| `[assoc.ID]` | **extension** (`.ext`, lower case), **prog-id**, **target** (`file:ID` of an `.exe`), description, icon (`file:ID`), args (default `"%1"`), content-type, perceived-type, default (`false`: only under "Open with") |
| `[menu.ID]` | **on** (file types, `"*"`, `"folder"`, `"background"`, `"drive"`, `"assoc:ID"`; an item of a sub-menu has none), **text** or text-xx, target (`file:ID` of an `.exe`; none = a sub-menu), args (default `"%1"`), icon (`file:ID`), parent (a sub-menu's ID), multi (`each`, `one`, `single`), extended, windows11 (default `true`), list (with `multi = "one"`: what stands for `%*` when the paths do not fit a command line; `%L` is a file of them), hide-when (`"HKCU\\key\\value"` or `"HKLM\\..."`: the Windows 11 item is hidden while that value exists) - an item of Explorer's right-click menu: see [Explorer's right-click menu](#explorers-right-click-menu-menuid) |
| `[other-copy.ID]` | **msix** (an app package's identity name) with **publisher**, or **upgrade-code** (another MSI's), action (`warn` default, `refuse`), message or message-xx (`[1]` this product, `[2]` the copy found) - the same program installed another way, looked for before a first install: see [Another copy](#another-copy-other-copyid). MSI only |
| `[protocol.ID]` | **name** (the scheme, lower case), **target** (`file:ID` of an `.exe`), description, args (default `"%1"`) |
| `[com.ID]` | **file** (`file:ID` of an `.exe` or `.dll`), **class** (`{GUID}`), description, threading (`sta` default, `mta`, `both`, `neutral`; a DLL), args (a program), prog-id, app-id (`{GUID}`), surrogate (a DLL in dllhost), typelib (`{LIBID}`), typelib-version (default `"1.0"`), typelib-file (default: the server), msi-only - a COM class, see [COM classes](#com-classes-comid) |
| `[handler.ID]` | **kind** (`thumbnail`, `preview`, `property`), **class** (a `[com.*]` DLL class), **types** (`[".ext", ...]`), description (a preview handler's name), msi-only - an Explorer handler, see [Explorer handlers](#explorer-handlers-handlerid) |
| `[font.ID]` | **file** (`file:ID` of a file in a dir with `path = "$(Fonts)"`), title |
| `[permission.ID]` | **target** (`dir:ID`, `file:ID`, `registry:ID`), **sddl** |
| `[env.ID]` | **name**, **value**, mode (`set`, `append`, `prepend`), keep, feature, when |
| `[copy.ID]` | **source** (`file:ID`), **dir**, name (default: the source's name) |
| `[move.ID]` | **from** (a dir ID, or `property:NAME`), **name** (wildcards), **to** (a dir ID), new-name, copy (`true`: the source stays), with (`file:ID`) or feature - files moved or copied at installation. MSI only |
| `[odbc-driver.ID]` | **file** (`file:ID` of the driver `.dll`), **description**, setup (`file:ID` of its setup `.dll`), attributes (`["Key=Value", ...]`) - an ODBC driver. MSI only |
| `[odbc-source.ID]` | **driver** (an `[odbc-driver]` ID) or **driver-name** (with **with**), **name**, registration (`machine`, `user`), attributes - an ODBC data source. MSI only |
| `[publish.ID]` | **file** (`file:ID`: its component is published), **category** (`{GUID}`), **qualifier**, data - a component other programs find with `MsiProvideQualifiedComponent`. MSI only |
| `[merge.ID]` | **source** (an `.msm` merge module), **dir** (where the module's own root goes), feature, config (`["Name=value", ...]` for a configurable module); in an MSIX its files and registry values only |
| `[module]` | **name** (the module's ID: letters, digits, `_`; at most 35), **manufacturer**, **version**, **arch**, **id** (the module's GUID, kept in every version), language (`neutral` default, `en-US`, `ko-KR`), compress (`none`, `mszip`, `mszip:0`..`mszip:9`) - in place of `[package]`: a merge module, see [Writing a merge module](#writing-a-merge-module-module) |
| `[ui]` | install-dir (a dir ID; default `INSTALLDIR`), banner (`.bmp`), launch (`file:ID`), launch-args, launch-checked, save-log (default `true`), details (default `true`: the progress page shows what is being done), embedded (a `.dll`: the author's own UI handler, MSI only), languages (added to English, e.g. `["ko"]`), license-xx, name-xx, font-xx, langid-xx - see [Several languages](#several-languages) |
| `[ui-text.ID]` | **text** or text-xx - replaces one built-in dialog text |
| `[dialog.ID]` | **after** (a built-in page or another `[dialog.*]`), title, description, title-xx, description-xx |
| `[dialog-control.ID]` | **dialog**, **type** (`text`, `checkbox`, `edit`, `radio`, `combo`, `listbox`, `bitmap`), **x**, **y**, **width**, **height**, text, property, values, labels, image (`.bmp`, bitmap only), text-xx, labels-xx |
| `[billboard.ID]` | **image** (`.bmp`), **feature**, order (MSI only; shown on the progress page while the feature installs) |
| `[shortcut.ID]` | **dir** (a dir ID, or `Programs`, `Desktop`, `StartMenu`, `Startup`), **name**, **target** (`file:ID`), args, description, working-dir (a dir ID), icon (`.ico`), when, advertise (`true`: see [Shortcuts](#shortcuts-shortcutid), MSI only) |
| `[msix]` | **identity-name**, **publisher**, display-name, display-name-xx, publisher-display-name, publisher-display-name-xx, min-version, capabilities (names), file-system-virtualization, registry-virtualization (default `true`), main-package, main-publisher, modification, language-packs (default `true`), appinstaller-uri, package-uri, update-hours (0-255, default 24), update-prompt, update-blocks, update-background - see [MSIX packages](#msix-packages) |
| `[msix-app.ID]` | **executable** (a `[file.*]` ID), display-name, display-name-xx, description, description-xx, logo-150, logo-44, store-logo (each with optional `.scale-NNN` variants), background-color (`transparent` default, `#RRGGBB`), hidden (no Start menu entry) |
| `[msix-dependency.ID]` | **name**, **publisher**, **min-version** - a framework package the MSIX needs - MSIX only |
| `[msix-extension.ID]` | **kind** (`alias`: **alias**; `startup-task`: task-id, display-name, enabled; `firewall`: **direction** (`in`, `out`), **protocol** (`tcp`, `udp`), ports (`8080` or `8000-8100`), profile (`all`, `domain`, `private`, `public`), file (default: the application's program); `com-server`: **file** (an `.exe` or `.dll`), **class** (`{GUID}`), display-name, args (`.exe`), threading (`sta` default, `mta`, `both`, `neutral`; `.dll`); `toast`: **class**, file (default: the application's program), args (default `-ToastActivated`); `context-menu`: **file** (a `.dll`), **class**, **types** (`[".txt", "*"]`), verb (default: the table ID; letters and digits only), threading), app (an `[msix-app.*]` ID; default the first) - MSIX only |
| `[chain]` | **name**, **manufacturer**, **version**, arch (the setup program's: `x64`, `x86`, `arm64`), elevate (default `true`), languages (codes with a language pack, for the setup program's own texts) - a chain source: see [Several packages in one setup](#several-packages-in-one-setup-chain) |
| `[chain-package.ID]` | **source** (an `.msi`), properties (msiexec properties), vital (default `true`) |

`$(Base)` in a dir path is another dir ID or a Windows folder: `$(ProgramFiles)` (64-bit for
x64/arm64, 32-bit for x86), `$(ProgramFiles(x86))`, `$(CommonProgramFiles)`, `$(APPDATA)`,
`$(LOCALAPPDATA)`, `$(ProgramData)`, `$(TEMP)`, `$(SystemRoot)`, and the folders without an
environment variable `$(StartMenu)`, `$(Programs)`, `$(Desktop)`, `$(Startup)`, `$(System)`,
`$(Fonts)` - see [Windows names](#windows-names). A path may also be a folder alone
(`path = "$(Fonts)"`) for files that go into that folder itself.

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
  known location, such as `$(ProgramFiles)/Example App`. Files in other known locations go into the
  package's virtual file system, where the app sees them at their usual place: Program Files
  (`VFS\ProgramFilesX64`, or `X86` for an x86 package and for `ProgramFiles(x86)`),
  `CommonProgramFiles`, `System`, `SystemRoot` and `ProgramData`. There is none for the user's
  `APPDATA` or `LOCALAPPDATA`, nor for `TEMP`: files there are an error (`RP1609`). A font in `Fonts` goes in
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
  must have the exact size (`RP1608`). Files named like the logo with `.scale-100`, `.scale-125`,
  `.scale-150`, `.scale-200` or `.scale-400` before the extension are the same logo for screens
  at that scale, each its size times the scale (`Square44x44.scale-150.png` is 66x66); the logo
  itself, when there, counts as scale 100, so it cannot be there together with `.scale-100`.
- `display-name-xx`, `description-xx` (`[msix-app.*]`) and `display-name-xx`,
  `publisher-display-name-xx` (`[msix]`; `display-name` defaults to the package name) give those
  texts in another language (`xx` as in `[ui]`: `ko`, `ja`, `de`, ...); the text without a suffix is
  the one in the package's own language, so a suffix for that language is an error (`RP1606`).
  The manifest then refers to them as `ms-resource:` names and lists the languages.
- Logos in several scales and texts in several languages go into `resources.pri`, the package
  resource index Windows resolves them through; a package without them has none.
- What an MSIX cannot do is an error, not something left out quietly (`RP1605`): custom actions,
  permissions, launch conditions and searches, files removed at install, empty folders. A
  `[copy.*]` puts the file in the package a second time, at the copy's place.
- `[ini.*]` entries become INI files in the package (beside the program, or in the virtual file
  system), written when the package is built: ASCII, or UTF-16 with a BOM when a text is not
  ASCII; `add` joins the values; `remove` has nothing to remove in a new package. A value with an
  install-time part (`[...]`) or `when` is refused (`RP1612`).
- `[env.*]` cannot change the computer's environment from an MSIX. The variables go to the
  application's own processes instead: every application then starts through rubrapack's launcher
  (`rubrapack\<AppId>.exe`, a console program for a console application), which reads
  `rubrapack\launch.txt`, sets the variables (`append`/`prepend` join with `;` to what is there),
  starts the program in its own folder with the command line it was given and returns its exit
  code. In a value, a dir of the package becomes its real folder, a Windows folder or name its
  environment variable, `%NAME%` is read when the program starts; a dir elsewhere, an
  install-time part or `when` is refused (`RP1612`). Execution aliases and startup tasks start the
  launcher too; services, COM servers and firewall rules name the program itself.
- `capabilities` declares what the application asks Windows for beyond `runFullTrust`, which every
  package has; rubrapack writes each name with the element the manifest schema wants for it
  (`RP1616` for a name it does not know):
  - `Capability`: internetClient, internetClientServer, privateNetworkClientServer, allJoyn, codeGeneration
  - `uap:Capability`: documentsLibrary, picturesLibrary, videosLibrary, musicLibrary, removableStorage,
    enterpriseAuthentication, sharedUserCertificates, appointments, contacts, userAccountInformation,
    objects3D, phoneCall, voipCall, chat, blockedChatMessages
  - `uap3:Capability`: backgroundMediaPlayback, remoteSystem, userNotificationListener; `uap6`:
    graphicsCapture; `uap7`: globalMediaControl
  - `rescap:Capability` (restricted: the Microsoft Store asks why): allowElevation, unvirtualizedResources,
    broadFileSystemAccess, packageManagement, packageQuery, confirmAppClose, appDiagnostics,
    appLicensing, localSystemServices, packagedServices, extendedExecutionUnconstrained,
    extendedBackgroundTaskTime, inputForegroundObservation, inputObservation, inputSuppression,
    inputInjectionBrokered, uiAccess, interopServices, customInstallActions, modifiableApp,
    appCaptureSettings
  - `DeviceCapability`: webcam, microphone, location, bluetooth, proximity, radios, wiFiControl,
    lowLevel, gazeInput
  A desktop application that runs with full trust needs few of them: `allowElevation` lets it start
  elevated, the libraries and device names matter for the APIs that check them.
- `file-system-virtualization = false` and `registry-virtualization = false` let the application
  write where it really is (its install folder, `HKCU`) instead of the package's private copy;
  they add `unvirtualizedResources` and need `min-version = "10.0.18362.0"` or later (`RP1614`).
  Microsoft reserves them for certain games in the Store; sideloaded packages may use them.
- `[msix-dependency.ID]` names a framework package the application needs - the C++ runtime
  `Microsoft.VCLibs.140.00.UWPDesktop` (publisher `CN=Microsoft Corporation, O=Microsoft Corporation,
  L=Redmond, S=Washington, C=US`), the Windows App SDK - with the oldest version that will do;
  Windows refuses to install the package without it. An MSI build leaves the table out.
- `background-color` colours the application's tile and `hidden = true` keeps an application (a
  helper, say) out of the Start menu.
- `main-package = "Contoso.Main"` (the main package's identity name) makes the package an
  *optional package* of it: content, or more applications, that run in the main package's
  container and can only be installed with it (`uap3:MainPackageDependency`; Windows 10 1703).
  `main-publisher` names the main package's publisher when it differs (outside the Store only).
  With `modification = true` it is a *modification package* instead: files (in the virtual file
  system) and registry values that change how the main application is set up - an enterprise's
  settings, say - without applications of its own (`rescap6:ModificationPackage`; Windows 10
  1903, `min-version = "10.0.18362.0"`). Neither kind has `[msix-app.*]` tables necessarily, nor
  capabilities or virtualization settings: the main package's apply (`RP1617`). Without
  applications the dir `INSTALLDIR` is the package's own folder (an optional package; a
  modification package keeps everything in the virtual file system).
- In a bundle, the texts of each language other than the package's own (`display-name-xx`,
  `description-xx`, `publisher-display-name-xx`) go into a resource package of that language
  (`<identity>_<version>_language-xx.msix`, `ResourcePackage`, no execution) with its part of
  `resources.pri`; Windows installs those of the user's languages. `language-packs = false` keeps
  every language in each architecture's package.
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
user. The arguments are passed as written (they are not formatted strings).

If a later step fails, Windows Installer undoes the installation, and the commands above put the
registration back. Should that undoing itself go wrong, the user gets a report: a text file on the
desktop (or in Downloads) named `<product> setup report <date and time>.txt`, in the user's language,
saying what was being done, what is not as it was, what to do next, and where the installation's log
is. It looks at the older version an upgrade was replacing (installed again?) and at each
`check`:

```toml
[action.Tip]
run = "file:MainExe"
do = "--register"
undo = "--unregister"
check = "--status"        # exits 0 when what --register made is there
```

After the undoing, `check` should exit 0 when the product was there before (an upgrade, a repair, a
removal) and not 0 after a first installation that failed. When everything is as it was, nothing is
written. `check` is a formatted string (`[INSTALLDIR]` is the install folder).

### Running a program or DLL: a single `[action.ID]`

Besides the do/undo pair above, one `[action.*]` runs one thing once:

```toml
[action.Warm]
run = "file:MainExe"       # an .exe this package installs
args = "--warm-cache"      # a formatted string ([INSTALLDIR] and the like are filled in)
phase = "deferred"         # immediate | deferred (default) | commit | rollback
when = "NOT Installed"     # a condition (optional)
before = "InstallFinalize" # where it goes: before / after a standard action, or sequence = <n>

[action.Ping]
dll = "file:HelperDll"     # a .dll this package installs
entry = "PingServer"       # the function to call
phase = "deferred"

[action.Clean]
binary = "clean.dll"       # a .dll carried in the package, not installed
entry = "CleanUp"
phase = "commit"
as = "system"              # system | user (a per-machine package's deferred actions are SYSTEM by default)
data = "[INSTALLDIR]"      # what the deferred DLL reads as its CustomActionData (a formatted string)
```

- One of `run` (an exe), `dll` (an installed DLL) or `binary` (a DLL in the package, in the Binary
  table). An exe takes `args`; a DLL takes `entry` (its function) and, when deferred, `data`.
- `phase`: an **immediate** action runs while the install is still being worked out (it may read
  and set properties, and runs as the installing user); a **deferred** one runs during the install
  with the changes it makes; a **commit** one after the install has succeeded; a **rollback** one
  when the install is undone. Only a deferred, commit or rollback action takes `as` and `data`.
- `as`: who a deferred action runs as. A per-machine package runs them as SYSTEM, a per-user one as
  the user; `as = "user"` keeps a per-machine action as the installing user, `as = "system"` (a
  per-machine package only) makes one SYSTEM.
- Placement: `before` or `after` a standard action (`InstallFiles`, `WriteRegistryValues`,
  `InstallFinalize`, ...), or `sequence = <number>`. A deferred action that runs an installed file
  must come after `InstallFiles` (its number is above 4000). Two actions may not land on one number.
- `ignore-exit = true`: the install goes on whatever the action returns. By default a non-zero exit
  (or a DLL that returns failure) fails the install.

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
omitted - during installation. With `on = "uninstall"` (and no `name`) it is the removal that
deletes the key, with everything below it: see [Wiping at removal](#wiping-at-removal-recursive-on--uninstall).

### Shortcuts: `[shortcut.ID]`

```toml
[dir.Menu]
path = "$(Programs)/Example"         # Start menu > Example

[shortcut.Settings]
dir = "Menu"
name = "Example settings"         # ".lnk" is added
target = "file:MainExe"
args = "--settings \"[INSTALLDIR]\""
```

A shortcut belongs to its target file (and its feature); folders created for it are removed at
uninstall. In a per-machine package `Programs` and `Desktop` are the all-users Start menu and the
Public Desktop.

`advertise = true` makes an advertised (install-on-demand) shortcut: its target is the target
file's feature, which Windows installs the first time the shortcut is used and repairs if anything
is missing. An advertised shortcut needs an `icon` (Windows shows it before the feature is
installed) and is not combined with `when`. A feature may be kept from being advertised with
`advertise = "disallow"` (an advertised shortcut to it is then refused). Advertised shortcuts are
for an MSI; an MSIX installs the whole package, so `advertise` is refused there.

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

A file type may also say what it holds and whether the program takes it:

```toml
[assoc.Picture]
extension = ".png"
prog-id = "Example.Picture"
target = "file:MainExe"
content-type = "image/png"        # the type's media type
perceived-type = "image"          # image, text, audio, video, compressed, document, system, application
default = false                   # offer the program under "Open with", do not claim the type
```

Every `[assoc.*]` lists its prog-id under the extension's `OpenWithProgids`, so the program is
offered under "Open with"; with `default = false` that is all it does (the extension's own value is
left alone). In an MSIX `content-type` goes into the manifest; `perceived-type` and `default` are an
MSI's.

### Explorer's right-click menu: `[menu.ID]`

```toml
[menu.Convert]
on = [".png", ".jpg", "folder"]   # where it shows
text = "Convert with Example"
text-ko = "Example 로 변환"        # a text per language
target = "file:MainExe"
args = "--convert \"%1\""         # "%1" is the selected path; the default is "%1" alone
icon = "file:MainExe"             # default: the program's own icon

[menu.Tools]                      # no target: a sub-menu
on = "*"
text = "Example tools"

[menu.Checksum]
parent = "Tools"                  # an item of that sub-menu; it shows where the sub-menu shows
text = "Checksum"
target = "file:MainExe"
args = "--sum \"%1\""
multi = "single"                  # only when one thing is selected
```

An item starts a program of the package with what was right-clicked. `on` is one place or a list:
a file type (`".png"`), `"*"` (every file), `"folder"`, `"background"` (the empty part of an open
folder: `%1` is then that folder), `"drive"`, or `"assoc:ID"` (the file type of an `[assoc.*]`, under
its prog-id). `multi` says what several selected items mean: `"each"` (the default: one run per
item), `"one"` (one run with all of them: write `%*` in `args` for all the paths) or `"single"` (the
item shows only for one). `extended = true` shows the item only with Shift held. One level of
sub-menus. The ID of an item that is not in a sub-menu names it in Windows 11's menu, which takes
letters and digits only (`Open2`, not `Open_2`); the items are listed in the order of their IDs.

The same table serves both menus of Windows:

- **The classic menu** (Windows 10; "Show more options" on Windows 11) reads registry verbs, which
  the package writes under each place (`SystemFileAssociations\.png\shell\<Product>.<ID>`, `*`,
  `Directory`, `Directory\Background`, `Drive`). No code of the package runs inside Explorer. A verb
  has one text: with `text-xx` it is the text of the installation's language (the language of the
  dialogs, else the user's display language among `[ui] languages`). `"one"` runs once per item
  there, as `"each"`.
- **The Windows 11 menu** shows only items that a package with identity declares, each a COM class.
  rubrapack supplies that class: its menu part `rubrapack_menu.dll` serves every item, shows the
  text in the user's display language and starts the program. It runs in `dllhost.exe`, not in
  Explorer, and does nothing but what the installation wrote down for the item.
  - An **MSIX** has identity: the DLL goes into the package and the manifest declares the items.
  - An **MSI** installs two more files beside the item's program - the DLL and
    `rubrapack_menu.msix`, a small package with only a manifest and logos (made from `[arp] logo`, a
    PNG, or else the largest 32-bit or PNG image of `[arp] icon`, fitted and centred; else plain) -
    and registers that
    package for the program's folder when it is installed (for every user of the computer, others
    at their next sign-in) and removes it when it is removed. **It needs no certificate**: the
    package is unsigned in the form Windows accepts from an installation with administrator
    rights. With `--key` and `[msix] publisher` (the certificate's subject) it is signed instead.
    Where it cannot be registered - Windows before 10 version 2004, or a per-user installation by
    a user without administrator rights - the installation goes on and the items are in the classic
    menu only; the log says so (`rubrapack: menu:`).
- Where the package is registered Windows shows its items in the classic menu too, so the registry
  verbs are switched off there (`LegacyDisable`): each item shows once.

What to expect on Windows 11: an item appears some seconds after the installation ends; two or
more items of one package for the same kind of thing are put by Windows under one entry named
after the product (it stays when `hide-when` hides all but one); in an MSIX, an item for a file type no program
on the computer has registered (an extension Windows only calls "RPX file") was not shown, while
the same item on `.txt` was; `"drive"` and `windows11 = false` items are in the classic menu only. To see
what the menu part does, set the value `MenuLog` (a file's path) under
`HKCU\Software\rubrapack`: each choice and the command it starts are appended there.

Two keys concern the Windows 11 menu only:

```toml
[menu.Open]
on = [".png", ".jpg"]
text = "Open with Example"
target = "file:MainExe"
args = "--open %*"
multi = "one"
list = "--paths-from=%L"          # when the paths do not fit a command line
hide-when = "HKCU\\Software\\Example\\MenuRegistered"
```

- **`list`**: a command line holds about 32,000 characters, a few hundred paths. When `args` with
  every path does not fit, the paths go in a file of the user's temporary folder
  (`rubrapack-selection-<numbers>.txt`: UTF-8, one path a line, each ended by a line feed), and
  `list` stands where `%*` was, with `%L` the file's path (quoted). The program reads the file and
  deletes it. A selection that fits goes on the command line as before. It needs `multi = "one"`,
  `%*` in `args` and `%L` in `list`. Without `list` such a selection starts nothing (the menu part's
  log says so).
- **`hide-when`**: a registry value, `HKCU` or `HKLM`, then the key, then the value's name after the
  last backslash. While that value exists the item, and a sub-menu with all its items, is hidden.
  It serves two copies of one program on one computer - say an MSI of your own and the Microsoft
  Store's package: the program installed one way writes the value when it puts its own menu in,
  and the other copy's items step aside. A value the package itself writes is refused (the item
  would never show). Not for an item inside a sub-menu, and not for the classic menu (Windows has
  no such test there).

### COM classes: `[com.ID]`

```toml
[com.Widget]
file = "file:WidgetDll"           # an .exe or a .dll of this package serves the class
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D35}"
description = "Example widget"
prog-id = "Example.Widget"        # what scripts name it by
threading = "both"                # a DLL's apartment: sta (default), mta, both, neutral
app-id = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D36}"
surrogate = true                  # may run in dllhost, out of the caller's process
typelib = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D37}"
typelib-version = "1.2"           # major.minor in hex digits; typelib-file names another file

[com.Server]
file = "file:MainExe"             # a program: LocalServer32, started with its args
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D38}"
args = "-Embedding"
```

In an MSI these are registry values under `HKEY_CLASSES_ROOT`, in the server's component, as for file
types: `CLSID\{class}` with its description; `InprocServer32` (the DLL and `ThreadingModel`) or
`LocalServer32` (the program, quoted, then `args`); with a prog-id, `CLSID\{class}\ProgID` and
`<prog-id>\CLSID`; with an app-id (or `surrogate`, which uses the class ID when there is no app-id),
the class's `AppID` value and `AppID\{app-id}`, with `DllSurrogate` for a surrogate; with a typelib,
`CLSID\{class}\TypeLib` and `TypeLib\{typelib}\<version>` (`0\win64` or `0\win32` by the package's
architecture, `FLAGS`, `HELPDIR`). Removal takes them away. The values are written as Registry rows -
as Microsoft's tools write a class that is not advertised - rather than through the Class, ProgId,
TypeLib and AppId tables, whose classes Windows Installer registers as advertised (installed on first
use); a per-user installation puts them in `HKCU\Software\Classes`. Microsoft's validation (ICE33)
warns about such rows - it would have the tables used - and the warning is expected for `[com]`. A class ID, a prog-id and an ID
are each used once (`RP1301`); `threading` and `surrogate` are for a DLL, `args` for a program
(`RP1316`).

In an MSIX a `[com]` becomes a class of the package's COM catalog (`com:ComServer`: an `ExeServer` for
a program, a `SurrogateServer` for a DLL) and its prog-id a `com:ProgId`; `app-id` and `typelib` stay
with the MSI, which a warning says (`RP1612`).

### Explorer handlers: `[handler.ID]`

```toml
[handler.Thumbs]
kind = "thumbnail"                # the picture Explorer shows for the file
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D41}"   # a [com.*] class of a DLL of the package
types = [".exdoc"]

[handler.Preview]
kind = "preview"                  # what the preview pane shows
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D43}"
types = [".exdoc"]
description = "Example document preview"
msi-only = true

[handler.Props]
kind = "property"                 # the file's properties (title, author, ...) for Explorer and search
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D42}"
types = [".exdoc"]
msi-only = true
```

The handler is a COM class in a DLL of the package, written as a `[com.*]` (with `threading`), and
`[handler]` tells Windows which file types it serves. In an MSI: a thumbnail or preview handler is
the `ShellEx` value of each type under `HKEY_CLASSES_ROOT` (`{E357FCCD-...}` thumbnail,
`{8895B1C6-...}` preview); a preview handler is also on the `PreviewHandlers` list, and its class
gets the `AppID` of Windows' preview host (`prevhost.exe`, 64-bit or 32-bit by the package's
architecture), so leave `app-id` and `surrogate` out of its `[com]`; a property handler is under
`PropertySystem\PropertyHandlers` in `HKLM`, which Windows reads per machine only, so the package
needs `scope = "machine"`. A type has one handler of each kind (`RP1301`).

In an MSIX thumbnail and preview handlers go into a file type association of the package
(`desktop2:ThumbnailHandler`, `desktop2:DesktopPreviewHandler`, with their classes in the package's
COM catalog), one association per set of types (or the `[assoc]` that opens them); Explorer uses
them while the package is installed. A package serves its classes from a surrogate (`dllhost`),
so give a preview handler's class `threading = "sta"`: with another model its window may get a
thread without a message loop and stay blank (a warning, `RP1612`). A packaged property handler gave Explorer no values on
Windows 11 where the same class installed by an MSI did, so an MSIX refuses that kind (`RP1612`)
and `msi-only = true` keeps it for the MSI.

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

```toml
[msix-extension.Web]
kind = "firewall"                 # a Windows Firewall rule while the package is installed
direction = "in"
protocol = "tcp"
ports = "8080"
profile = "private"               # default "all"; file = "file:ID" for another program
```

An MSI build leaves these out. Fonts (`[font.*]`) in an MSIX are shared with other applications
(`uap4:SharedFonts`) from the package's `Fonts` folder; `title` is not used there.

```toml
[msix-extension.Server]
kind = "com-server"               # a COM class the program (or a DLL) serves
file = "file:MainExe"
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C41}"
args = "-Embedding"

[msix-extension.Toast]
kind = "toast"                    # a click on the app's notification starts it as this class
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C43}"

[msix-extension.Menu]
kind = "context-menu"             # an Explorer context menu verb, handled by a DLL (IExplorerCommand)
file = "file:MenuDll"
class = "{7A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C44}"
types = [".txt", "*"]             # file types; "*" = every file
```

These become the application's `com:ComServer` (an `ExeServer` for a program, a `SurrogateServer`
for a DLL), `desktop:ToastNotificationActivation` with the program registered for its class and
`args`, and `desktop4:FileExplorerContextMenus` with the DLL as the class. Windows lists the
classes in its packaged COM catalog while the package is installed. The context menu shows only if
the DLL implements the verb (IExplorerCommand); rubrapack registers it, it does not write it.

A `[service.*]` goes into an MSIX as a packaged service (`desktop6:Service`, in the first
application, with the capability `packagedServices`, and `localSystemServices` for
`account = "LocalSystem"`): its name, start and account as in the MSI, `args` as plain text.
`display-name` and `description` are not used there, and the service starts by `start`, not by
`start-on-install`. It needs `min-version = "10.0.19041.0"` or later (`RP1614`). Removing the
package stops and removes the service; firewall rules go with the package too.

### Moving files, ODBC, published components

These tables have no MSIX counterpart; in an MSIX build they are left out.

```toml
[move.OldLogs]
from = "LegacyDir"        # a dir ID, or property:NAME (a folder a [search] found)
name = "*.log"            # the files, wildcards allowed
to = "LogsDir"            # a dir ID of this package
# new-name = "one.log"    # one file renamed (no wildcards); copy = true keeps the source

[odbc-driver.Mydb]
file = "file:DriverDll"
description = "My Database Driver"
attributes = ["APILevel=2", "ConnectFunctions=YYN"]

[odbc-source.Sample]
driver = "Mydb"           # or driver-name = "..." with = "file:ID" for a driver installed otherwise
name = "My Sample Data"
attributes = ["Description=a sample data source"]

[publish.Plugin]
file = "file:PluginDll"   # the DLL's component is published
category = "{8D1E2F30-...}"
qualifier = "image/png"
```

- `[move.*]` moves (or, with `copy = true`, copies) files at installation. With `with = "file:ID"`
  the moved files belong to that file's component; otherwise the move gets a component of its own in
  the destination folder (name the `feature` when the package has more than one).
- `[odbc-driver.*]` registers an ODBC driver, `[odbc-source.*]` a data source of it. `attributes`
  are the ODBC key-value pairs. A per-machine package registers data sources for the machine, a
  per-user one for the user (`registration` overrides).
- `[publish.*]` publishes a component so other programs find it by category and qualifier with
  `MsiProvideQualifiedComponent`.
- `isolate-for` on a `[file.*]` DLL puts a private copy of it beside the named program, so that
  program loads its own version (Windows' side-by-side isolation).

### Removing and copying: `[remove.ID]`, `[copy.ID]`

`[remove.ID]` deletes files matching `name` in `dir` - for example `*.log` an older version left, at
`on = "install"`, or files the program writes at run time, at `on = "uninstall"`. Without `name` it
removes the folder itself when it is empty. A failed installation puts removed files back.

An upgrade removes the old version too, and with it runs the old version's `on = "uninstall"`
rows. `upgrade = false` keeps them out of that: the files go only when the product (or its
feature) is really removed - for files the new version still wants, or that programs still running
the old version may read again. It is done by rubrapack's helper DLL instead of the RemoveFile
table: the files are moved to a backup folder (the volume's `Config.Msi`, or the temporary folder
for a per-user package), put back if the removal fails, and deleted when it succeeds; a file a
program holds is deleted at the next restart. It takes effect from the first version built with
it, since an upgrade runs the old version's own rows.
`[copy.ID]` installs a second copy of a file of this package into another folder; it goes and comes
with its source. Folders that existed before the installation are never removed.

### Wiping at removal: `recursive`, `on = "uninstall"`

A removal takes away what the package installed. What the program made while it was used - a
settings file beside it, a folder under AppData, a key under `HKCU\Software` - stays, and with a
file left in it the install folder stays too. To have it all go, name the places:

```toml
[remove.Everything]
dir = "INSTALLDIR"
on = "uninstall"
recursive = true            # the folder with every file and folder in it

[dir.Settings]
path = "$(APPDATA)/Example/Tip"

[remove.Settings]
dir = "Settings"
on = "uninstall"
recursive = true

[registry.Settings]
root = "HKCU"
key = 'Software\Example\Tip'
remove = true
on = "uninstall"            # the key with its values and every key below it
```

- With `ui = "installdir"` or `"features"`, the folder page asks before it takes a folder that
  already holds something, when that folder is one the package wipes: the page says that a removal
  deletes everything in it, and offers **Choose another** (the default) or **Use this folder**. A
  silent installation is not asked.
- A folder of the user's profile (AppData, the Start menu, the desktop) named here is not made at
  installation: it appears when the program makes it. Other folders are made at installation,
  empty, as a plain `[remove]` makes its folder. (0.41.0 made the profile ones too.)
- It happens only when the product (or the feature the row belongs to) is really removed -
  Installed apps, the maintenance page's Remove, `msiexec /x`. **An upgrade never wipes**: the new
  version finds the settings as they were. A removal that fails and rolls back has touched nothing,
  for the deleting is done last, once the removal has succeeded - which also means that nothing
  brings it back afterwards. Where rollback is turned off (the `DisableRollback` policy, or an
  installation run that way) Windows Installer skips that last step; the wiping then happens at the
  end of the removal itself instead.
- **It deletes whatever is there**, not only what the program wrote. Name a folder that is the
  product's alone. If the user may choose the install folder and chooses one that holds other
  things, those go too.
- Refused when it is built (`RP1316`): a folder below `SystemRoot` or `Temp`; a key that is not below
  `Software`, or that is a whole branch of `Software\Microsoft`, `Classes`, `Policies` or
  `Wow6432Node`, or anything below `Software\Microsoft\Windows` and `Windows NT`; `HKCR`; `recursive` with `name`, `upgrade`, or an `on` other than `uninstall`; a
  merge module.
- Kept when it is removed, with a line in the log (`rubrapack: purge:`): a drive's root; a folder
  that is, or holds, one of Windows' own (Program Files, ProgramData, the user's profile, AppData,
  Desktop, Documents, ...); anything inside the Windows folder; a folder in which another installed
  product has a component (the file or folder Windows Installer knows that component by - not
  every file of it).
- Links are not followed: a junction or symbolic link inside the folder is deleted as a link, and
  what it points at is left alone. A file a program still holds stays, and its folder with it.
- **AppData and `HKCU` are those of the user who removes the product**, unless the place says
  `users = "all"` (below). For this purpose a per-machine package may name `HKCU` here, which it
  otherwise may not. The user's profile and `HKCU` are deleted with the
  user's own rights; the rest, in a per-machine package (`scope = "machine"`), by SYSTEM. A `dual`
  package installed for all users (`ALLUSERS=1`) does the same; installed for one user, it deletes
  everything with that user's rights.
- The folder's `[dir.ID]` must be a folder of this package, so it is created (empty) at
  installation if it was not there.
- An MSIX needs none of this: Windows removes the package's folder and the package's own registry
  and AppData with it.

#### Every user's: `users = "all"`

A per-machine product used by several people leaves each one's settings behind. `users = "all"` on a
folder of AppData or LocalAppData that is wiped, or on an `HKCU` key that is, has the removal wipe
the same folder or key of every other user of the computer too - **when the person removing it
says so**:

```toml
[remove.Settings]
dir = "Settings"            # $(APPDATA)/Example/Tip
on = "uninstall"
recursive = true
users = "all"

[registry.Settings]
root = "HKCU"
key = 'Software\Example\Tip'
remove = true
on = "uninstall"
users = "all"
```

- Before the removal starts, the users who may have them are listed, and asked about: **Yes**
  deletes them for every user, **No** (the default button) deletes only the remover's, **Cancel**
  stops. A user is listed when the folder is there in that user's profile, or the key is in that
  user's registry - or that registry is not loaded, so it cannot be looked at. Nobody listed: no
  question. Installed apps' Uninstall (`/qb`) is asked too.
- Without a window (`/qn`) nobody is asked and only the remover's go, unless the command line says
  `RPPURGEUSERS=all` (`RPPURGEUSERS=mine` the same as nothing). With `preflight = false` the command
  line is the only way.
- The removal of a per-machine product already runs with administrator rights, so there is no
  second request: SYSTEM does it, last, as the rest of the wiping. A logged-off user's registry is
  loaded for the while and unloaded after.
- Skipped, with a line in the log: a user who has this product installed for themselves; a user
  whose AppData or LocalAppData is set to a place outside that user's profile (each user may set it,
  and SYSTEM deletes there); a registry that cannot be loaded. Links are not followed, as above; a
  file a logged-on user's program holds stays.
- Not reached: the server's copy of a roaming profile or of a redirected folder - it comes back at
  that user's next sign-in; users who have never signed in on this computer.
- Refused when it is built (`RP1316`): anywhere else - a folder outside AppData and LocalAppData, a
  key outside `HKCU`, without `recursive = true` or `on = "uninstall"` - and in a `user` or `dual`
  package, where each user's installation is that user's own product.

### Environment variables: `[env.ID]`

A system (machine-wide) variable. `mode = "set"` (default) replaces it, `append`/`prepend` add
`;value` to the end or `value;` to the front of what is there. Uninstall undoes exactly that: a set
variable is deleted, an appended part is taken out and the rest kept. `keep = true` leaves it.

### INI files: `[ini.ID]`

`mode = "set"` (default) writes `key=value` in `[section]` of the file, `add` appends the value to a
comma-separated list (`a` becomes `a,b`), `remove` deletes the key during installation. Uninstall
takes out what `set` and `add` wrote. `remove` runs before files are installed, so it is for INI
files an older version left behind.

### Several packages in one setup: `[chain]`

A source with `[chain]` builds a setup program instead of a package: `rubrapack build suite.toml -o
setup.exe`. The setup program holds the `[chain-package.*]` packages, in the order the source lists
them, each with its SHA-256; it checks them all, then installs them one after the other with
Windows Installer. A package already installed (its ProductCode) is skipped. Started again when
every package is installed, the setup program asks what to do - Repair, Remove or Cancel (nothing is
asked at `/quiet` or `/passive`). When a `vital` package
(the default) fails, the chain stops and returns that package's error; the package rolls itself
back, the ones before it stay. `properties` are passed to that package as on msiexec's command line.

- `setup.exe` shows Windows Installer's progress windows and a last message; `/passive` only the
  progress; `/quiet` nothing. `/uninstall` removes the packages, last first; `/repair` repairs the
  installed ones. `/log <file>` writes a
  verbose log. `/lang <code>` (`/lang ar`) shows the setup program's texts, and the packages' dialogs
  through `RPLANGUAGE`, in that language instead of the display language's. Exit code: 0, 3010 (a restart is needed), the failing package's error, 1620 for a
  damaged setup program, 1602 when the user refuses elevation.
- With `elevate = true` (the default) the setup program asks for administrator rights once, for all
  packages; per-user chains set `elevate = false`.
- The setup program's own texts are English, and Korean for a Korean user; `[chain] languages =
  ["ja"]` adds a language that has a pack ([Language packs](#language-packs), its `Setup.*` texts).
  It shows the language of the user's display language.
- A chain holds only `[chain]`, `[chain-package.ID]` and `[define]`; each package is built from its
  own source first. `--key` signs the setup program like any program; `inspect setup.exe` lists its
  packages and `extract setup.exe -d dir` takes them out.

### Merge modules: `[merge.ID]`

A merge module (`.msm`) is a piece of an installer that another vendor ships for its runtime or
library. `[merge.ID]` copies its tables into the package: the module's files, components, registry
values and its own actions. Its root folder becomes `dir`, its components join `feature` (default:
the dir's feature, or `Main`), and its cabinet is embedded as a second cabinet of its own. A
standard action the module's tables need and the package lacks (`WriteRegistryValues`, for
example) is added at its usual place. rubrapack does not merge a module with non-ASCII text in a
code page other than UTF-8 (`RP1517`). In an MSIX a module gives its files (below `dir`, or in the virtual file system when
they go to a standard folder such as the system folder) and its registry values (in the package's
hives); a module that needs more - custom actions, conditions on components, environment or INI
entries, services, fonts, COM registration - or a value filled in at install time is refused
there (`RP1517`; `msi-only = true` keeps it for the MSI). The merged module's
`ModuleSignature` and `ModuleComponents` rows stay in the package, as Microsoft's merge tool leaves
them; the tables the module lists in `ModuleIgnoreTable` do not go in.

A *configurable* module (one with a `ModuleConfiguration` table) asks for values - a text, a
number, a set of bits, or a key such as one of its folders - which `config` gives, one
`"Name=value"` string per item:

```toml
[merge.Runtime]
source = "runtime.msm"
dir = "INSTALLDIR"
config = ["ServerName=example.com", "Port=8080", "DataDir=DataFolder.6E0A1C52_8F3B_4B7D_9A21_3C4D5E6F7A99"]
```

An item not given takes the module's default, as when a merge tool declines to answer. The values go
where the module's `ModuleSubstitution` table says, the way Microsoft's merge tool puts them there:
a bitfield item sets only the bits of its mask, a key item may name one part of a key with
`[=Item;2]`, the null GUID becomes the feature's name, and a default row that every answered
`KeyNoOrphan` item replaced is left out. A key value is written as the module's own key (with its
GUID) and in the module's escaped form (`\;` for a semicolon). An unknown item, a number item
without a number and an empty value where the module needs one are refused (`RP1517`), as is
`config` for a module that takes none. The names, kinds and defaults of the items are in the
module's `ModuleConfiguration` table: `rubrapack inspect runtime.msm ModuleConfiguration`.

### Writing a merge module: `[module]`

A source with `[module]` in place of `[package]` builds a merge module for others to merge:
`rubrapack build runtime.toml -o runtime.msm`.

```toml
format = 1

[module]
name = "ExampleRuntime"
manufacturer = "Example"
version = "2.1.0"
arch = "x64"
id = "{6E0A1C52-8F3B-4B7D-9A21-3C4D5E6F7A99}"     # the module's own GUID, kept in every version

[dir.RuntimeDir]
path = "$(TARGETDIR)/Example Runtime"             # TARGETDIR: where the package puts the module

[files.Runtime]
dir = "RuntimeDir"
glob = "runtime/*"

[registry.Home]
root = "HKLM"
key = 'SOFTWARE\Example\Runtime'
name = "Home"
value = '$(RuntimeDir)'                           # the folder the package chose for it
```

- A module holds `[dir.*]`, `[file.*]`, `[files.*]`, `[folder.*]`, `[registry.*]`, `[env.*]`,
  `[ini.*]`, `[remove.*]`, `[copy.*]` and `[define]`. Features, dialogs, actions, services and the
  rest belong to the package that merges it (`RP1201`); a qword registry value and `guard` need
  rubrapack's helper DLL, which a module does not carry (`RP1316`).
- Its paths start at `$(TARGETDIR)`, the module's root, which the merging package redirects
  (`[merge.ID] dir`), or at a Windows folder (`$(System)`, ...).
- Every key the module defines ends in `.<GUID>` (the `id`, `-` as `_`) so it cannot meet the
  package's keys - `[dir.RuntimeDir]` becomes `RuntimeDir.6E0A1C52_8F3B_...` - and so do the
  references to them, `$(RuntimeDir)` in a value included. An ID in a module therefore has at
  most 35 characters (`RP1301`). Component GUIDs derive from the `id`, as a package's from its
  upgrade code.
- The module has `ModuleSignature` (`name.<GUID>`, language, version), `ModuleComponents`,
  `ModuleInstallExecuteSequence` with the standard actions its tables need, empty
  `FeatureComponents` and `InstallExecuteSequence` tables (ICEM04), and its files in
  `MergeModule.CABinet` under their keys.
- Checked against Microsoft's tools: its module ICEs (`mergemod.cub`) report nothing, its merge tool
  (`mergemod.dll`) merges the module without an error into the same tables as `[merge.ID]`, and the
  merged package installs.

### Searching and requiring: `[search.ID]`, `[require.ID]`

A search runs before anything else and puts what it found into a public property (empty when
nothing is found): a registry value's data, the full path of a file (`path` = a known folder and a
relative path, like `$(System)` or `$(ProgramFiles)/Example`; `min-version` for program files), a folder,
or the key file of another product's component. A requirement stops a first installation with its
message when its `condition` is false (repair and removal are never blocked); conditions use Windows Installer's syntax
(`VersionNT >= 603`, `FOUND_TOOL`, `NOT OLDSETTING`) and may test search results.

```toml
[search.Tool]
property = "FOUND_TOOL"
kind = "file"
path = "$(System)"
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
and the machine folders (`SystemRoot`, `System`, `Fonts`, `ProgramData`) need `scope = "machine"`.

A file, `[folder]`, `[ini]` or `[remove]` in a folder of the user's profile (`AppData`,
`LocalAppData`, `StartMenu`, `Programs`, `Desktop`, `Startup` and below them, in any scope) gets a
component keyed by an `HKCU` value, as shortcuts are: `Software\<manufacturer>\<name>\Components`,
one value per component, written and removed with it (Windows' rule ICE38). Each of those profile
folders, up to the known folder, is removed at removal when it is empty (ICE64), the shared
`%LOCALAPPDATA%\Programs` too when nothing else is in it. rubrapack 0.46.0 and earlier keyed these
components by their files, under other GUIDs: from a package built by one of those, the upgrade has
to be a major upgrade (rubrapack builds one: the older version is removed first), `lint --previous`
lists the old components as gone (`RP2306`), and a patch is refused. A `component-guid` keeps its
file as the key path, with a warning (`RP1320`). A `dual` package with files in `AppData` or
`LocalAppData` gets a warning too: installed for all users, they reach the installing user's profile
only.

### Cabinets

Files are compressed into one cabinet embedded in the package, with MSZIP (deflate) or LZX
(`compress = "lzx"`: typically 5 to 20% smaller, about 4 MB a second on one processor where MSZIP
uses them all). `cab-max-size = N` starts a new
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

Uninstall in Installed apps runs `msiexec /qb /x {ProductCode}`: Windows Installer's own reduced
window, not the package's dialogs, and when a program with a window holds one of the files, its own
files-in-use box - in English, with Cancel as the default (Ignore goes on). `no-remove = true` turns
Uninstall off for the product; Modify, still there, runs the package's dialogs (`msiexec /i`), whose
Remove leads to the package's own files-in-use dialog with Continue as the default. Worth it for an
input method or a shell extension, which nearly every program holds; it cannot go with `no-modify`,
and the files-in-use dialog is a dialog set's (`ui = "none"` has Windows Installer's own).

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
| `none` | Windows Installer's progress bar | a small window: Repair, Remove or Cancel |
| `basic` | progress, then finished (or error) | change (with features to choose), repair or remove |
| `minimal` | welcome, the license if there is one, progress, finished | change (with features to choose), repair or remove |
| `installdir` | welcome, license, install folder (with a folder browser), ready, progress, finished | change (with features to choose), repair or remove |
| `features` | as `installdir`, plus a feature tree and the disk space it needs | change, repair or remove |

"Already installed" is the same package opened again (a double click, `msiexec /i`, or Modify in
Installed apps) with the product - this version - installed: Repair puts back missing or damaged
files and settings, Remove removes the product. **Remove asks once more** before anything is
removed: the dialog sets show a page that says what goes (the program's settings and data too, when
the package wipes them - see [Wiping at removal](#wiping-at-removal-recursive-on--uninstall)), on
which Back is the default button; the window of `none` asks Yes or No, with No as the default.
**Change** is there when the package has a feature the user may turn on or off (one that is not
`hidden` and not `required`), in every set: it leads to the feature tree, and Install there adds
the features turned on and takes out those turned off. The window of `none` has no Change (it
cannot show a tree): there the command line does it (`ADDLOCAL=`, `REMOVE=`). Nothing is asked when the command line has already
chosen (`REINSTALL`, `REMOVE`, `ADDLOCAL`, `msiexec /f`, `/x`) or at `/qn`, `/qb` and `/qr`, where
the run repairs nothing and changes nothing. A newer version's package is an upgrade, not this. The
window of `none` is in English, or Korean for a Korean user; its texts are `MaintTitle`, `MaintText`,
`Repair`, `Remove`, `Cancel` and `RemoveAsk` (`RemoveAskAll` when the package wipes) below.

Every set also has the cancel question, the error dialog, the files-in-use list and the
out-of-disk-space warning. The files-in-use list comes when a program with a window holds a file
being replaced or removed - for an input method or a shell extension, nearly every program. Its
default button is Continue: the files are replaced at once, programs already open keep the old
files until they are reopened, and with `reboot = "suppress"` (the default) no restart is asked for
(`FilesInUseText`; with `reboot = "allow"`, `FilesInUseTextRestart`, which says Windows may ask to
restart). The dialogs only collect values that all have defaults, so `/qn` still
installs without a window.

```toml
format = 1

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
`FatalTitle`, `FatalText`, `CancelText`, `FilesInUseTitle`, `FilesInUseText`,
`FilesInUseTextRestart`, `Continue`, `OutOfDiskTitle`,
`OutOfDiskText`, `MaintTitle`, `MaintText`, `Repair`, `RepairText`, `Remove`, `RemoveText`,
`Change`, `ChangeText`, `RemoveConfirmTitle`, `RemoveConfirmText`, `RemoveConfirmTextAll`,
`RemoveAsk`, `RemoveAskAll` (the second question before a removal; another language that does not
give these five gets English), `DetailsFull` (the details box's check box),
`LanguageTitle`, `LanguageText`, `LangGroup1` to `LangGroup9` (the regions of a grouped language
page; a language without them gets English), `DirGuardText` (the guard's message below), `PreflightAsk`,
`PreflightSilent`, `PreflightFolder`, `PreflightCache`, `PreflightExtra`, `PreflightExtraSilent`, `PreflightOtherStore`, `PreflightOtherStoreRefuse`, `PreflightOtherMsi`, `PreflightOtherMsiRefuse` (the preflight's messages), `ReportInstall`, `ReportUpgrade`, `ReportRemove`, `ReportRepair` (the report after a failed rollback), and for a removal -
the maintenance page's Remove, or `REMOVE=ALL` - `RemovalProgressTitle`, `RemovalExitTitle`,
`RemovalExitText`, `RemovalUserExitTitle`, `RemovalUserExitText`, `RemovalFatalTitle`,
`RemovalFatalText` (another language that does not give these uses its install texts). In button texts `&` marks the access key (`&Next` is Alt+N).

`embedded = "ui.dll"` carries the author's own UI handler (a DLL exporting `InitializeEmbeddedUI`,
`EmbeddedUIHandler` and `ShutdownEmbeddedUI`) in the package; Windows runs it for the interactive
installation in place of rubrapack's dialogs, so it may be given with `ui = "none"`. It is bypassed
for a silent installation (`/qn`) and when another program drives the UI from outside - as the
chained `setup.exe` does - so a chained package should not rely on its window. An embedded UI is for
an MSI; an MSIX has no installer UI of its own and refuses it (`RP1605`).

### Several languages

The dialogs are in English. `[ui] languages` adds other languages to the same package; then the
first page asks for the language, and every page after it - welcome, license, folder, your own
pages, ready, progress, finished, and the cancel, error, files-in-use, disk-space and maintenance
pages - speaks the one chosen. The choice made for the user in advance is the first added language
whose LANGIDs hold the user's regional format (`UserLanguageID`), then the system locale
(`SystemLanguageID`), else English. `RPLANGUAGE=ko` on the command line makes that choice instead (the language page still shows, with it selected); a silent
installation (`/qn`) shows nothing and needs no choice. With English alone there is no language
page.

```toml
[ui]
languages = ["ko"]                # English is always there, and the default
# languages = "all"               # or: every built-in language, region by region
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

- The built-in texts exist in English and in the languages whose packs come with rubrapack:
  Korean (`ko`), Japanese (`ja`), Chinese, Simplified (`zh`) and Traditional (`zh_TW`), Spanish
  (`es`), French (`fr`), German (`de`), Portuguese of Brazil (`pt_BR`) and of Portugal (`pt_PT`),
  Russian (`ru`), Italian (`it`), Dutch (`nl`), Swedish (`sv`), Danish (`da`), Norwegian Bokmål
  (`nb`), Finnish (`fi`), Polish (`pl`), Czech (`cs`), Ukrainian (`uk`), Hungarian (`hu`), Romanian
  (`ro`), Turkish (`tr`), Greek (`el`), Vietnamese (`vi`), Thai (`th`), Indonesian (`id`), Malay
  (`ms`), Bulgarian (`bg`), Croatian (`hr`), Slovak (`sk`), Slovenian (`sl`), Serbian in Latin script
  (`sr`), Estonian (`et`), Latvian (`lv`), Lithuanian (`lt`), Arabic (`ar`), Hebrew (`he`) and Hindi
  (`hi`) ([Language packs](#language-packs)). Another language (`ko_KP`, `fa`, ...) brings a
  pack of its own, or gives every built-in text as `text-xx` (lint names the ones missing), and
  may give `name-xx` (its name
  on the language page), `font-xx` (the face of its dialogs) and `langid-xx` (the LANGIDs that
  choose it: one number or an array). Common languages have these built in: `ja`, `zh`, `de`,
  `fr`, `es`, `it`, `pt`, `nl`, `pl`, `ru`, `uk`, `tr`, `vi`, `th`.
- The typeface of a language's dialogs is its `font-xx` when given (`font-en` for English);
  otherwise the language's usual face - Malgun Gothic (`맑은 고딕`) for `ko`, Yu Gothic UI for `ja`,
  Microsoft YaHei UI for `zh`, Leelawadee UI for `th` - and Segoe UI for every other language.
  That language's license text (`license-xx` as `.txt` or `.md`) is shown in the same face; its
  RTF needs the face's English name, which rubrapack knows for the faces that come with Korean,
  Japanese and Chinese Windows and for the Nanum faces (`나눔고딕` is NanumGothic). For another
  local name the license text keeps the language's usual face, with a warning (`RP1319`): write
  the English name instead. A face is named as Windows lists it, 1 to 31 characters (`RP1308`);
  one the user's computer lacks is replaced by Windows with a similar one.
- A text that uses a property other than `[ProductName]`, `[Manufacturer]` and `[ProductVersion]`
  is formatted when the language is chosen, and must fit 255 characters.
- The progress page's status line ("Copying new files", "Updating component registration") is the
  package's, in English and every language with a pack, and follows the chosen language. A package
  with a language that has no pack among its `languages` keeps Windows Installer's own words
  there, which follow Windows; so does a package without dialogs.
- The language page lists every language by its own name. Its heading names the page in each of
  them; the line under it does too for up to three languages, and with four or more it is in the
  language chosen in advance (the user's), as the two lines would not hold them all.
- Windows Installer's own texts - the feature tree's menu, sizes, the time left, and its error
  messages - are not the package's: the first stay English, the error messages follow Windows.
- `[package] language` sets only the package's language (`ProductLanguage`, the summary). Since
  0.2 it no longer makes the dialogs Korean: add `languages = ["ko"]` (lint warns, `RP1317`).

### Language packs

A language is one file. English is built into rubrapack; forty-eight languages come with it as
`resources/lang/<code>.po`: `ko`, `ja`, `zh` (Simplified Chinese), `zh_TW` (Traditional Chinese, for
Taiwan, Hong Kong and Macao), `es`, `fr`, `de`, `pt_BR` (Brazil), `pt_PT` (Portugal), `ru`, `it`,
`nl`, `sv`, `da`, `nb` (Norwegian Bokmål), `fi`, `pl`, `cs`, `uk`, `hu`, `ro`, `tr`, `el`, `vi`,
`th`, `id`, `ms`, `bg`, `hr`, `sk`, `sl`, `sr` (Serbian in Latin script), `et`, `lv`, `lt`, `ar`
(Arabic), `he` (Hebrew), `hi` (Hindi), `fa` (Persian), `ur` (Urdu), `ca` (Catalan), `eu` (Basque),
`gl` (Galician), `bn` (Bengali), `ta` (Tamil), `kk` (Kazakh), `uz` (Uzbek in Latin script) and `az`
(Azerbaijani in Latin script). Any other language is supported by adding its file - `<code>.po`, in the
PO format translators' tools read - without changing rubrapack or the source's texts. A code is 2
or 3 lower-case letters, and for a region's own variant `_` and the region's two capitals, as
gettext names its files. One package holds English and up to 63 more languages - all 48 built-in
packs fit. With up to seven, each text of each language is an action of its own; with eight or
more, the texts are rows of a table (`RpText`) that rubrapack's helper DLL puts in place when the
language is chosen. Up to twelve languages with English, the language page lists them in one
column; with more it groups them by region: the regions on the left, the chosen region's languages
on the right. A pack names its region with `X-Rubrapack-Group` (1 East Asia, 2 Southeast Asia,
3 South Asia, 4 Central Asia and the Caucasus, 5 Middle East, 6 Western and Southern Europe - with
English -, 7 Northern Europe and the Baltics, 8 Central and Eastern Europe); a pack that names none
is under "Other". A region holds at most twelve languages (`RP1308`). `languages = "all"` takes
every built-in pack, region by region in the order above and, within a region, the widely used first
(`ko ja zh zh_TW`, then `en de fr es it ...`); a pack beside the source still replaces a built-in one.
"all" stands alone: in an array it is an error (`RP1316`).

```po
msgid ""
msgstr ""
"Language: ja\n"
"X-Rubrapack-Name: 日本語\n"        # the language's name on the language page
"X-Rubrapack-LangIDs: 1041\n"      # the Windows LANGIDs it is chosen for
"X-Rubrapack-Font: Yu Gothic UI\n"
"X-Rubrapack-Direction: ltr\n"     # rtl for a language read right to left (ar, he, fa, ur)
"X-Rubrapack-Group: 1\n"           # its region on a grouped language page

# The command line: msgid is the English text as rubrapack writes it.
msgid "cannot read '%s' (%s)"
msgstr "'{1}' を読めません ({2})"

# The installers rubrapack builds: msgctxt "installer", msgid is the text's ID.
msgctxt "installer"
msgid "WelcomeTitle"
msgstr "[ProductName] セットアップ"
```

- **Where a pack is looked for**: the folder `RUBRAPACK_LANG_DIR` names; then `lang/` beside the
  source being built; then `lang/` beside the rubrapack program; then the packs built in. A file
  with a built-in language's code comes first, so a corrected or reworded `de.po` beside the
  source replaces the built-in German.
- **The command line**: `--lang ja` (or `RUBRAPACK_LANG=ja`) uses the pack. A translation names the
  English text's values by number, `{1}` for the first `%s` or `%zu`, so it can put them in the
  order its language wants; `{-2}` says the second has no place (a plural "s"); `{{` is a brace. It
  is not a format string: a pack can leave a value out but cannot make rubrapack read one that is
  not there. A text the pack lacks comes out in English.
- **Installers**: `[ui] languages = ["ja"]` takes the dialogs' texts (the IDs listed under
  [Several languages](#several-languages)), the status line (`Action.InstallFiles`, ...), the
  details box (`Details.FileCopy`, ...) and the helper's messages from the pack, and the language's
  name, LANGIDs and font from its header - `[ui] name-ja`, `langid-ja`, `font-ja` and
  `[ui-text.ID] text-ja` in the source still win. A text neither gives is an error (`RP1202` names
  the first), except the few that fall back to English. A chain's setup program takes `Setup.*`
  for the languages `[chain] languages` names.
- **Right to left**: a pack with `X-Rubrapack-Direction: rtl` makes its pages mirrored. A package
  with such a language among `[ui] languages` holds every dialog twice - as written, and mirrored
  (`<name>_RTL`: controls placed from the right, texts read right to left, path fields still left to
  right) - and shows the mirrored set while that language is chosen; a package without one is as
  before. The helper's questions and details box and the chain's setup program read right to left
  when the language they show does. Limits: the files-in-use dialog stays left to right (Windows
  Installer finds it by its name), and the command line prints such text in its stored order, as a
  console has no right-to-left layout.
- **Character sets**: the details box draws each language in its own character set, taken from the
  language's LANGID, whatever language Windows is in (a Korean page shows a path's backslash as
  `₩`, as Korean Windows does). In any other language's box a Korean, Chinese, Japanese, Indic or
  Thai file name is drawn in its own script's face - Malgun Gothic, Yu Gothic UI, Microsoft YaHei UI
  or JhengHei UI (a name in Chinese characters alone: by Windows' display language), Nirmala UI,
  Leelawadee UI - while the rest of the line keeps the box's face and backslashes, right-to-left
  boxes (`ar`, `he`, `fa`, `ur`) included.
- **Starting one**: copy one of `resources/lang/*.po`, change the header, translate the `msgstr`
  lines.
  `RUBRAPACK_LANG_MISSING=<file>` makes a run append every text the chosen pack lacks to that file.

### Icon, the finished page, and the scope page

- `[arp] icon = "app.ico"` is the product's icon in Installed apps; `[shortcut] icon` gives a
  shortcut its own `.ico` (without it, the program's own icon). Both are read when the package is
  built and stored in it.
- `[ui] launch = "file:App"` puts "Launch [ProductName]" on the finished page (ticked unless
  `launch-checked = false`; `launch-args` are its arguments). Finish starts the program after a
  first installation or an upgrade, as the user who ran the setup, not with the installer's
  rights; not after a repair or removal, and never at `/qn`.
- The finished, cancelled and failed pages have **Save log...**, which saves a copy of the log of
  this run where the user chooses (rubrapack's helper DLL, `RpSaveLog`). The package sets
  `MsiLogging` so that Windows Installer keeps a log in the user's temporary folder even without
  `/l`; the button hides when there is no log. `[ui] save-log = false` leaves out the button (and,
  with `details = false` too, the logging).
- **The progress page shows what is being done**, not only the bar: a scrolling box under it fills,
  line by line, while the installation, removal or repair runs - each step Windows Installer starts
  ("Copying new files") and under it each thing done: a file copied or removed with its folder, a
  registry value with its key, a shortcut, a folder, a service, an environment variable, an INI
  value, one of the source's own `[action.*]`, and what a rollback takes back. The lines are in
  the dialog's language (English and Korean). The check box below it, **Show the full log**
  (`DetailsFull`), turns the box to the log file's own lines, as Save log saves them - everything
  Windows Installer writes, in English. `RPDETAILSFULL=1` on the command line starts that way.
  The box is filled by rubrapack's helper DLL (`RpDetails`), which reads the log of the run as it
  grows; for that the package's `MsiLogging` has the flush flag (`voicewarmup!`). It never holds
  the installation up: without a log (logging turned off by policy) the box stays empty.
  `[ui] details = false` leaves the box and the check box out. `/qb` and `/qn` have no box.
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
  type the property's value. `radio`, `combo` and `listbox` choose one of `values` (1 to 32 strings
  of at most 64 bytes), shown as `labels` (default: the values themselves), in the order written -
  `radio` as a set of buttons, `combo` as a drop-down, `listbox` as a list. `bitmap` shows a picture
  from a `.bmp` file named by `image` (Windows Installer shows BMP only, as the banner does),
  stretched to the control's rectangle; it takes no property and is not a tab stop.
- A `radio`, `combo` or `listbox` needs a `[property.*]` whose value is one of its values: the dialogs only
  collect values, and a silent installation (`/qn`) uses the defaults. Any property can also be
  set on the command line: `msiexec /i example.msi /qn APP_MODE=server`.
- The properties are public names of your own (upper case), one control each, and reach the
  elevated part of the installation, so `[registry.*]`, `[ini.*]`, `[env.*]`, conditions and the
  rest can use them as `[APP_MODE]`.
- Page IDs starting with `Rp` and the control IDs of the frame (`Banner`, `Title`, `Description`,
  `BannerLine`, `BottomLine`, `Back`, `Next`, `Cancel`) are reserved; a page holds up to 64
  controls.

### Billboards: `[billboard.ID]`

```toml
[billboard.Welcome]
image = "ads/welcome.bmp"
feature = "Main"
order = 1
```

A billboard is a picture shown on the progress page while its `feature`'s files install, one after
another in `order` (default: the order the billboards are written). Each names a `.bmp` in `image`
(Windows Installer shows BMP only). Billboards are for an MSI; an MSIX installs the whole package at
once, so a `[billboard.*]` is refused there (`RP1605`).

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

### Before it goes on: the preflight

Before an installation, an upgrade or a removal changes anything, the package looks:

- **Which programs use the product's files** - its own programs that are running, and other
  programs that have one of its DLLs loaded (for an input method or a shell extension, nearly every
  open program). Their number and names are shown, with the question whether to close them all.
  **Yes** asks their windows to close and, a few seconds later, ends what is left (unsaved work in
  them may be lost); **No** goes on without closing - the files are replaced anyway, and those
  programs keep the old ones until they are reopened; **Cancel** stops, with nothing changed.
- **Without a window to ask in** (`/qn`), the run stops with an error (1603; the log names the
  programs) - unless the command line says what to do: `RPCLOSE=yes` closes them all and goes on,
  `RPCLOSE=no` goes on without closing.
- For a first installation (not an upgrade, a repair or a removal): **the same program installed
  another way**, when the package names it with `[other-copy.*]` (see below).
- For an installation or an upgrade: **each package folder that exists** is a folder the installer
  may delete files in, and **the older version still has its cached package** (without it the
  upgrade could not remove that version). If not, the message says what is wrong and to remove the
  product first, and nothing is installed. A removal is never refused for these.
- For a removal: **what is in the product's folders but was not installed with it** - a file the
  user put there, a folder the program made, a log. Each of the package's own folders (its
  `[dir]`s, not Windows' own like Program Files) is looked at; a file the package installed, a
  shortcut, an INI file it writes, a name a `[remove]` of that folder matches and the package's
  other folders are its own, everything else is listed (the first 15, then how many more), with
  three answers: **Yes** deletes them too - the folders go with everything in them, at the end of
  a removal that succeeded, the way `[remove] recursive` wipes; **No** removes the product and
  keeps them (the folders holding them stay); **Cancel** stops, with nothing removed. A folder the
  package wipes anyway (`[remove] recursive = true`) is not looked at, and neither is anything when
  an upgrade removes the older version.
- **Without a window to ask in** (`/qn`) such a removal stops with 1603 before anything is
  removed (the log names each item), unless the command line says: `RPEXTRA=delete` or
  `RPEXTRA=keep`.

`close-programs = "always"` closes without asking, `"never"` never closes and never asks (also at
`/qn`); `"ask"` is the default. A removal is no different: with `"always"` (or `RPCLOSE=yes`) the
product's running programs are asked to close and, a few seconds later, ended, and then the product
is removed; a silent removal (`/qn`) of a package that says nothing stops with 1603 while one of
them is open, rather than end a program nobody was asked about. For an input method or a shell extension, `"never"` is the usual
choice: nearly every open program has its DLL loaded, so `"ask"` would put the question at every
upgrade and removal and stop every silent one, while the files are replaced safely without closing
anything (the old copies go through the cleanup task). `RPCLOSE=yes` or `RPCLOSE=no` on the command
line still wins over the package's setting. `extra-files = "keep"` keeps what was not installed
without asking (what Windows Installer itself does), `"delete"` deletes it without asking; `"ask"`
is the default, and `RPEXTRA` wins over it. `preflight = false` leaves the whole step out. The texts are
`PreflightAsk`, `PreflightSilent`, `PreflightFolder`, `PreflightCache`, `PreflightExtra` and
`PreflightExtraSilent` (`[ui-text.*]`; English and
the packs' languages are built in, another language without them gets English).

#### Another copy: `[other-copy.ID]`

```toml
[other-copy.Store]                # the Microsoft Store's package of the same program
msix = "Example.Example"          # its identity name and publisher, as Partner Center gives them
publisher = "CN=12345678-ABCD-..."
action = "warn"                   # the default; "refuse" stops instead

[other-copy.Old]                  # an older installer of another make
upgrade-code = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D40}"
action = "refuse"
message = "[1] cannot be installed beside [2]. Remove [2] first."
message-ko = "[2] 이(가) 있으면 [1]을(를) 설치할 수 없습니다. 먼저 [2]을(를) 제거하세요."
```

Before a first installation the preflight looks for each copy named: an app package (MSIX) of that
identity registered for the user who installs (Windows keeps app packages per user), or an installed
product of that upgrade code. When one is there:

- `action = "warn"`: a question - Yes installs anyway, No stops (1602). Without a window (`/qn`) the
  installation goes on and the log says what was found (`rubrapack: preflight: other copy ...`).
- `action = "refuse"`: the message, then the installation stops (1603), with or without a window.

The message names this product (`[1]`) and the copy found (`[2]`: the app package's full name, or
the other product's name). `message` and `message-xx` replace the default texts
`PreflightOtherStore`, `PreflightOtherStoreRefuse`, `PreflightOtherMsi` and
`PreflightOtherMsiRefuse`. It needs the preflight (`preflight = false` refuses it). An MSIX build
leaves it out: an app package runs nothing when it is added. The other way round - an MSIX that
should step aside for an installed MSI - is `hide-when` on its menu items, and the program's own
look at run time.

Windows Installer looks after two things itself: while another installation is running, a second
one ends at once with 1618; and after an installation that was cut off (a crash, a power cut), the
next one first undoes the unfinished one. After a removal that was cut off, that undo puts the
product back and the removal ends with 1605: run it once more.

### Cleaning up later: the cleanup task

A file a running program holds cannot always be removed at once: Windows Installer moves what it can
aside and queues the rest for deletion at the next restart, and an input method or a shell extension
may keep a computer from restarting for weeks. So a package that removes or replaces files (a
removal, an upgrade, a repair) registers a scheduled task, `rubrapack cleanup {ProductCode}` (per
user, followed by the user's SID). Two minutes later, then at every logon and every 15 minutes, it
deletes what this installation left for the restart - files in the package's folders and the
installer's backup copies of them in `Config.Msi` - as soon as nothing holds them, then the
package's folders that were left only because of them, and finally itself. With nothing to do it
goes at its first run; after 30 days it gives up and leaves the rest to the restart. It runs as
SYSTEM for a per-machine package and as the user, never elevated, for a per-user one, from a folder
only that account can write (`%ProgramData%\rubrapack\cleanup\{ProductCode}`, or the user's
`%LOCALAPPDATA%`; per machine, a `rubrapack` folder there that someone else made first is not used
and no task is registered), and it
never deletes anything else: only what this installation queued, only while the same file is still
at that path, and not a file an installed product now has there. `[package] cleanup = false`
leaves it out.

A per-user package removed without administrator rights cannot have Windows Installer queue a file
a program still holds, so such a file would stay for good: the removal notes each of the product's
files that is still there, and the task deletes it once it is free.

A per-machine package also protects its own files from an earlier removal: when another product
was removed while a program held one of its files, the file's deletion stays queued for the
restart, and if this package then installs an identical file at that path, Windows Installer keeps
the one already there - so the restart would delete it. The package takes such deletions back as
it installs (and queues them again if the installation fails).

There is no separate uninstall program: Windows' Installed apps removes an MSI with Windows
Installer, and what a removal leaves is the cleanup task's job. What the program itself wrote is not
the package's and stays, unless the package names it: [Wiping at removal](#wiping-at-removal-recursive-on--uninstall).

### Add-ons removed with their product: `parent`, `remove-addons`

An add-on is a package of its own that extends another product - a language pack, a plug-in - and
makes no sense without it. The add-on names the main product by its upgrade code,
`[package] parent = "{...}"`; installing it writes its product code under
`SOFTWARE\rubrapack\Addons\<that upgrade code>` (HKLM per machine, HKCU per user), and removing it
takes the value away again. The main product sets `remove-addons = true`: when it is really removed
(not when an upgrade replaces it), its cleanup task, started at once, waits for the removal to end and
then removes every add-on still named there with `msiexec /x {ProductCode} /qn`, whichever way the
main product was removed (Installed apps, its maintenance page, `msiexec /x`). An add-on that cannot
be removed at that moment (another installation running) is tried again at the task's next run. Two
packages cannot be removed in one Windows Installer run, so the add-ons go a few seconds after the
main product, not before it; build them so that they can be removed without it. It takes effect
from the first main version built with it, and needs the cleanup task (`cleanup = false` is an
error); both keys are for MSI packages only.

### Several instances side by side: `instances`

`[package] instances = N` (at least 2) lets the product be installed more than once on one computer.
rubrapack embeds N-1 *instance transforms* in the package, named `Instance1` .. `Instance(N-1)`.
Install the base product the usual way, and an instance with:

```text
msiexec /i pkg.msi MSINEWINSTANCE=1 TRANSFORMS=:Instance1
```

Each instance has its own ProductCode, its own name (`<Name> (instance K)`, so Installed apps lists
them apart), its own component GUIDs (Windows Installer requires this - two products in one place must
not own the same components) and its own top folder (`<folder> (instance K)`); the upgrade code is
shared, so each instance upgrades itself. It is for a per-machine package (`scope = "machine"`).
Inspect a transform with `rubrapack inspect pkg.msi --transform Instance1`.

### Taking the place of other products: `replaces`

`replaces = ["{UpgradeCode}", ...]` (at most 16) names products this package takes the place of - say,
add-ons that were separate packages and are now features of this one. Installing this package
removes every installed version of them in the same run, as it removes an older version of itself
(RemoveExistingProducts; their own removal runs, with `UPGRADINGPRODUCTCODE` set), so no two products
own the same files. The codes must not be this package's own upgrade code or its `parent`. MSI only.

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
tree. Later - in every dialog set, not only `features` - the same package opened again (or Modify
in Installed apps) offers Change, which leads to that tree: a feature left out at first is added, one
no longer wanted is taken out (see [Dialogs: `ui`](#dialogs-ui)).

- `required = true`: the tree offers no "will be unavailable" for it.
- `follow-parent = true`: installed wherever its parent is (on this computer, or not at all).
- `when = "<condition>"`: the feature is off - not installed and not shown - unless the condition
  holds (see below).
- `default-when = "<condition>"` (with `level` above 1): off by default, but on by default at the
  first installation when the condition holds - for example when a search found that this part was
  installed before as a package of its own (see `replaces`), so a silent upgrade keeps it.

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

`$(NAME)` is the one way to put a name into a string, and `$$` is a literal `$`. Every name is
replaced once; the result is not read again (a value containing `$(X)` stays literal). A name is

- a **build variable**, anywhere: `-D NAME=value` on the command line first, then `[define]`.
  `$(ARCH)` is built in, unless `-D` or `[define]` defines it: the architecture being built (`x64`,
  `arm64` or `x86`) - so one source can name each architecture's program, as
  `source = "bin/$(ARCH)/app.exe"`. An undefined name is an error (`RP1403`).
- a **dir ID** or a **Windows name**, at the start of a path ([Paths](#paths)) and in the values
  Windows Installer fills in during installation: `[registry.*]`, `[env.*]` and `[ini.*]` values,
  `args` of shortcuts, services, file types and links, `[action.*]` `do`/`undo`/`check`,
  `[require.*]` `message` and `[ui]` `launch-args`. Anywhere else it is an error (`RP1404`).

A build variable may not take a Windows name or a dir ID (`RP1404`).

### Windows names

The names of [Windows folders and environment variables](basics/09-folders-and-environment-variables.md),
written with Windows' spelling; letter case does not matter (`$(appdata)` is `$(APPDATA)`). In a
value they become what Windows Installer fills in at install time, for the user who installs - or,
in a `type = "expand"` registry value, the environment variable that the reading program expands
at run time, for the user who runs it:

| Name | Path base | Install time (64-bit / 32-bit package) | Run time (`type = "expand"`) |
|---|---|---|---|
| `ProgramFiles` | yes | `[ProgramFiles64Folder]` / `[ProgramFilesFolder]` | `%ProgramFiles%` |
| `ProgramFiles(x86)` | yes | `[ProgramFilesFolder]` | `%ProgramFiles(x86)%` |
| `ProgramW6432` | - | `[ProgramFiles64Folder]` / `[%ProgramW6432]` | `%ProgramW6432%` |
| `CommonProgramFiles` | yes | `[CommonFiles64Folder]` / `[CommonFilesFolder]` | `%CommonProgramFiles%` |
| `CommonProgramFiles(x86)` | - | `[CommonFilesFolder]` | `%CommonProgramFiles(x86)%` |
| `CommonProgramW6432` | - | `[CommonFiles64Folder]` / `[%CommonProgramW6432]` | `%CommonProgramW6432%` |
| `ProgramData`, `ALLUSERSPROFILE` | yes | `[CommonAppDataFolder]` | `%ProgramData%` ... |
| `APPDATA` | yes | `[AppDataFolder]` | `%APPDATA%` |
| `LOCALAPPDATA` | yes | `[LocalAppDataFolder]` | `%LOCALAPPDATA%` |
| `TEMP`, `TMP` | yes | `[TempFolder]` | `%TEMP%` ... |
| `SystemRoot`, `windir` | yes | `[WindowsFolder]` | `%SystemRoot%` ... |
| `System` | yes | `[System64Folder]` / `[SystemFolder]` | - |
| `Fonts`, `Desktop`, `StartMenu`, `Programs`, `Startup` | yes | `[FontsFolder]`, `[DesktopFolder]`, `[StartMenuFolder]`, `[ProgramMenuFolder]`, `[StartupFolder]` | - |
| `USERNAME` | - | `[LogonUser]` | `%USERNAME%` |
| `COMPUTERNAME` | - | `[ComputerName]` | `%COMPUTERNAME%` |
| `SystemDrive`, `USERPROFILE`, `PUBLIC`, `HOMEDRIVE`, `HOMEPATH`, `USERDOMAIN`, `LOGONSERVER`, `ComSpec`, `Path`, `PATHEXT`, `OS`, `PROCESSOR_ARCHITECTURE`, `NUMBER_OF_PROCESSORS` | - | `[%NAME]` | `%NAME%` |

A dir ID in a value becomes `[ID]`, its folder at install time. A folder's Windows Installer
value ends with `\`, so a `\` right after a folder name is dropped: `'$(INSTALLDIR)\app.exe'`
becomes `[INSTALLDIR]app.exe`. A folder without an environment variable cannot stand in a
`type = "expand"` value (`RP1404`). An MSIX has no install time: a value that needs one is refused
there (`RP1612`, `RP1613`), while `%NAME%` in an expandable value works.

`[NAME]` and `%NAME%` written as they are pass through unchanged, for what has no `$(...)` name:
`[#FileID]`, `[ProductVersion]`, `[\[]` for a literal `[`, `%1` in file type arguments.

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
- does not exist yet and, in a per-machine installation, the deepest folder above it that does
  exist has another owner (a folder made inside someone's folder takes that folder's permissions,
  and its owner can put another folder in its place), or
- is reached through a junction or another reparse point (the folder itself or any folder above it).

A folder that does not exist yet under a folder of a trusted owner - `Program Files`, say - passes
(the installer creates it; `[permission.*]` can lock it).
Repair, upgrade-in-place and removal are not checked; a major upgrade is a first installation of
the new version and is checked like one (the folder the earlier version created passes). The
installation ends with 1603 after the message `DirGuardText` (`[1]` is the folder; in the language
chosen in the dialogs), also at `/qn`, and the log names the owner found. The check is done by
rubrapack's helper DLL, which the package then carries (as for `REG_QWORD` values). What is
checked is the owner: a folder owned by Administrators whose permissions let anyone write is not
refused (lock it with `[permission.*]`).

```toml
[dir.INSTALLDIR]
path = "$(ProgramFiles)/Example"
guard = true
```

### Paths

A target path - `[dir.*]` and `[search.*]` `path` - starts with a folder: a dir ID or a Windows
name that is a path base, as `"$(ProgramFiles)/Example"` or `"$(INSTALLDIR)/docs"`, then `/` and
the folders below. Build variables may follow (`"$(INSTALLDIR)/v$(VERSION)"`).

Source paths are relative to the source file and use `/`. Absolute paths, `\`, symbolic links and
missing files are errors. Target names may use any Unicode text except what Windows forbids
(`< > : " / \ | ? *`, control characters, trailing dot or space, device names such as `CON`), and two
names in one folder may not differ only by letter case. Windows Installer stores a name together
with its 8.3 short name in 255 UTF-16 units, so a long name may use only what the short name
leaves: 242 units for `name.ext` with a three-letter extension, 246 for a name without one
(NTFS would accept 255). A longer name is refused with RP1514.

## Command line

```text
rubrapack build <src.toml> -o <out.msi|out.msm|out.msix|out.msixbundle> [-D NAME=VALUE]...
                [--arch x64|arm64|x86 | --arch <list> (.msixbundle)]
                [--compress none|mszip|mszip:N|lzx|lzx:N] [--jobs N] [--nfc] [--reproducible]
                [<key> [--cert <chain.pem>]
                 [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                  [--proxy <URL>]] [--allow-unsigned-cabs]]
                [--unsigned-test] [--msix-compress deflate|store]      (.msix, .msixbundle)
rubrapack inspect <file.msi> [table | --summary | --files | --streams]
rubrapack inspect <file.msi> [table | --summary] --json
rubrapack inspect <file.msix|file.msixbundle> [--files | --manifest]
rubrapack inspect <file.cab>
rubrapack new [msi] <name>
rubrapack new [<file>.toml] [-i]
rubrapack new <name> [--dist <folder>] [--name ...] [--ui ...] [--optional ...] ...
rubrapack edit <file>.toml [--set <table>.<key>=<value>]... [--unset <table>.<key>]... [--sync]
rubrapack guid [--from <text>]
rubrapack lint <src.toml> [-D NAME=VALUE]... [--arch x64|arm64|x86] [--target msi|msix] [--nfc] [--strict] [--json]
rubrapack lint <file.msi> [--previous <old.msi>] [--strict] [--json]
rubrapack explain [RPnnnn]
rubrapack schema
rubrapack lint <file.msix|file.msixbundle> [--strict]
rubrapack extract <file.msi|file.msix|file.msixbundle|file.cab> -d <new dir> [--limit-entries N] [--limit-bytes N]
rubrapack sign <file.exe|.dll|.msi|.msp|.msix|.msixbundle> <key> [--cert <chain.pem>]
               [--timestamp <URL> [--tsa-trust <certificates>] [--tls-trust <certificates>] [--system-roots]
                [--proxy <URL>]] [--allow-unsigned-cabs] [-o <out>]
rubrapack keys list [--pkcs11 <module> [--token-label <label>] [--pin-env VAR | --pin-file FILE]]
rubrapack verify <file.exe|.dll|.msi|.msp|.msix|.msixbundle> [--trust <certificates>]... [--system-roots] [--tsa-trust <certificates>]...
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
- `rubrapack new <file>.toml --from <package.msi>` makes a source from an existing package
  (RFC-0023): its files go into a new folder beside the source (`<stem>-files`, or `--dist <name>`),
  laid out as `extract` lays them out, and the source describes the package with them - name,
  version, architecture, upgrade code, folders, files (with each component's code where the file is
  its key path, so the result upgrades the original; the product code is not carried - a new
  version needs its own - and is named in a note), features, registry values,
  shortcuts, environment variables, INI values, services, public properties, launch conditions,
  copies, removals, fonts, permissions, the Installed apps settings, and for a package rubrapack
  made, its own dialog set, launch box, actions, qword values and guard. What a source cannot say
  - custom actions, the original's own dialogs, searches, COM and other tables - is listed at the
  end of the source, table by table, with what was changed (a folder at the root of a drive moved
  under Program Files, a per-user value left out of a per-machine package, ...). `--publisher
  "CN=..."` also writes `[msix]` and an `[msix-app.*]` (the program of the Start menu shortcut), so
  the same source builds an `.msix`; what an MSIX cannot carry is then refused by the build and
  takes `msi-only = true`. A merge module is refused (merge it with `[merge.ID]`).
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
- `explain [RPnnnn]` says what a diagnostic code is about, the messages that carry it, what to do
  and where the manual says more; without a code, the ranges (RFC-0025).
- `--json` on `lint` (a source, a package, `--previous`) prints one JSON object on stdout - `file`,
  `exit`, `errors`, `warnings`, `not_shown`, and `diagnostics` with `file`, `line`, `column`,
  `severity`, `code`, `message` (line and column 0 for one about the file as a whole) - with the
  same exit code; on `inspect <file.msi>` it gives every table, one table or `--summary` as JSON.
- `schema` prints the JSON Schema of sources (tables, keys, required keys, booleans and numbers),
  made from the parser's key lists, for an editor: `#:schema ./rubrapack.schema.json` as the
  source's first line in Taplo-based editors.
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
- `transform <base.msi> <target.msi> -o <out.mst>` writes a transform: the rows that turn the base
  into the target (inserted, changed, deleted, and tables added or dropped), as msi.dll's own
  `MsiDatabaseGenerateTransform` writes them, with the summary information that names both
  packages. `--validate` sets what Windows checks before it applies the transform: `product-code`
  and `upgrade-code` (the default), `language`, `platform`, or `none`. Refused (`RP0013`): tables
  whose columns differ, a changed `File` or `Media` table (a transform carries no files), a changed
  column past the 16th of its table. `inspect <file.mst> --base <base.msi>` lists the rows,
  `inspect <file.mst> --summary` the summary information.
- `patch <base.msi> <target.msi> -o <out.msp>` writes a patch that updates an installed base to
  the target in place: a product transform (the rows that differ; existing files keep their place
  in the base's cabinets), a patch transform (the new and changed files' new places, a Media row for
  the patch's cabinet, PatchPackage, the PatchFiles action) and the cabinet of those files whole, as
  Microsoft's `MsiMsp.exe` lays them out. Both packages need the same product and upgrade code, the
  same first two version numbers (a *small update* or *minor upgrade*), embedded cabinets, and every
  file and component of the base. `--patch-code {GUID}` (default: derived from the two package
  codes), `--family <name>` (MsiPatchSequence; default the product name), `--no-removal` (the
  patch cannot be removed on its own). Refusals are `RP0013`. `inspect <file.msp> --base <base.msi>`
  lists the patch's tables and both transforms. `sign` signs a patch like a package (its
  transforms are part of the signature), and `verify` checks one.
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
- `--lang <code>` chooses the language of what the tool prints for a person: errors and warnings,
  `help`, the result lines, `explain`, the questions of `new -i` and `edit`. The codes are `en`
  (the default), the codes of the built-in packs (`ko`, `ja`, `zh`, `zh_TW`, `es`, `fr`, `de`, `pt_BR`, `pt_PT`, `ru`, `it`, `nl`, `sv`, `da`, `nb`, `fi`, `pl`, `cs`, `uk`, `hu`, `ro`, `tr`; `help` lists them), and any language whose pack is found ([Language packs](#language-packs)). It may stand anywhere on the line (`rubrapack --lang ko build ...`,
  `rubrapack lint app.toml --lang=ko`); the environment variable `RUBRAPACK_LANG` says the same
  for every run, and the option wins over it. What a program reads stays as it is: the diagnostic
  codes, exit codes, table dumps of `inspect`, `schema`, and the structure of `--json` (whose
  `message` texts follow the language). It never changes what is built - the languages of a
  package's dialogs are the source's (`[ui] languages`).

## Diagnostic codes

Every problem rubrapack reports has a code, `error[RPnnnn]` or `warning[RPnnnn]`, after the file,
line and column it is about. The first two digits say what kind of problem it is:

| Codes | What went wrong | Where to look |
|---|---|---|
| RP00xx | the command line: an unknown command or option, a file that cannot be read or written, a difference a transform cannot carry (`RP0013`) | `rubrapack help <command>` |
| RP10xx | the source file's encoding: not UTF-8 (or UTF-16 with a BOM), stray carriage returns | save the file as UTF-8 |
| RP11xx | TOML outside the subset rubrapack reads: multi-line strings, inline tables, a table defined twice; `format` missing or too new (RP1108) | [The TOML subset](#the-toml-subset), [Source format](#source-format) |
| RP12xx | tables and keys: an unknown table or key (with a suggestion), a required key or table missing, something without a feature once features exist | [Tables](#tables) |
| RP13xx | values: IDs (unique across all tables, not reserved), GUIDs, versions, numbers out of range, references to things that do not exist | the table's section |
| RP14xx | variables: `$(NAME)` without a value, `$(` not closed, a Windows name or dir ID where it cannot stand | [Variables](#variables) |
| RP15xx | the files to install: not found, a directory, a link, a glob without a match, a program for another architecture, names too long, a file that changed during the build, a package too large | [Paths](#paths), [Program files](#program-files) |
| RP16xx | MSIX: what an MSIX package needs, and what it cannot carry | [MSIX packages](#msix-packages) |
| RP19xx | a feature this rubrapack does not provide | - |
| RP20xx | the finished tables break a Windows Installer rule. A source that passes the RP1xxx checks should never meet these: please report it | - |
| RP21xx | `lint` of a package: dialogs, code page, text normalisation | `lint` under [Command line](#command-line) |
| RP22xx | `lint` of an MSIX package or bundle | `lint` under [Command line](#command-line) |
| RP23xx | `lint --previous`: this package would not upgrade the previous one cleanly | [Versions and upgrades](tutorial/03-versions-and-upgrades.md) |

The message says what to change; the codes above are the ones to search the manual for.
