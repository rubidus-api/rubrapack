# Shortcuts, icons and the Installed apps entry

Goal: a Start menu folder "Example Software" with Hello and its readme, a desktop shortcut with
its own icon, and an icon and links for Hello in Installed apps.

You need an icon file, `hello.ico`, next to the source (any `.ico` will do; many image editors
save one).

## The source

```toml
# tutorial 05: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.3.0"

[arp]
icon = "hello.ico"
help = "https://example.com/hello/help"
about = "https://example.com/hello"
no-repair = true

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[dir.MenuFolder]
path = "Programs/Example Software"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Readme]
dir = "INSTALLDIR"
source = "dist/readme.txt"

[shortcut.StartMenu]
dir = "MenuFolder"
name = "Hello"
target = "file:Hello"
description = "Says hello"
working-dir = "INSTALLDIR"

[shortcut.ReadmeLink]
dir = "MenuFolder"
name = "Hello - read me"
target = "file:Readme"

[shortcut.Desktop]
dir = "Desktop"
name = "Hello"
target = "file:Hello"
args = "--greet \"[ProductName] user\""
icon = "hello.ico"
```

## Shortcuts: `[shortcut.ID]`

- `dir` is where the shortcut goes: a dir of your own, or directly one of `Programs` (the Start
  menu's program list), `StartMenu`, `Desktop`, `Startup` (started at every sign-in). Here the dir
  `MenuFolder` is a folder "Example Software" in the Start menu's programs, so the Start menu shows
  "Example Software" with two entries.
- `name` is the text under the shortcut.
- `target` is the file it opens: `file:` and the ID of a `[file.*]` of this package.
- `description` is the tool tip; `working-dir` (a dir ID) the folder the program starts in.
- `args` are the program's arguments. They are an MSI *formatted string*: `[ProductName]` becomes
  "Hello" when the shortcut is made. A double quote inside a TOML string is written `\"`.
- `icon` gives the shortcut its own `.ico`; without it Windows shows the program's own icon.

The Start menu folder rubrapack makes for shortcuts is removed again with the product, as are
the shortcuts.

## The Installed apps entry: `[arp]`

("ARP" is the old name of the list: Add or Remove Programs.)

| Key | Effect |
|---|---|
| `icon = "hello.ico"` | the icon in the list |
| `help = "https://..."` | a support link Windows shows with the product |
| `about = "https://..."` | the product's web page |
| `no-repair = true` | no Repair for this product |
| `no-modify = true` | no Modify/Change (useful when the package has nothing to change: chapter 8 adds something) |

## A name in another script

`name` may be in any language (`name = "안녕"`). The file properties of the `.msi` itself (its
Details tab) can only show plain ASCII text, so for such a name give an ASCII form as well:
`summary-name = "Annyeong"` in `[package]`. Without it rubrapack warns (`RP1203`) and the
properties read "rubrapack package".

## Try it

Build, install, and open the Start menu: "Example Software" holds "Hello" and "Hello - read me",
and the desktop has "Hello" with the icon you gave. Installed apps shows Hello with its icon. Remove
it: the shortcuts and the Start menu folder go too.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi Shortcut
Shortcut	Directory_	Name	Component_	Target	Arguments	Description	Hotkey
...
Desktop	DesktopFolder	Hello	C_2ea6d5bfd9b4a9073b30	[INSTALLDIR]hello.exe	--greet "[ProductName] user"
ReadmeLink	MenuFolder	HELLO-~1|Hello - read me	C_1643d000d3751c809248	[INSTALLDIR]readme.txt
StartMenu	MenuFolder	Hello	C_a7452f86f3da3f7d2a6c	[INSTALLDIR]hello.exe		Says hello
```

- Each shortcut has a component of its own, in its target file's feature: it is installed and
  removed with the file. Its key path is a value under `HKEY_CURRENT_USER\Software\Example
  Software\Hello\Shortcuts`, because Windows' rules treat the Start menu and the desktop as the
  user's places (Part IV, [checking against Windows](../formats/verify.md#microsofts-ice-rules-observed)).
- `[INSTALLDIR]hello.exe` is the target: the folder `INSTALLDIR`, wherever it was installed, and
  the file name.
- `HELLO-~1|Hello - read me` holds two names: an old-style short name (8 characters, a dot, 3
  characters - from the days of MS-DOS) and the long one. Windows Installer wants both for every
  name; rubrapack makes the short ones up.

The icons are stored inside the package (the `Icon` table), and the `Property` table carries the
Installed apps settings:

```text
ARPHELPLINK	https://example.com/hello/help
ARPNOREPAIR	1
ARPPRODUCTICON	RpIcon1.ico
ARPURLINFOABOUT	https://example.com/hello
```
