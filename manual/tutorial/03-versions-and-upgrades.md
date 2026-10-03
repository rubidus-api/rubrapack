# Versions and upgrades

Goal: ship version 1.1.0 of Hello so that installing it replaces 1.0.0, and installing 1.0.0 again
afterwards is refused.

## One source for every version

Editing `version` by hand before each release works, but it is easy to forget. Instead, let the
version be a *variable* with a default, and give the real number when you build:

```toml
# tutorial 03: hello.toml
format = 1

[package]
name = "Hello"
manufacturer = "Example Software"
version = "$(VERSION)"
arch = "x64"
upgrade-code = "{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}"
downgrade-message = "A newer Hello is already installed. Remove it first if you want this version."

[define]
VERSION = "1.0.0"

[dir.INSTALLDIR]
path = "$(ProgramFiles)/Hello"

[file.Hello]
dir = "INSTALLDIR"
source = "dist/hello.exe"
```

- `[define]` holds variables. `$(VERSION)` anywhere in a string is replaced by the variable's
  value; here the default is `1.0.0`.
- `-D NAME=VALUE` on the command line overrides a variable for one build:

```text
C:\work\hello> rubrapack build hello.toml -o hello-1.0.0.msi
C:\work\hello> rubrapack build hello.toml -o hello-1.1.0.msi -D VERSION=1.1.0
```

- `downgrade-message` is the message a user sees when they try to install an older (or the same)
  version over a newer one. Without it, rubrapack uses "The same or a newer version of [ProductName]
  is already installed." (`[ProductName]` is replaced by the name).

## What Windows does with two versions

