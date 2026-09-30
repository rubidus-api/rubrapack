# Conditions and searches

Goal: decide *on the user's computer* what to install. Install only on 64-bit Windows; offer a
part only where Notepad exists; make the desktop shortcut something the user can turn off; and
install the next version into the folder the user chose for this one.

## Properties

Windows Installer keeps named values called *properties* while it runs: `ProductName`,
`INSTALLDIR`, `VersionNT64` (set on 64-bit Windows), and your own. A property whose name is all
capitals is *public*: it can be set on the command line (`msiexec /i hello.msi DESKTOP_SHORTCUT=0`)
and by the dialogs. A *condition* is a small expression over properties, true or false:

| Condition | True when |
|---|---|
| `VersionNT64` | the property is set (not empty): here, on 64-bit Windows |
| `NOT Installed` | the product is not installed yet (a first installation) |
| `DESKTOP_SHORTCUT = "1"` | the property's value is exactly `1` |
| `VersionNT >= 603` | the Windows version number is 6.3 or later |
| `A AND (B OR NOT C)` | combinations, as you would expect |

## The source

```toml
# tutorial 09: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
ui = "features"

[define]
VERSION = "1.7.0"

[property.DESKTOP_SHORTCUT]
value = "1"
secure = true

[require.Windows64]
condition = "VersionNT64"
message = "[ProductName] needs 64-bit Windows."

[search.Notepad]
property = "NOTEPAD_PATH"
kind = "file"
path = "Windows"
file = "notepad.exe"

[search.PreviousDir]
property = "INSTALLDIR"
kind = "registry"
root = "HKLM"
key = "Software\\Example Software\\Hello"
name = "InstallDir"

[feature.Main]
title = "Hello"
required = true

[feature.NotepadHelper]
title = "Open notes in Notepad"
description = "Only offered where Notepad is installed."
when = "NOTEPAD_PATH"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"
feature = "Main"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"
feature = "NotepadHelper"

[registry.RememberDir]
root = "HKLM"
key = "Software\\Example Software\\Hello"
name = "InstallDir"
value = "[INSTALLDIR]"
feature = "Main"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"

[shortcut.Desktop]
dir = "Desktop"
name = "Hello"
target = "file:Hello"
when = "DESKTOP_SHORTCUT = \"1\""
```

## Your own property: `[property.NAME]`

`[property.DESKTOP_SHORTCUT]` creates the property with the default `1`. Names must be all capitals
to be public; names Windows or rubrapack use themselves (`ALLUSERS`, `ARP...`, `MSI...`, ...) are
refused.

- `secure = true`: an installation for all users runs in two parts - the user's part (dialogs) and
  an elevated part that changes the computer. A value set in the first part or on the command line
  reaches the second only if the property is *secure*. Make every property that a `when` condition
  tests secure. (rubrapack does this by itself for the properties of your own dialog pages and for
  search results.)
- `hidden = true`: the value is not written into installation logs - for passwords and keys.

## Installing something only when: `when`

`when` on a feature, a file or file group, a shortcut, a registry value, an environment variable or
an INI value installs it only when the condition is true at the first installation:

- `when = "DESKTOP_SHORTCUT = \"1\""` on the desktop shortcut: by default it is made; with
  `msiexec /i hello.msi DESKTOP_SHORTCUT=0` it is not. Chapter 14 turns the property into a tick
  box on a page of your own.
- `when = "NOTEPAD_PATH"` on a feature: where the condition is false, the feature is neither
  installed nor shown in the tree.

A repair keeps what was installed; a major upgrade (the next version) decides again.

## Refusing to install: `[require.ID]`

A requirement stops a first installation with its `message` when its `condition` is false - here,
on 32-bit Windows: "Hello needs 64-bit Windows." Repair and removal are never blocked.

## Looking around first: `[search.ID]`

A search runs before anything else and puts what it found into a property (empty if nothing was
found), which conditions can then test:

| `kind` | Finds | Keys |
|---|---|---|
| `file` | the full path of a file | `path` (a known folder and a relative path), `file`, `min-version` (for programs) |
| `dir` | a folder | `path` |
| `registry` | a value in the registry | `root`, `key`, `name`, `view` (`32` for the 32-bit view) |
| `component` | the key file of another product's component | `component-guid` |

`[search.Notepad]` looks for `notepad.exe` in the Windows folder and puts its path into
`NOTEPAD_PATH`.

## Remembering the install folder

If a user installed Hello into `D:\Tools\Hello`, the next version should go there too, not back to
Program Files. Two pieces do it:

1. `[registry.RememberDir]` writes the folder into the registry when installing: `[INSTALLDIR]`
   in a value is replaced by the folder (chapter 11 is about the registry).
2. `[search.PreviousDir]` reads it back. Its `property` is the dir ID `INSTALLDIR`, so what it
   finds becomes that dir's *default* - but only if that folder still exists, and a folder given
   on the command line still wins.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi LaunchCondition
Installed OR (VersionNT64)	[ProductName] needs 64-bit Windows.
C:\work\hello> rubrapack inspect hello.msi AppSearch
NOTEPAD_PATH	Notepad
RpFound_PreviousDir	PreviousDir
C:\work\hello> rubrapack inspect hello.msi Condition
NotepadHelper	0	NOT Installed AND NOT (NOTEPAD_PATH)
```

(Headers left out.) rubrapack adds `Installed OR` to requirements, so that an installed product can
always be repaired or removed. A feature's `when` becomes a row in the `Condition` table that sets
its level to 0 - "not installed, not shown" - when the condition is false; `NOT Installed` keeps
that from happening at removal, when properties are gone. Every shortcut has a component of
its own (chapter 5); the desktop shortcut's holds the `when` in its `Condition` column:

```text
C_2ea6d5bfd9b4a9073b30	{BCBFF063-...}	DesktopFolder	260	DESKTOP_SHORTCUT = "1"	R_7f1795ed8aa7ac1a1c60
```
