# The smallest package that installs, repairs, upgrades and uninstalls

Table meanings are documented on Microsoft Learn ("Database Tables", "Standard Actions",
"Suggested InstallExecuteSequence"). This page is a tested recipe: a package built exactly like
this installs per machine, survives repair, upgrades an older version, refuses a downgrade and
uninstalls cleanly on Windows 11. [observed]

## Tables and columns

Types use the notation of [msi-database.md](msi-database.md) (`s72` = `CHAR(72)`, upper case =
nullable, `l` = localizable, `i2`/`i4` integers). Keys are in bold.

| Table | Columns |
|---|---|
| Property | **Property** s72, Value l0 |
| Directory | **Directory** s72, Directory_Parent S72, DefaultDir l255 |
| Component | **Component** s72, ComponentId S38, Directory_ s72, Attributes i2, Condition S255, KeyPath S72 |
| Feature | **Feature** s38, Feature_Parent S38, Title L64, Description L255, Display I2, Level i2, Directory_ S72, Attributes i2 |
| FeatureComponents | **Feature_** s38, **Component_** s72 |
| File | **File** s72, Component_ s72, FileName l255, FileSize i4, Version S72, Language S20, Attributes I2, Sequence i4 |
| MsiFileHash | **File_** s72, Options i2, HashPart1..HashPart4 i4 |
| Media | **DiskId** i2, LastSequence i4, DiskPrompt L64, Cabinet S255, VolumeLabel S32, Source S72 |
| Upgrade | **UpgradeCode** s38, **VersionMin** S20, **VersionMax** S20, **Language** S255, **Attributes** i4, Remove S255, ActionProperty s72 |
| CustomAction | **Action** s72, Type i2, Source S72, Target S255 |
| InstallExecuteSequence, InstallUISequence | **Action** s72, Condition S255, Sequence I2 |

`File.Sequence` and `Media.LastSequence` as 32-bit integers let a package hold more than 32767
files.

## Property

`ProductCode`, `ProductName`, `ProductVersion` (`a.b.c`, a and b up to 255, c up to 65535; a
fourth part is allowed but ignored when versions are compared), `Manufacturer`, `ProductLanguage`
(1033, 1042, ...), `UpgradeCode`, `ALLUSERS=1` (per machine), `REBOOT=ReallySuppress`, and
`SecureCustomProperties` listing the two upgrade properties below.

## Directory

- `TARGETDIR` with no parent and `DefaultDir = SourceDir`.
- Each standard folder used (`ProgramFiles64Folder`, `ProgramFilesFolder`, ...) as a child of
  `TARGETDIR` with `DefaultDir = .`. Windows resolves these names itself. For an x64 or Arm64
  package use the `...64Folder` names, for x86 the plain ones.
- Your folders below them with `DefaultDir = SHORT|Long name`. The long name may be any Unicode
  text (with code page 65001). The short name must be a valid 8.3 ASCII name, unique among all
  files and folders of the same parent; when the long name already is a valid 8.3 name it can be
  written alone.

## Components, features, files

- **One component per file**, the file as its key path. `ComponentId` is a GUID that must stay the
  same for the same resource at the same place for the life of the product (see
  [identity.md](identity.md)). `Attributes` 256 (64-bit) for x64 and Arm64 packages, 0 for x86.
- A feature with `Level` 1; `Display` 0 hides it. Map every component in `FeatureComponents`.
- `File.FileName` is `SHORT|Long` like DefaultDir. `Attributes` 512 marks the file vital.
  `Sequence` numbers 1..n are the order of the files inside the cabinet.
- Files without a version resource need an **MsiFileHash** row: the MD5 of the file, read as four
  little-endian 32-bit integers (stored as signed values). Installed files hash with
  `MsiGetFileHash` to exactly these values. Files with a version resource put the version in
  `File.Version` instead (and need no hash).

## Media and the cabinet

One row: `DiskId 1`, `LastSequence n`, `Cabinet #cab1.cab` - the `#` means "a stream inside this
package named `cab1.cab`". The cabinet holds the files named by their `File` keys, in `Sequence`
order ([cab-mszip.md](cab-mszip.md)). Summary Word Count 2 says the files are compressed into
cabinets.

## Major upgrade and downgrade refusal

Two Upgrade rows with the package's `UpgradeCode`:

| VersionMin | VersionMax | Attributes | ActionProperty | Meaning |
|---|---|---|---|---|
| this version | - | `0x102` (only detect, min inclusive) | `NEWER_FOUND` | same or newer version installed |
| - | this version | `0x001` (migrate features) | `OLDER_FOUND` | older version installed: remove it |

An error custom action (type 19, `Target` = the message; it is a formatted string, so `[ProductName]`
works) conditioned on `NEWER_FOUND` stops the installation with error 1603 when a same-or-newer
version is present. Every version needs a new `ProductCode` (and every package file a new package
code).

## Sequences

InstallExecuteSequence (conditions in brackets):

```text
FindRelatedProducts 25, <refuse-downgrade action> 30 [NEWER_FOUND], CostInitialize 800,
FileCost 900, CostFinalize 1000, MigrateFeatureStates 1200, InstallValidate 1400,
InstallInitialize 1500, RemoveExistingProducts 1501, ProcessComponents 1600,
UnpublishFeatures 1800, RemoveFiles 3500, RemoveFolders 3600, CreateFolders 3700,
InstallFiles 4000, RegisterUser 6000, RegisterProduct 6100, PublishFeatures 6300,
PublishProduct 6400, InstallFinalize 6600
```

InstallUISequence: FindRelatedProducts 25, the refuse-downgrade action 30 [NEWER_FOUND],
CostInitialize 800, FileCost 900, CostFinalize 1000, MigrateFeatureStates 1200, ExecuteAction 1300.

`RemoveExistingProducts` right after `InstallInitialize` puts the removal of the old version
inside the new installation's transaction, so a failed upgrade rolls back to the old version.

## What this recipe does not cover yet

Registry, shortcuts, services, custom actions beyond the error type, dialogs, per-user and
dual-scope packages, administrative and advertised installation sequences, and `_Validation`
(needed by validation tools, not by the installer). These pages grow as rubrapack implements them.
