# The registry, environment variables, INI files, file types and links

Goal: give Hello the settings other programs look for - registry values of every type, an
environment variable, a line in its INI file - and let Windows open `.hello` files and `hello:`
links with it.

## The registry in two minutes

The Windows *registry* is a database of settings, organised like folders: *keys* contain other
keys and *values*; a value has a name, a type and data. The top-level keys ("roots") are:

| Root | Holds |
|---|---|
| `HKLM` (HKEY_LOCAL_MACHINE) | settings for the whole computer; writing needs administrator rights |
| `HKCU` (HKEY_CURRENT_USER) | the current user's settings |
| `HKCR` (HKEY_CLASSES_ROOT) | file types and links (a merged view of both) |
| `HKMU` | rubrapack's (and Windows Installer's) "HKLM when installed for everyone, HKCU when for one user" |

Run `regedit` on Windows to look around (and do not change what you do not know).

## The source

```toml
# tutorial 11: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.9.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"

[registry.InstallDir]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "InstallDir"
value = "[INSTALLDIR]"

[registry.Greeting]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Greeting"
value = "Hello, world"
with = "file:Hello"

[registry.Runs]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "MaxRuns"
type = "dword"
value = 100

[registry.BigNumber]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Limit"
type = "qword"
value = "0x100000000"

[registry.Colors]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Colors"
type = "multi"
value = ["red", "green", "blue"]

[registry.LogPath]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "LogPath"
type = "expand"
value = "%TEMP%\\hello.log"

[registry.Legacy32]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "Mode"
value = "compatible"
view = "32"

[registry.OldKey]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello Old'
remove = true

[registry.UserChoice]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "FirstRun"
type = "dword"
value = 1
keep = true

[env.HelloHome]
name = "HELLO_HOME"
value = "[INSTALLDIR]"

[env.Path]
name = "PATH"
value = "[INSTALLDIR]"
mode = "append"

[ini.InstalledVersion]
dir = "INSTALLDIR"
file = "settings.ini"
section = "hello"
key = "installed"
value = "[ProductVersion]"

[ini.Plugins]
dir = "INSTALLDIR"
file = "settings.ini"
section = "hello"
key = "plugins"
value = "core"
mode = "add"

[assoc.HelloDoc]
extension = ".hello"
prog-id = "Example.HelloDocument"
description = "Hello document"
target = "file:Hello"
icon = "file:Hello"
args = "--open \"%1\""

[protocol.HelloLink]
name = "hello"
description = "Hello link"
target = "file:Hello"
```

TOML note: a string in *single* quotes is taken as it is, so `'SOFTWARE\Example Software\Hello'`
needs no doubled backslashes; in double quotes a backslash starts an escape (`"%TEMP%\\hello.log"`).

## Registry values: `[registry.ID]`

| `type` | TOML value | Registry type |
|---|---|---|
| `string` (default) | a string | REG_SZ |
| `expand` | a string with `%VARIABLES%` | REG_EXPAND_SZ - Windows expands `%TEMP%` when the value is read |
| `dword` | an integer 0 .. 4294967295 | REG_DWORD, 32 bits |
| `qword` | an integer, or `"0x"` and up to 16 hex digits | REG_QWORD, 64 bits |
| `binary` | hex digits, `"01ff"` | REG_BINARY |
| `multi` | an array of strings | REG_MULTI_SZ |

- `name` omitted: the key's default value (shown as "(Default)" in regedit).
- `value` is a formatted string: `[INSTALLDIR]` becomes the install folder, `[#Hello]` the path of
  the file `Hello`.
- Every value is removed again with the product; `keep = true` leaves it (`FirstRun` here, which
  the program may change).
- `with = "file:Hello"` ties the value to that file: installed and removed with it (`Greeting`).
- `view = "32"`: 64-bit Windows keeps a separate registry for 32-bit programs; a 32-bit program
  reading `SOFTWARE\Example Software\Hello` sees that one. Values go to the 64-bit view in an x64
  package unless `view = "32"`.
- `remove = true` deletes, during installation, the value named by `name` - or, without `name`, the
  whole key (`OldKey`: something an older version left behind).

Windows Installer cannot write REG_QWORD by itself; for `qword` values rubrapack adds its small
helper DLL to the package, which writes them and puts the previous values back if the installation
fails.

## Environment variables: `[env.ID]`

