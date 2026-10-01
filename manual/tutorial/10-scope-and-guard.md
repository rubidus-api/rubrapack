# Who it installs for, and a guarded folder

Goal: let a user without administrator rights install Hello for themselves, while an
administrator can still install it for everyone - and refuse an install folder that someone else
prepared.

## Three scopes

So far every package installed *per machine*: into Program Files, for every user of the computer,
which needs administrator rights (the User Account Control prompt). `scope` in `[package]` changes
that:

| `scope` | Installs for | Needs administrator | `$(ProgramFiles)/Hello` becomes | Start menu, desktop | Registry `HKMU` |
|---|---|---|---|---|---|
| `machine` (default) | everyone | yes | `C:\Program Files\Hello` | all users' | `HKLM` |
| `user` | the current user | no | `%LOCALAPPDATA%\Programs\Hello` | the user's | `HKCU` |
| `dual` | the user by default, or everyone when chosen | only for everyone | either of the above | either | either |

`%LOCALAPPDATA%` is the user's own `C:\Users\<name>\AppData\Local`. A per-user installation needs
no permission and is invisible to other users of the computer - often right for small tools.

Some things only exist per machine and need `scope = "machine"`: services, fonts, permissions,
and the folders `$(SystemRoot)`, `$(System)`, `$(Fonts)` and `$(ProgramData)`. rubrapack refuses them in a
`user` or `dual` package.

## The source

```toml
# tutorial 10: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
scope = "dual"
ui = "installdir"

[define]
VERSION = "1.8.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"
guard = true

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"
```

## A dual package

With `scope = "dual"` and dialogs, a page after the license asks:

```text
( ) Just me             (the default)
( ) Everyone on this computer
```

"Everyone" asks for administrator rights; the install folder, the Start menu entry and the
Installed apps entry follow the choice. Without dialogs, a dual package installs for the current
user; from an administrator's terminal it installs for everyone with

```text
msiexec /i hello.msi ALLUSERS=1 MSIINSTALLPERUSER=""
```

(`MSIINSTALLPERUSER=""` means "empty": the property is cleared.) A `user` package refuses to install
for everyone.

## A guarded install folder: `guard`

Some programs are loaded by other programs or run with high rights - an input method, a shell
extension, a service. For them an install folder that *already exists* is a risk: whoever created
it may have left files in it (a planted DLL would be loaded with the program's rights). `guard = true`
on a dir makes a first installation stop, before any file is placed, when that folder

- already exists and is not owned by SYSTEM, Administrators or TrustedInstaller, or
- is reached through a junction or another link (the folder itself or any folder above it).

A folder that does not exist yet passes - the installer creates it. Repair, removal and an upgrade
into the folder the earlier version created are not affected. The installation ends with the
message `DirGuardText` (in the language chosen in the dialogs), also in a silent installation, and
the log names the owner it found. The check is done by a small helper DLL of rubrapack's that the
package then carries.

## Try it

As a normal user (not an administrator), double-click `hello.msi` and keep "Just me": no
permission prompt; Hello lands in `%LOCALAPPDATA%\Programs\Hello`, and only you see it in the
Start menu and in Installed apps. Remove it, and install again choosing "Everyone": Windows asks
for administrator rights and installs into Program Files.

To see the guard: as a normal user, create the folder `C:\Users\Public\Hello` (its owner is then
that user). From an administrator's terminal, install for everyone into it:

```text
msiexec /i hello.msi ALLUSERS=1 MSIINSTALLPERUSER="" INSTALLDIR="C:\Users\Public\Hello\"
```

The installation stops with the guard's message and installs nothing. Delete the folder and run
the same command again: now the installer creates the folder itself, and the installation goes
through.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi Property
...
ALLUSERS	2
MSIINSTALLPERUSER	1
RP_GUARD	INSTALLDIR
...
```

`ALLUSERS = 2` together with `MSIINSTALLPERUSER = 1` is Windows Installer's "single package for
both": per user unless asked otherwise. A per-machine package has `ALLUSERS = 1`. `RP_GUARD` lists
the guarded dirs for the helper DLL. The page that asks is `RpScopeDlg` in the `Dialog` table.
