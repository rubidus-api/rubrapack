# Starting fast and automating

Goal: let rubrapack write a first source for you and change it without opening an editor, then
turn every step of this tutorial into a script that builds, checks and signs a release by itself -
on your computer, on a build server, or through an AI assistant.

## A source from answers: `new`

In a folder that has the program's files in `dist\`:

```text
C:\work\hello2> rubrapack new hello.toml
rubrapack new: a few questions make the source (Enter takes the value in [ ]).
Product name [hello]: Hello
Manufacturer (shown in Installed apps) [Hello authors]: Example Software
Version [1.0.0]:
Folder with the files to install [dist]:
  2 files and 2 folders there
Main program, for shortcuts (- for none) [hello.exe]:
Architecture (x64, x86, arm64) [x64]:
Folder name under Program Files [Hello]:
Install for (machine, user, dual) [machine]:
  sub folders: docs samples
Optional parts the user may tick (sub folders, commas; - for none) [-]: samples
Dialogs (none, basic, minimal, installdir, features) [features]:
License to accept (.txt, .md, .rtf; - for none) [LICENSE.txt]:
Korean dialogs as well as English? [y/N]: y
Start menu shortcut? [Y/n]:
Desktop shortcut? [y/N]: y
wrote hello.toml
the same without questions:
  rubrapack new "hello.toml" --name "Hello" --manufacturer "Example Software" --version "1.0.0" --arch "x64" --dist "dist" --main "hello.exe" --install-dir "Hello" --scope "machine" --ui "features" --license "LICENSE.txt" --languages "ko" --optional "samples" --shortcuts "start,desktop"
lint: no problems
```

Every question is something a chapter of this tutorial explained, and the value in `[ ]` is what
Enter takes. The offers follow the answers: after the name `Hello` the manufacturer offered is
`Hello authors`, and once there are optional parts the dialogs offered are `features`.

The source it writes is an ordinary source - with comments that say what each line does, a fresh
upgrade code, a `[file.*]` for each file at the top of the folder and a glob for each sub folder,
and a `[feature.*]` for each optional one:

```text
[feature.Main]
title = "Hello"
required = true                  # always installed

[feature.samples]
title = "samples"
level = 2                        # offered, not ticked by default
...
[files.samples_files]
dir = "samples_dir"
glob = "dist/samples/**"
```

From here on you change it by hand, as in chapters 2-18, or with `edit`. `new` never replaces a
file that is there: `new hello.toml` a second time says to use `edit`.

Three other forms:

- The line `new` prints last - `rubrapack new hello.toml --name ... --shortcuts ...` - gives the
  same answers without asking, for scripts. A left-out option takes the default the question would
  offer.
- `rubrapack new hello` (a name without `.toml`, and no options) writes a short starter source
  without asking anything, for filling in by hand.
- `rubrapack new hello -i` (or `--interactive`) asks the questions. They go to the error output and the answers are read
  one per line, so a file of answers can be piped in: `rubrapack new hello.toml < answers.txt`. If
  the answers run out before the last question, nothing is written.

## Changing a source: `edit`

`edit` without options shows a menu:

```text
C:\work\hello2> rubrapack edit hello.toml
hello.toml: "Hello" 1.0.0, x64, machine, dialogs features
  1 name, manufacturer, version, architecture   2 install folder, who it installs for
  3 dialogs, license, languages                  4 optional parts
  5 shortcuts                                    6 files: match the program folder
  7 any key                                      v view   l lint   s save   q quit
Choose [q]:
```

Each choice asks its questions again, offering the current values. `7` changes any key of any
table. `v` shows the source, `l` checks it, `s` saves and checks, `q` leaves (and asks first if
something changed). Only the values you changed are rewritten: your comments, your order, and the
tables you wrote by hand stay as they are.

With options, `edit` asks nothing:

```text
C:\work\hello2> rubrapack edit hello.toml --set define.VERSION=1.1.0 --set package.summary-name=Hello --unset package.license
wrote hello.toml
lint: no problems
```

