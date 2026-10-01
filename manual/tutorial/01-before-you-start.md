# Before you start

This part of the manual is a tutorial. It starts with the smallest installer there is - one program
copied into Program Files - and adds one thing per chapter until every feature of rubrapack has
been used once. Each chapter builds on the one before, so it is best read in order. Nothing is
assumed beyond using a computer: where a chapter needs an idea from outside (a GUID, a hash, a
registry key), it explains it on the spot or points to the Background knowledge part.

## What an installer does

A program on your own computer is just files in a folder. To give it to someone else, you could
send them the folder - but then they have to choose where to put it, make their own Start menu
entry, and later find and delete everything by hand. An *installer* does this for them:

- it copies the files to the right place (usually `C:\Program Files\<your program>`);
- it creates shortcuts, registry values, file type links and whatever else the program needs;
- it registers the program in Settings > Apps > Installed apps, so it can be removed there;
- when you ship version 2, it replaces version 1 cleanly;
- if something goes wrong halfway, it puts everything back as it was.

Windows has a built-in engine for this, **Windows Installer**. You do not write a program that
installs; you write a *package*, a file that describes what to install, and Windows Installer
carries it out. Two package formats exist:

| Format | File | Installed by | Typical use |
|---|---|---|---|
| Windows Installer | `.msi` | Windows Installer (`msiexec`), since Windows 2000 | desktop programs of every kind; companies deploy MSI through their management tools |
| MSIX | `.msix`, `.msixbundle` | the App Installer of Windows 10 and 11 | modern packaged apps, the Microsoft Store |

rubrapack makes both, from the same description. Most of this tutorial is about MSI; chapter 17
turns the same example into an MSIX.

## What you write, what rubrapack makes

You write one short text file, the *source*, in [TOML](https://toml.io/en/v1.0.0) - a simple
`key = value` format. rubrapack reads it and writes the package:

```text
  hello.toml  (you write this)
      |
      |   rubrapack build hello.toml -o hello.msi
      v
  hello.msi   (you give this to users)
      |
      |   double-click, or: msiexec /i hello.msi
      v
  C:\Program Files\Hello\hello.exe   (Windows Installer puts it there)
```

An MSI file is itself a small database with dozens of tables, identifiers and a compressed archive
inside - Part IV of this book shows it byte by byte. You never have to write those tables:
rubrapack works them out from the source.

## Getting rubrapack

rubrapack is one program file with nothing else to install. From the releases page,
<https://github.com/rubidus-api/rubrapack/releases>, download:

- on Windows: `rubrapack-<version>-windows-x64.exe`. Rename it to `rubrapack.exe`.
- on Linux: `rubrapack-<version>-linux-x86_64`. Rename it to `rubrapack` and make it executable:
  `chmod +x rubrapack`.

Put it in a folder that is on your `PATH` (so the command works from anywhere), or keep it in the
folder you work in and type `.\rubrapack.exe` (Windows) or `./rubrapack` (Linux) instead of
`rubrapack`. This tutorial writes plain `rubrapack`.

You can build packages on Linux, but you need Windows to install and try them. A virtual machine
with Windows is enough.

## A terminal, for those who have not used one

rubrapack has no window: you type commands. On Windows, press the Windows key, type `terminal`
(or `cmd`), and press Enter. A window with a prompt opens, for example:

```text
C:\Users\you>
```

The part before `>` is the *current folder*. Commands work on files in that folder unless you give
a path. Three commands are all you need:

| Command | What it does |
|---|---|
| `cd C:\work\hello` | go into the folder `C:\work\hello` |
| `dir` (Linux: `ls`) | list the files in the current folder |
| `mkdir dist` | make a folder named `dist` here |

Try it: `rubrapack version` prints the version, and `rubrapack help` lists the commands.

```text
C:\work\hello> rubrapack version
rubrapack 0.23.0 (proven_c_lib-v0.1.1)
```

## The folder you work in

Every chapter uses the same layout. Make a folder for your project, and inside it a folder `dist`
("distribution") holding exactly the files you ship:

```text
C:\work\hello\
    dist\
        hello.exe        the program - any program of your own will do
    hello.toml           the source (the next chapter writes it)
```

If you have no program at hand, any `.exe` works for learning; the installer does not run it.

## Words used in this tutorial

| Word | Meaning |
|---|---|
| package | the `.msi` (or `.msix`) file you give to users |
| source | the `.toml` file you write; rubrapack builds the package from it |
| install folder | where the files go on the user's computer, e.g. `C:\Program Files\Hello` |
| product | your program as Windows sees it: a name, a version, a manufacturer |
| upgrade code | a fixed identifier that ties every version of your product together (chapter 2) |
| feature | a part of the product the user may choose to install or not (chapter 8) |
| component | Windows Installer's unit of installation; rubrapack makes these for you |
