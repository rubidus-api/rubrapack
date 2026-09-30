# MSIX packages and bundles

Goal: build Hello as an MSIX package too - the newer package format of Windows - from the same
source, then as one bundle that holds all three architectures.

## MSI or MSIX?

| | MSI (`.msi`) | MSIX (`.msix`) |
|---|---|---|
| Installs | anywhere the package says, and changes the computer as told | into a sealed folder Windows owns; files and registry the app writes are kept apart |
| Removal | as clean as the package makes it | always complete: nothing is left behind |
| Signature | recommended | **required**: Windows does not install an unsigned MSIX (except for testing, below) |
| Can do | everything in this tutorial | files, shortcuts, file types, registry, fonts, a command-line alias, a start-at-sign-in task |
| Cannot do | - | services, custom actions, environment variables, INI files, permissions, conditions, dialogs |

Many products ship both. rubrapack builds either from one source: the output's extension decides.

## The folder

Three logos go next to the source, in `assets\`: PNG pictures of exactly 150x150, 44x44 and 50x50
pixels. The Start menu and the Settings app show them. Without them the package gets plain
one-colour logos - give all three or none.

```text
C:\work\hello\
    assets\
        Square150x150.png
        Square44x44.png
        StoreLogo.png
    dist\
        x64\hello.exe
        x86\hello.exe
        arm64\hello.exe
        docs\guide.txt
    hello.toml
