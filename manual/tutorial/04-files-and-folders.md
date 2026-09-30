# More files and folders

Goal: install a whole program folder - the program, a readme under another name, a settings file
the user may change, a folder of documents, samples in sub folders, an empty folder for data - and
clean up the log files the program writes.

## The folder

```text
C:\work\hello\
    dist\
        hello.exe
        readme.txt
        settings.ini
        docs\
            guide.txt
        samples\
            sample1.txt
            sub\
                sample2.txt
    hello.toml
```

## The source

```toml
# tutorial 04: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.2.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[dir.DocsDir]
path = "INSTALLDIR/docs"

[dir.SamplesDir]
path = "INSTALLDIR/samples"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[file.Readme]
dir = "INSTALLDIR"
source = "dist/readme.txt"
name = "Read me.txt"
vital = false

[file.Settings]
dir = "INSTALLDIR"
source = "dist/settings.ini"
keep = true

[files.DocFiles]
dir = "DocsDir"
glob = "dist/docs/**"

[files.SampleFiles]
dir = "SamplesDir"
glob = "dist/samples/**/*.txt"

[folder.Data]
dir = "INSTALLDIR"
name = "data"
keep = true

[remove.Logs]
dir = "INSTALLDIR"
name = "*.log"
on = "uninstall"

[copy.ReadmeInDocs]
source = "file:Readme"
dir = "DocsDir"
name = "readme.txt"
```

Build it and look at the files:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msi
C:\work\hello> rubrapack inspect hello.msi --files
[ProgramFiles64Folder]\Hello\Read me.txt	8	Readme
[ProgramFiles64Folder]\Hello\docs\guide.txt	6	F_6d1508f52f6615459567
[ProgramFiles64Folder]\Hello\hello.exe	17920	Hello
[ProgramFiles64Folder]\Hello\samples\sample1.txt	7	F_1dc2b5e85973ee91d955
[ProgramFiles64Folder]\Hello\samples\sub\sample2.txt	7	F_c9c5c4da73214a6abf37
[ProgramFiles64Folder]\Hello\settings.ini	23	Settings
```

(The output has more columns; only the path, the size and the ID are shown here.)

## Folders: `[dir.ID]`

A dir's `path` starts from a *known folder* or from another dir, then names the folders below it:

| `path` | On the user's computer (typical) |
|---|---|
| `ProgramFiles/Hello` | `C:\Program Files\Hello` (for an `x86` package: `C:\Program Files (x86)\Hello`) |
| `INSTALLDIR/docs` | `C:\Program Files\Hello\docs` - below the dir `INSTALLDIR`, wherever the user put it |
| `CommonAppData/Hello` | `C:\ProgramData\Hello` - data shared by all users |
| `LocalAppData/Hello` | `C:\Users\<name>\AppData\Local\Hello` |

Building on `INSTALLDIR` rather than repeating `ProgramFiles/Hello` matters once users may choose
the install folder (chapter 6): `docs` then follows the folder they chose. The full list of known
folders is in the Reference part ([Tables](../rpk.md#tables)).

## One file: `[file.ID]`

- `name` installs the file under another name - here `readme.txt` becomes `Read me.txt`. Names
  may use any language's letters; what Windows forbids in file names (`< > : " / \ | ? *`) is
  refused.
- `vital = false`: if this file cannot be written, the installation goes on without it. By default
  every file is vital.
- `keep = true`: the file stays when the product is removed, and a repair or a later version does
  not overwrite it once the user has changed it. Use it for settings the user edits.

## Many files: `[files.ID]`

`glob` is a path with wildcards: `*` is any characters within one folder name, `?` one character,
`**` any number of folders. Every file that matches is installed, and **the folders below the first
wildcard are recreated** under `dir`:

| `glob` | `dist\samples\sub\sample2.txt` goes to |
|---|---|
| `dist/samples/**/*.txt` into `SamplesDir` | `...\Hello\samples\sub\sample2.txt` |
| `dist/samples/*.txt` into `SamplesDir` | not installed: `*` does not go into `sub` |

The matches are sorted by name, so the package is the same whatever order the file system lists
them in. A glob that matches nothing stops the build (`RP1503`) - usually a typo in the path.
rubrapack gives each matched file an ID of its own (`F_` and a number derived from its path).

## An empty folder: `[folder.ID]`

`[folder.Data]` creates `data` inside `INSTALLDIR` although no file goes there - a place for the
program to write. `keep = true` leaves it (and whatever the program wrote in it) when the product
is removed.

## Cleaning up: `[remove.ID]`

Files the program creates itself - logs, caches - are not the installer's, so Windows Installer
would leave them, and with them the folder. `[remove.Logs]` deletes `*.log` in `INSTALLDIR` when the
product is removed (`on = "uninstall"`). `on = "install"` deletes at installation (files an earlier
version left behind), `on = "both"` at either. Without `name`, the folder itself is removed if it is
empty. Folders that existed before the installation are never removed, and a failed installation
puts removed files back.

## A second copy: `[copy.ID]`

`[copy.ReadmeInDocs]` installs another copy of the file `Readme` into `DocsDir` as `readme.txt`. The
copy comes and goes with its source.

## Two more keys you may meet

- `any-arch = true` on a file: rubrapack checks that every program file (`.exe`, `.dll`) is built for
  the package's `arch`, because an x64 installer that copies a 32-bit DLL by mistake installs a
  broken program. When that is intended - a 32-bit helper in an x64 product - say so with
  `any-arch = true`.
- `component-guid = "{...}"` on a file: rubrapack derives every component's GUID from the product and
  the file's place, so it stays the same from version to version, which upgrades rely on. Give one
  by hand only to continue a component that an earlier package made with a different GUID.

## What happened inside

Windows Installer installs *components*: small groups of resources that are installed and removed
together, each with a GUID and a *key path* (the thing whose presence means "installed").
rubrapack makes one component per file, which is the safe rule, and one more for each folder,
removal and copy that needs one:

```text
C:\work\hello> rubrapack inspect hello.msi Component
...
C_74a883a037bc227f9189	{51A68DD4-96FA-83C0-86AA-F20D51339238}	INSTALLDIR	272		Settings
C_cec3a9b89b2e391393d0	{615660C7-986E-8FB8-87AF-0616B544A6F9}	Data	272
...
```

The `272` in the attributes column is `256 + 16`: 256 means a 64-bit component, 16 "permanent" -
`keep = true`. Part IV describes these tables; `RemoveFile`, `CreateFolder` and `DuplicateFile`
hold the removal, the empty folder and the copy.