Install `hello-1.0.0.msi`, then `hello-1.1.0.msi`. Windows Installer sees that the new package has
the same upgrade code and a higher version, removes 1.0.0 and installs 1.1.0 - one entry in
Installed apps, now showing 1.1.0. This is called a *major upgrade*, and it is how rubrapack's packages
upgrade: every new version is a complete package, and nobody needs the old one to install the new
one. (A patch, [below](#small-fixes-as-a-patch-patch), updates an installed version in place
instead.)

Now try to install `hello-1.0.0.msi` again: Windows shows the downgrade message and changes
nothing. Running `hello-1.1.0.msi` a second time is not an error either: it has the same product
code as the installed version (see below), so Windows recognises the product as installed and
does not install it twice. To repair an installed version - put back a deleted file, for example -
run `msiexec /fa hello-1.1.0.msi`.

A package rebuilt from changed files *without* raising the version has the same product code as
the installed one but is not the same package, and Windows refuses it with its own message:
"Another version of this product is already installed" (error 1638). So raise the version
whenever the files change - only a higher version replaces what is installed.

How rubrapack arranges this is visible in the `Upgrade` table:

```text
C:\work\hello> rubrapack inspect hello-1.1.0.msi Upgrade
UpgradeCode	VersionMin	VersionMax	Language	Attributes	Remove	ActionProperty
s38	S20	S20	S255	i4	S255	s72
Upgrade	UpgradeCode	VersionMin	VersionMax	Language	Attributes
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}		1.1.0		1		RP_OLDER_FOUND
{3F2A6C1D-8B4E-4F7A-9C2D-5E6F7A8B9C0D}	1.1.0			258		RP_NEWER_FOUND
```

The first row finds installed versions below 1.1.0 (to remove them); the second finds 1.1.0 or
higher, which stops the installation with the downgrade message. The attribute numbers are bit
flags - `258` is `256 + 2`, "the minimum is included" plus "only detect, do not remove". [Bit flags](../basics/02-numbers-and-flags.md#bit-flags) in Part III explains them, [the tables that install](../formats/msi-package.md) in Part IV this table.

## Version numbers

- Three or four numbers: `major.minor.build` or `major.minor.build.revision`. Windows Installer
  allows at most `255.255.65535` for the first three.
- **Windows compares only the first three.** `1.0.0.1` and `1.0.0.2` are the same version to it: the
  second would be refused as "the same". Raise the third number (or a higher one) for every
  release.
- A wrong version stops the build:

```text
hello.toml:5:1: error[RP1308]: version '1.0' must be a.b.c or a.b.c.d with a, b <= 255 and c, d <= 65535
```

## Two identifiers

| | Upgrade code | Product code |
|---|---|---|
| identifies | the product across all its versions | one version |
| you write it | once, in `upgrade-code`; never change it | no: rubrapack derives it |
| changes | never | whenever the version (first three numbers) changes |

```text
1.0.0: ProductCode {98BFA9A2-2741-81F9-A1CA-2C6626AE1304}
1.1.0: ProductCode {9CC17977-2023-8B8F-8BB0-2FA82C5B248F}
```

The derived product code is the same every time you build the same version, on any computer.
`product-code` in `[package]` sets it by hand - only needed to continue a product whose earlier
versions came from another tool with a product code of their own.

## Check the upgrade before users do

Some mistakes do not stop a build but break the upgrade on users' computers: a changed upgrade
code, a version that is not higher, a file that moved in a way Windows cannot follow. Compare the
new package with the one before it:

```text
C:\work\hello> rubrapack lint hello-1.1.0.msi --previous hello-1.0.0.msi
hello-1.1.0.msi: 0 errors, 0 warnings (against hello-1.0.0.msi)
```

and the same package against itself, to see what a mistake looks like:

```text
C:\work\hello> rubrapack lint hello-1.0.0.msi --previous hello-1.0.0.msi
hello-1.0.0.msi: error[RP2302]: ProductVersion 1.0.0 is not higher than the previous 1.0.0 in its first three fields: Windows Installer does not upgrade
hello-1.0.0.msi: warning[RP2303]: the ProductCode is the previous version's: that is a minor upgrade (REINSTALLMODE), not the major upgrade rubrapack builds
hello-1.0.0.msi: 1 error, 1 warning (against hello-1.0.0.msi)
```

Keep every released `.msi` (for example in a `releases` folder) so the next one can be checked
against it.

## Small fixes as a patch: `patch`

A patch (`.msp`) updates an installed version in place: it carries only the files that changed,
whole, and the table rows that differ, and Windows applies it without removing anything. A patch
joins two builds of the *same product*, so they need the same product code - which rubrapack
otherwise derives from the version:

```text
C:\work\hello> rubrapack patch hello-1.0.0.msi hello-1.1.0.msi -o hello-1.1.0.msp
rubrapack: error[RP0013]: ProductCode differs: that is a major upgrade, which a patch does not carry
```

For the releases a patch will join, fix the product code in `[package]` (`rubrapack guid` prints a
new one) and raise only the third number:

```toml
product-code = "{5B3E2A71-9C4D-4E8F-A1B2-C3D4E5F60718}"
```

```text
C:\work\hello> rubrapack build hello.toml -o hello-1.1.0.msi -D VERSION=1.1.0
C:\work\hello> rubrapack build hello.toml -o hello-1.1.1.msi -D VERSION=1.1.1
C:\work\hello> rubrapack patch hello-1.1.0.msi hello-1.1.1.msi -o hello-1.1.1.msp
C:\work\hello> msiexec /p hello-1.1.1.msp
```

Computers with 1.1.0 installed apply the patch; new ones install `hello-1.1.1.msi`. A patch can be
removed on its own, which brings the files and values of 1.1.0 back, unless it was made with
`--no-removal`. Each patch has a patch code, derived from the two packages unless `--patch-code`
gives one; `--family` names the line of patches it belongs to (the product name by default).
`inspect hello-1.1.1.msp --base hello-1.1.0.msi` lists what the patch changes. A patch cannot
remove files or components, change the product or upgrade code, or change the first two version
numbers (`RP0013`); `sign` does not sign patches yet.

## Earlier versions made by another tool

If your product was installed before with packages from another tool, rubrapack's upgrade
replaces them as long as they have the same upgrade code. Two keys help when that goes badly:

- `refuse-upgrade-below = "0.60.0"`: versions below this one are not upgraded but refused, with a
  message that tells the user how to remove them first - for old packages known to break when
  removed as part of an upgrade.
- `refuse-upgrade-message = "..."`: that message's first part (the removal command is added).

## Add-ons that go with the product

A language pack or a plug-in shipped as a package of its own has its own upgrade code and versions,
and Windows lists it as an app of its own. To remove it together with the product, the add-on names
the product's upgrade code and the product asks for its add-ons to go with it:

```toml
# hello-pack.toml
[package]
parent = "{6F1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C60}"   # hello's upgrade-code

# hello.toml
[package]
remove-addons = true
```

Removing Hello then removes the pack a few seconds later; upgrading Hello keeps it. See
"Add-ons removed with their product" in the reference.