```

## The source

```toml
# tutorial 17: hello.toml
[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
upgrade-code-x86 = "{7D1C2B3A-4E5F-4061-9728-3A4B5C6D7E8F}"
upgrade-code-arm64 = "{0E9F8D7C-6B5A-4948-8372-6150F4E3D2C1}"

[define]
VERSION = "2.1.0"

[msix]
identity-name = "ExampleSoftware.Hello"
publisher = "CN=Example Software"
publisher-display-name = "Example Software"
min-version = "10.0.17763.0"

[msix-app.Hello]
executable = "Hello"
display-name = "Hello"
description = "Says hello."
logo-150 = "assets/Square150x150.png"
logo-44 = "assets/Square44x44.png"
store-logo = "assets/StoreLogo.png"

[msix-extension.Command]
kind = "alias"
alias = "hello.exe"

[msix-extension.AtSignIn]
kind = "startup-task"
display-name = "Hello"
enabled = false

[msix-extension.Web]
kind = "firewall"
direction = "in"
protocol = "tcp"
ports = "8080"
profile = "private"

[dir.INSTALLDIR]
path = "ProgramFiles/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/$(ARCH)/hello.exe"

[files.DocFiles]
dir = "INSTALLDIR"
glob = "dist/docs/**"

[shortcut.StartMenu]
dir = "Programs"
name = "Hello"
target = "file:Hello"

[assoc.HelloDoc]
extension = ".hello"
prog-id = "ExampleSoftware.HelloDocument"
description = "Hello document"
target = "file:Hello"

[registry.Greeting]
root = "HKMU"
key = "Software\\Example Software\\Hello"
name = "Greeting"
value = "Hello"

[env.HelloHome]
name = "HELLO_HOME"
value = "[INSTALLDIR]"
msi-only = true
```

## The package's identity: `[msix]`

| Key | Meaning |
|---|---|
| `identity-name` | the package's name for Windows, 3-50 characters of `A-Z a-z 0-9 . -`; by custom `Company.Product`. Like the upgrade code of an MSI it must never change: a new version with the same name and publisher replaces the old one. |
| `publisher` | the *subject* of the certificate you will sign with, written as Windows writes it (next section) |
| `publisher-display-name` | the publisher's name people see; the default is `[package] manufacturer` |
| `min-version` | the oldest Windows it installs on, as `10.0.<build>.0`; the default `10.0.17763.0` is Windows 10 version 1809 |

The version is `[package] version` with a fourth part: `2.1.0` becomes `2.1.0.0`.

## The application: `[msix-app.ID]`

An MSIX lists the *applications* it contains - the entries in the Start menu. `executable` names
the `[file.*]` that starts it; the folder of that file (here `INSTALLDIR`) becomes the package's
own folder. `display-name` and `description` default to the package's name. Several
`[msix-app.*]` tables give several entries.

## Extras: `[msix-extension.ID]`

- `kind = "alias"`: typing `hello.exe` in a console starts the application, from any folder - the
  MSIX way of putting a program on the `PATH`.
- `kind = "startup-task"`: the application starts when the user signs in, once it has been run
  once. `enabled = false` leaves it off until the user turns it on in Task Manager's Startup apps,
  which shows `display-name`. `task-id` names the task for Windows; the default is the table's ID
  (here `AtSignIn`, as the manifest below shows).

- `kind = "firewall"`: a Windows Firewall rule that exists while the package is installed - here
  incoming TCP connections on port 8080 to Hello, on private networks. `direction` is `in` or `out`,
  `protocol` `tcp` or `udp`, `ports` one port or a range like `8000-8100`, `profile` `all` (the
  default), `domain`, `private` or `public`; `file = "file:ID"` names another program of the
  package.

- `kind = "com-server"`, `"toast"` and `"context-menu"` register COM classes: a class your program
  or a DLL serves (`file`, `class = "{GUID}"`, `args`, `threading` for a DLL), the class Windows
  starts the program as when the user clicks one of its notifications, and an Explorer context
  menu verb for some file `types` (a DLL with the `verb`). The Reference ("MSIX only") has an
  example of each.

With several `[msix-app.*]` tables, `app = "Hello"` says which application an extension belongs to;
the default is the first. An MSI build leaves these tables out.

## What the other tables become

- `[shortcut.StartMenu]` in `Programs` to the application's executable *is* the application's
  Start menu entry. A shortcut on the `Desktop` also works, from `min-version = "10.0.19645.0"` on.
  Shortcuts elsewhere, with arguments or with an `icon`, are MSI only (`RP1613`).
- `[assoc.HelloDoc]` becomes a file type association of the application; the application's logo
  stands for the file type.
- `[registry.Greeting]` goes into the package's *virtual registry*: the application sees the value
  as if it were in the real registry, while the computer's registry stays untouched. Only keys
  under `Software` can go there (`RP1612`).
- A `[service.*]` (chapter 12) becomes a packaged service: the same name, start and account, from
  Windows 10 version 2004 on (`min-version = "10.0.19041.0"`); it goes when the package goes.
- `[env.HelloHome]`: an MSIX cannot set environment variables. Without `msi-only = true` the build
  stops:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --unsigned-test
hello.toml:72:1: error[RP1605]: [env.HelloHome] cannot go into an MSIX; add msi-only = true to build the MSIX without it
```

`msi-only = true` keeps the table in the MSI and leaves it out of the MSIX. rubrapack never drops
something silently: what an MSIX cannot hold is an error until you say what you want.

Features, properties, dialogs and `[arp]` concern Windows Installer only and are not used.
`rubrapack lint hello.toml --target msix` checks a source for MSIX without building it.

## Building for testing: `--unsigned-test`

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --unsigned-test
C:\work\hello> rubrapack build hello.toml -o hello.msi
```

Windows installs an unsigned MSIX only when the package says it is meant for testing and the
installer says it accepts that. `--unsigned-test` adds that mark (it changes the publisher, so the
package is a different package from the signed one). Install it in PowerShell, as administrator
because it contains a program:

```text
PS C:\work\hello> Add-AppxPackage -Path hello.msix -AllowUnsigned
PS C:\work\hello> hello.exe
PS C:\work\hello> Get-AppxPackage ExampleSoftware.Hello | Remove-AppxPackage
```

Such a package is for your own test computer only.

## Signing it

For people to install it, sign it (chapter 16) - the MSIX's `publisher` must be exactly the
certificate's subject. Otherwise signing stops and prints the subject to use:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msix --key signer.pfx --pass-env SIGN_PASS
rubrapack: error[RP0011]: 'hello.msix': the package's publisher "CN=Example Software" is not the certificate's subject "O=Example Software Ltd, CN=Example Software"; set [msix] publisher to it (without --unsigned-test)
```

Copy that subject into `publisher`. Windows writes a subject's parts from the last to the first, so
it differs from how some tools show it.

## All architectures in one file: a bundle

An output ending in `.msixbundle` builds the source once for each architecture of `--arch`, as in
chapter 15, and puts the packages in one file:

```text
C:\work\hello> rubrapack build hello.toml -o hello.msixbundle --arch x64,x86,arm64 --unsigned-test
C:\work\hello> rubrapack inspect hello.msixbundle --files
AppxMetadata\AppxBundleManifest.xml	1616	deflate
ExampleSoftware.Hello_2.1.0.0_x64.msix	11119	stored
ExampleSoftware.Hello_2.1.0.0_x86.msix	11208	stored
ExampleSoftware.Hello_2.1.0.0_arm64.msix	11119	stored
```

Windows installs the package for its own processor from it. With `--key` the bundle and each
package in it are signed.

## Compression

Files are compressed with deflate; pictures and other files that are compressed already are stored
as they are. `--msix-compress store` stores everything, which makes the package bigger (here
11119 to 30072 bytes) but quicker to open. An MSIX holds no date or time: the same source gives the
same bytes on every computer.

## What happened inside

An MSIX is a ZIP archive. Look at its list of files:

```text
C:\work\hello> rubrapack inspect hello.msix --files
hello.exe	17920	deflate
guide.txt	6	deflate
Assets\Square150x150.png	301	stored
Assets\Square44x44.png	111	stored
Assets\StoreLogo.png	117	stored
Registry.dat	8192	deflate
AppxManifest.xml	2960	deflate
```

`Registry.dat` is the virtual registry - a registry *hive* file. `AppxManifest.xml` describes the
package; `--manifest` prints it:

```text
C:\work\hello> rubrapack inspect hello.msix --manifest
...
  <Identity Name="ExampleSoftware.Hello" Publisher="CN=Example Software, OID.2.25.311729368913984317654407730594956997722=1" Version="2.1.0.0" ProcessorArchitecture="x64" />
...
    <Application Id="Hello" Executable="hello.exe" EntryPoint="Windows.FullTrustApplication">
...
          <uap3:FileTypeAssociation Name="examplesoftware.hellodocument" Parameters="&quot;%1&quot;">
...
          <desktop:StartupTask TaskId="AtSignIn" Enabled="false" DisplayName="Hello" />
...
            <desktop:ExecutionAlias Alias="hello.exe" />
...
    <rescap:Capability Name="runFullTrust" />
```

The `OID.2.25...=1` part of the publisher is the test mark `--unsigned-test` added.
`runFullTrust` means the program runs like any desktop program, not in the restricted sandbox of
store apps. Two more entries are in every package, though `--files` does not list them:
`AppxBlockMap.xml`, the hash of every 64 KB block of every file, which is what a signature covers,
and `[Content_Types].xml`, the type of each file. A signed package also has `AppxSignature.p7x`, the
signature. Part IV shows the ZIP layout and the block map byte by byte.