A variable for the whole computer (per user in a per-user installation). `mode = "set"` (default)
sets it, `append` adds `;value` to the end of what is there, `prepend` `value;` to the front. The
product's removal undoes exactly that: `HELLO_HOME` is deleted; from `PATH` only `;C:\Program
Files\Hello\` is taken out. `keep = true` leaves it. Programs started after the installation see
the change (a terminal that was already open does not).

## INI files: `[ini.ID]`

An INI file is a text file of `[section]` and `key=value` lines. `mode = "set"` (default) writes
`key=value` in the section, `add` adds the value to a comma-separated list (`plugins=core` or, if
there was `plugins=extra`, `plugins=extra,core`), and `remove` deletes the key - before the files
are installed, so it cleans up an INI file that an older version left. Removal takes out what `set`
and `add` wrote. `file` is the file's name in `dir`; it does not have to be installed by the package.

After installing, `settings.ini` reads:

```text
[hello]
greeting=Hello
installed=1.9.0
plugins=core
```

## File types: `[assoc.ID]`

`[assoc.HelloDoc]` makes Windows open `.hello` files with Hello:

- `extension` is the file ending, lower case, with the dot;
- `prog-id` names the kind of document (`Company.Kind`); several extensions may share one;
- `description` is what Explorer calls the type ("Hello document");
- `target` is the program, `args` its arguments (`"%1"` is the file's path; the default);
- `icon` (`file:ID`) is the file whose first icon the documents show.

If the user has already chosen another program for `.hello`, Windows keeps that choice.

## Links: `[protocol.ID]`

`[protocol.HelloLink]` registers the scheme `hello:`, so a link `hello:world` - in a browser, in
the Run box - starts Hello with the whole link as its argument.

## COM classes: `[com.ID]`

A program or DLL that other programs create objects from - by a class ID, or by a name such as
`Hello.Widget` in a script - has to be registered as a COM class. Hello has none, but this is all it
would take for a DLL `widget.dll` in the package:

```toml
[com.Widget]
file = "file:WidgetDll"
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D35}"     # rubrapack guid makes one
description = "Hello widget"
prog-id = "Hello.Widget"
threading = "both"
```

- `file` is the server: a `.dll` (loaded into the caller) or an `.exe` (started; `args` are its
  arguments, often `-Embedding`);
- `class` is the class ID the server answers to, `prog-id` the readable name for it;
- `threading` is the DLL's apartment: `sta` (the default), `mta`, `both` or `neutral`;
- `app-id` and `surrogate = true` let the DLL run in a separate process (dllhost), and `typelib`,
  `typelib-version` and `typelib-file` register a type library with it.

PowerShell then makes one with `New-Object -ComObject Hello.Widget`. Removal unregisters it.

A DLL class can also be an Explorer handler for a file type - the picture Explorer shows for a
`.hello` file, what its preview pane shows, the properties it lists:

```toml
[handler.HelloThumbs]
kind = "thumbnail"                # or "preview", "property"
class = "{8D1E2F30-4A5B-4C6D-8E7F-901A2B3C4D41}"     # a [com.*] class of the DLL
types = [".hello"]
```

`description` names a preview handler; a property handler needs a per-machine package; and in an
MSIX thumbnail and preview handlers work (a preview handler's class with `threading = "sta"`) but
property handlers do not, so a property handler there takes `msi-only = true`.

## Try it

Install, then:

- `regedit`: `HKEY_LOCAL_MACHINE\SOFTWARE\Example Software\Hello` holds the values;
  `...\WOW6432Node\Example Software\Hello` holds `Mode` (the 32-bit view).
- a new terminal: `echo %HELLO_HOME%` and `echo %PATH%`.
- `C:\Program Files\Hello\settings.ini`: the two new lines.
- a file `test.hello` on the desktop opens Hello; Win+R, `hello:world`, Enter starts Hello.

Remove Hello: all of it goes, except `FirstRun`.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi Registry
...
Colors	2	SOFTWARE\Example Software\Hello	Colors	[~]red[~]green[~]blue[~]	C_f023...
Greeting	2	SOFTWARE\Example Software\Hello	Greeting	Hello, world	C_185f...
HelloDoc.Ext	0	.hello		Example.HelloDocument	C_185f...
LogPath	2	SOFTWARE\Example Software\Hello	LogPath	#%%TEMP%\hello.log	C_2e80...
Runs	2	SOFTWARE\Example Software\Hello	MaxRuns	#100	C_97ef...
...
```

The second column is the root (0 HKCR, 1 HKCU, 2 HKLM, -1 HKMU). The value column carries the type
in its first characters, as Windows Installer wants: `#100` is a DWORD, `#%` an expandable string,
`[~]` separates the strings of a multi-string, `#x` starts binary data. File types and links are
nothing but registry values under HKCR, in the program's component. The QWORD value is not in this
table: it travels in the property `RP_QWORDS` for the helper DLL. Environment variables are in the
`Environment` table:

```text
HelloHome	=-*HELLO_HOME	[INSTALLDIR]	C_0fa9...
Path	=-*PATH	[~];[INSTALLDIR]	C_82df...
```

In the name, `=` means "set", `-` "remove at uninstall", `*` "a system variable"; `[~]` in the value
stands for what was there before.
