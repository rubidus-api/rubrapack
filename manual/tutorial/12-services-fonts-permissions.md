# Services, fonts, permissions

Goal: install a background service that starts by itself, a font every program can use, and a
data folder whose access rights you set - the three things only a per-machine package may do.

## The source

`dist\hellosvc.exe` is the service program, `dist\HelloSans.ttf` a font file.

```toml
# tutorial 12: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
reboot = "suppress"

[define]
VERSION = "1.10.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[dir.DataDir]
path = "CommonAppData/Hello"

[dir.FontsDir]
path = "Fonts"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Service]
dir = "INSTALLDIR"
source = "dist/hellosvc.exe"

[file.Font]
dir = "FontsDir"
source = "dist/HelloSans.ttf"

[service.HelloService]
file = "file:Service"
name = "HelloService"
display-name = "Hello background service"
description = "Keeps Hello's greetings up to date."
start = "auto"
account = "LocalService"
args = "--service"
start-on-install = true

[font.HelloSans]
file = "file:Font"
title = "Hello Sans"

[permission.DataFolder]
target = "dir:DataDir"
sddl = "D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)"
```

## A service: `[service.ID]`

A *service* is a program Windows runs in the background, without a window, often before anyone
signs in (the Services console, `services.msc`, lists them).

| Key | Meaning |
|---|---|
| `file` | the service's program: `file:` and a `[file.*]` ID |
| `name` | the service's internal name (used by `sc start HelloService`) |
| `display-name`, `description` | what the Services console shows |
| `start` | `auto` (at every start of Windows), `demand` (when something starts it), `disabled` |
| `account` | who it runs as: `LocalSystem` (full rights), `LocalService` (few rights - prefer it), `NetworkService` |
| `args` | its command-line arguments |
| `start-on-install = true` | start it at the end of the installation |

The installer stops the service before its files are replaced (in an upgrade or a repair) and at
removal, and deletes it at removal. The program must really be a service - one that answers
Windows' service manager; an ordinary program would make the start fail, and with it the
installation.

## A font: `[font.ID]`

A font is installed by putting its file into the Fonts folder - `[dir.FontsDir]` with
`path = "Fonts"`, the known folder alone - and registering it with `[font.ID]`. `title` is the name
Windows lists; without it Windows reads the name from the font file. Every program sees the font
after the installation; removal unregisters and deletes it.

## Access rights: `[permission.ID]`

Every file, folder and registry key in Windows has an access list saying who may read or change
it. `[permission.ID]` sets that list, written in *SDDL* (Security Descriptor Definition Language),
on a folder the package creates (`dir:ID`), one of its files (`file:ID`) or a registry value it
writes (`registry:ID`). A folder given a permission is created even without files in it.

The SDDL string above, piece by piece:

| Piece | Meaning |
|---|---|
| `D:` | the access list (DACL) follows |
| `PAI` | protected (do not inherit from the parent folder), and passed on to what is inside |
| `(A;OICI;FA;;;SY)` | Allow, to files and sub folders (Object and Container Inherit), Full Access, to SYSTEM |
| `(A;OICI;FA;;;BA)` | the same for the Built-in Administrators |
| `(A;OICI;0x1301bf;;;BU)` | Built-in Users may read, write and delete, but not change the rights |

So `C:\ProgramData\Hello` is writable by every user, but only administrators may change who may.
Get a string for a folder you set up by hand with `icacls` or PowerShell's `(Get-Acl C:\path).Sddl`.

## Restarting: `reboot`

Sometimes a file cannot be replaced until Windows restarts. `reboot = "suppress"` (the default)
never restarts by itself: the installation ends with exit code 3010 ("restart needed") and the
user restarts later. `reboot = "allow"` lets Windows Installer ask for, or at `/qn` perform, a
restart when one is needed.

## Try it

Install (as administrator), then: `services.msc` lists "Hello background service", running; a
text editor offers the font "Hello Sans"; the Security tab of `C:\ProgramData\Hello` shows the
three entries. Remove Hello: the service, the font and the folder are gone.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi ServiceInstall
HelloService	HelloService	Hello background service	16	2	1			NT AUTHORITY\LocalService		--service	C_d677...	Keeps Hello's greetings up to date.
C:\work\hello> rubrapack inspect hello.msi ServiceControl
HelloService	HelloService	163		1	C_d677...
```

`16` is "a service in its own process", `2` "automatic start", `1` "report errors normally". The
`163` in `ServiceControl` is a sum of bit flags: 1 (start at install) + 2 (stop at install) + 32 (stop
at removal) + 128 (delete at removal). [Bit flags](../basics/02-numbers-and-flags.md#bit-flags) in Part III explains how such sums work. A package with
permissions declares Windows Installer 5.0 (the summary's schema, `14 500` in `rubrapack inspect
hello.msi --summary`), because the `MsiLockPermissionsEx` table is newer than the rest.