- `--set table.key=value` sets a key. A value written as TOML - `"text"`, a number, `true`,
  `["ko"]` - is used as it is; anything else is taken as text. A table that is not there is added.
- `--unset table.key` removes a key.

After the program changed - here `readme.txt` is gone and `changes.txt` is new - `--sync` matches
the file list to the folder:

```text
C:\work\hello2> rubrapack edit hello.toml --sync
  removed [file.readme_txt]
  added [file.changes_txt]
wrote hello.toml
lint: no problems
```

## A release script

Everything so far is a command with no state outside the source, so a release is a short script.
A PowerShell script, `release.ps1`, next to `hello.toml`:

```text
param([Parameter(Mandatory)] [string] $Version)
$ErrorActionPreference = "Stop"
function Run { & rubrapack @args; if ($LASTEXITCODE -ne 0) { throw "rubrapack $args failed ($LASTEXITCODE)" } }

Run lint hello.toml --strict -D VERSION=$Version
foreach ($arch in "x64", "x86", "arm64") {
    Run build hello.toml -o "out\hello-$Version-$arch.msi" -D VERSION=$Version --arch $arch --reproducible `
        --key-store $env:SIGN_THUMBPRINT --timestamp http://timestamp.digicert.com
    Run lint "out\hello-$Version-$arch.msi" --strict --previous "released\hello-$arch.msi"
    Run verify "out\hello-$Version-$arch.msi" --system-roots
}
Run build hello.toml -o "out\hello-$Version.msixbundle" -D VERSION=$Version --arch x64,x86,arm64 `
    --key-store $env:SIGN_THUMBPRINT --timestamp http://timestamp.digicert.com
```

```text
PS C:\work\hello> .\release.ps1 -Version 2.2.0
```

`released\` holds the packages of the version before, one per architecture, so `--previous`
checks each upgrade (chapter 3). The script stops at the first command that fails, because
rubrapack's exit code is not 0 (chapter 18).
The thumbprint comes from an environment variable, so the script holds no secret and can be kept
with the source.

## On a build server

rubrapack runs on Linux too, and builds the same bytes there with `--reproducible`, so a
build server (GitHub Actions, GitLab CI, Jenkins) does not need Windows. On a Linux runner:

```text
V=0.13.0
curl -sLo rubrapack "https://github.com/rubidus-api/rubrapack/releases/download/v$V/rubrapack-$V-linux-x86_64"
chmod +x rubrapack
./rubrapack lint hello.toml --strict
./rubrapack build hello.toml -o "hello-$VERSION.msi" -D VERSION="$VERSION" --reproducible \
    --key signer.pfx --pass-env SIGN_PASS --timestamp http://timestamp.digicert.com
```

The key and its password come from the server's secret store (for example the repository's
*secrets* in GitHub Actions) into a file and an environment variable for this step only. Never put
them in the repository. A key on a hardware token or in a cloud signing service is used with
`--pkcs11` instead (chapter 16).

## With an AI assistant

The source is short plain text and every error message says where and why, so an AI coding
assistant can do all of the above: give it this manual (the book,
<https://rubidus-api.github.io/rubrapack/>) and say what to install. It can write the source, run
`rubrapack lint` until it is clean, build, check the result with `inspect` and `extract`, and
write the release script. Two things stay yours:

- keep the upgrade code of the first version in every later one (chapter 3);
- install the package on a real Windows once before you ship it, and try the upgrade from the
  previous version.

## Where to go from here

You have used every table and every option of rubrapack. From now on:

- **Part II, Reference**, lists every key of every table and every option in one place - the page
  to keep open while writing a source.
- **Part III, Background knowledge**, explains the ideas under all this - bits and bytes, text
  encodings, GUIDs, hashes, compression, signatures - from the beginning.
- **Part IV, File formats**, opens the files you built and shows every byte: the MSI's file system,
  its tables and strings, the cabinet, the MSIX's ZIP and block map, and the signature.
