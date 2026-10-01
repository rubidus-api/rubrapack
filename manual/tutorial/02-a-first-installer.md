# A first installer: one program

Goal: an installer that copies `hello.exe` into `C:\Program Files\Hello`, shows up in Installed
apps, and removes itself cleanly.

## The source

In the folder `C:\work\hello` (see the previous chapter), create a text file `hello.toml` with any
text editor - Notepad will do - and write:

```toml
# tutorial 02: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "1.0.0"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"
```

Line by line:

- A line starting with `#` is a comment; rubrapack ignores it.
- `format = 1` comes first and says which version of rubrapack's source format the file is
  written in. Every source starts with it.
- `[package]` starts a *table*: the lines after it, up to the next `[...]`, belong to it. The
  `package` table says what the product is.
  - `name` is what users see: in Installed apps, in the title of the installer's windows.
  - `manufacturer` is you or your company; Installed apps shows it under the name.
  - `version` has three numbers (or four): major, minor, build. Chapter 3 is about changing it.
  - `arch` is the processor the program is built for: `x64` (almost every PC today), `x86`
    (32-bit programs) or `arm64`. rubrapack checks that `hello.exe` really is an x64 program.
  - `upgrade-code` is a GUID - a 128-bit number written as 32 hexadecimal digits in braces - that
    is unique to your product. Make your own with `rubrapack guid` (below), write it here once,
    and **never change it**: every later version must carry the same one, or Windows will treat it
    as a different product and install it next to the old one.
- `[dir.INSTALLDIR]` declares a folder with the ID `INSTALLDIR`. An ID is a name you choose
  (letters, digits and `_`) to refer to a thing from other tables. `path` says where the folder is:
  `$(ProgramFiles)/Hello` means `C:\Program Files\Hello` on an English or Korean Windows alike -
  `$(ProgramFiles)` names the Windows folder (its environment variable is `%ProgramFiles%`), and it
  is resolved on the user's computer.
- `[file.Hello]` installs one file, with the ID `Hello`. `dir` names the folder it goes into, by
  its ID; `source` is where the file is now, relative to `hello.toml`, written with `/`.

Get your own upgrade code - a new one every time you run it:

```text
C:\work\hello> rubrapack guid
{F1B1ED21-3061-4A65-8D77-FBB5ACCB1642}
```

Copy it over the one in the example. (Using the example's code would make your product and every
other reader's "the same product" to Windows.)

## Building

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi
```

Nothing is printed when all is well, and `hello.msi` (about 28 KB) appears next to the source. If
something is wrong, rubrapack says where and why and writes nothing, for example after a typo in
`source`:

```text
hello.toml:14:1: error[RP1507]: source file 'dist/hello.ex' not found
```

The numbers are the line and the column; the code in brackets (`RP1507`: RP15xx is "the files to
install") is explained under [Diagnostic codes](../rpk.md#diagnostic-codes) in the Reference part.
Fix the line and build again.

## Installing and removing it

Copy `hello.msi` to a Windows computer and double-click it. Because it installs into Program
Files, which belongs to all users, Windows asks for administrator permission (the User Account
Control prompt). Then a small window with a progress bar appears for a moment - this package has
no dialogs of its own yet (chapter 6 adds them). Afterwards:

- `C:\Program Files\Hello\hello.exe` exists;
- Settings > Apps > Installed apps lists **Hello**, version 1.0.0, by Example Software.

Remove it from Installed apps (the `...` menu, Uninstall), or from a terminal:

```text
msiexec /x hello.msi
```

The file and the folder are gone again. The same package can also be installed without any
window, which is how companies deploy software (`/qn` means "quiet, no user interface"; run the
terminal as administrator):

```text
msiexec /i hello.msi /qn
msiexec /x hello.msi /qn
```

When something goes wrong on Windows, a log tells why: `msiexec /i hello.msi /l*v install.log`
writes everything Windows Installer did into `install.log`.

## What happened inside

`rubrapack inspect` shows what the package holds without installing it. `--files` lists the files
and where they go:

```text
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello	C_185f8db32271fe25f561	1.2.3.4	1033
```

`ProgramFiles64Folder` is Windows Installer's name for 64-bit Program Files; `17920` is the size in
bytes; `1.2.3.4` and `1033` (US English) are the version and language written inside `hello.exe`
(Windows compares them when it replaces a file). An MSI is a database, and `inspect` can print any
of its tables; the `File` table has one row for our one file:

```text
C:\work\hello> rubrapack inspect hello.msi File
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	1
```

The first line names the columns, the second gives their types (`s72` is a string of up to 72
characters, `i4` a 4-byte integer, a capital letter means the column may be empty), the third
names the table and its key column. `Hello` is your file ID; `C_185f...` is a *component* that
rubrapack made for it; `512` means "vital" (the installation fails if this file cannot be
written). The `Property` table holds the product's identity:

```text
C:\work\hello> rubrapack inspect hello.msi Property
...
ProductCode	{98BFA9A2-2741-81F9-A1CA-2C6626AE1304}
ProductName	Hello
ProductVersion	1.0.0
UpgradeCode	{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}
...
```

The `ProductCode` is another GUID, which identifies *this version*: rubrapack derives it from the
upgrade code, the architecture and the version, so it changes by itself when the version does. You
do not write it. Part IV explains how these tables are stored in the file.

## Check it without building

`rubrapack lint hello.toml` runs every check the build runs and writes nothing - handy after an
edit. `rubrapack lint hello.msi` checks a finished package, even one made by another tool:

```text
C:\work\hello> rubrapack lint hello.msi
hello.msi: 0 errors, 0 warnings
```

## The same, with questions instead of a text editor

`rubrapack new hello.toml` asks for the name, the version, the folder with the files and so on, and
writes a source like the one above (it also makes up the upgrade code). Chapter 19 describes it and
its companion `rubrapack edit`. Writing the source by hand once, as here, is the best way to learn
what each line does.
