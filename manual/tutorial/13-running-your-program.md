# Running your program during installation

Goal: after its files are in place, run `hello.exe --register` so that Hello can register itself
somewhere (with another program, a plug-in host, the system); before removal, run
`hello.exe --unregister`. And if anything fails, undo it all.

## Why not just run a program?

An installation is a transaction: if step 40 fails, Windows Installer undoes steps 1 to 39 and
the computer is as before. A program you run is outside that - Windows cannot know how to undo
what it did. So rubrapack asks you for *two* commands, one that does and one that undoes, and
arranges that the right one runs whichever way things go.

## The source

```toml
# tutorial 13: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"

[define]
VERSION = "1.11.0"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"

[action.Register]
run = "file:Hello"
do = "--register"
undo = "--unregister"
```

## `[action.ID]`

| Key | Meaning |
|---|---|
| `run` | the program: `file:` and the ID of an `.exe` this package installs |
| `do` | its arguments after installation - and again on a repair |
| `undo` | its arguments at removal, before the files are removed |
| `check` | has no effect yet: rubrapack warns (`RP1318`); leave it out |

The arguments are passed as written: they are *not* formatted strings, so `[INSTALLDIR]` would
arrive as the text `[INSTALLDIR]`. The program knows where it is (its own path), which is usually
all it needs.

Both commands:

- must finish without asking anything: they run without a window, and nobody is there to answer;
- must be safe to run twice (register when already registered: fine);
- report failure by exiting with a non-zero code - which fails and rolls back the installation.

In a per-machine package they run with the installer's administrator rights; in a per-user package,
as the user.

## What happens when

| Situation | Runs |
|---|---|
| first installation | `do` after the files are installed |
| repair | `do` again |
| removal | `undo`, then the files go |
| upgrade to a new version | the old version's `undo` (its removal), then the new version's `do` |
| installation fails after `do` ran | `undo`, as part of the rollback |
| removal fails after `undo` ran | `do`, and the files come back |

## Try it

A real program for this reads its arguments and, for example, writes a registry value or calls a
registration interface. To watch the order, make `hello.exe` write its arguments into a log file,
install, repair and remove, and read the log.

## What happened inside

```text
C:\work\hello> rubrapack inspect hello.msi CustomAction
RP_Register_Do	3090	Hello	--register
RP_Register_DoRollback	3410	Hello	--unregister
RP_Register_RedoRollback	3410	Hello	--register
RP_Register_Undo	3090	Hello	--unregister
RP_Register_UndoRollback	3410	Hello	--register
```

Five *custom actions*. The type numbers are bit flags added together: 18 ("run an `.exe`
installed by this package") + 1024 ("deferred": runs in the installation script, in order with
the file copying) + 2048 ("no impersonation": with the installer's rights) = 3090; the rollback
twins add 256 ("run only while rolling back") and 64 ("ignore the exit code"). The
`InstallExecuteSequence` places them around the file actions, each with a condition on the state
of `hello.exe`'s component:

```text
RP_Register_UndoRollback	$C_185f...=2 AND ?C_185f...=3	3400
RP_Register_Undo	$C_185f...=2 AND ?C_185f...=3	3401
RemoveFiles		3500
InstallFiles		4000
RP_Register_DoRollback	$C_185f...>2 AND ?C_185f...<>3	4001
RP_Register_RedoRollback	$C_185f...>2 AND ?C_185f...=3	4002
RP_Register_Do	$C_185f...>2	4003
```

`$C` is the state the component is going to (2 absent, 3 installed), `?C` the state it is in now.
"Going away while installed" runs undo; "coming" runs do.
