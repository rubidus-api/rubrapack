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
| CreateFolder | **Directory_** s72, **Component_** s72 |
| Media | **DiskId** i2, LastSequence i4, DiskPrompt L64, Cabinet S255, VolumeLabel S32, Source S72 |
| Upgrade | **UpgradeCode** s38, **VersionMin** S20, **VersionMax** S20, **Language** S255, **Attributes** i4, Remove S255, ActionProperty s72 |
| CustomAction | **Action** s72, Type i2, Source S72, Target S255 |
| InstallExecuteSequence, InstallUISequence | **Action** s72, Condition S255, Sequence I2 |

`File.Sequence` and `Media.LastSequence` as 32-bit integers let a package hold more than 32767
files.

## Property

`ProductCode`, `ProductName`, `ProductVersion` (`a.b.c`, a and b up to 255, c up to 65535; a
fourth part is allowed but ignored when versions are compared), `Manufacturer`, `ProductLanguage`
(1033, 1042, ...), `UpgradeCode`, `ALLUSERS=1` (per machine), `REBOOT=ReallySuppress`,
`MSIRESTARTMANAGERCONTROL=Disable` (see "Files in use" below), and `SecureCustomProperties` listing
the two upgrade properties below.

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
  A child feature names its parent in `Feature_Parent`. A feature whose `Level` is above
  `INSTALLLEVEL` (1 unless the package sets it) is not installed by default; `MsiConfigureFeature`
  can add it later, and its files then arrive. [observed]
- `File.FileName` is `SHORT|Long` like DefaultDir. `Attributes` 512 marks the file vital.
  `Sequence` numbers 1..n are the order of the files inside the cabinet.
- Files without a version resource need an **MsiFileHash** row: the MD5 of the file, read as four
  little-endian 32-bit integers (stored as signed values). Installed files hash with
  `MsiGetFileHash` to exactly these values. Files with a version resource put the version in
  `File.Version` instead (and need no hash).

## Empty folders

A folder that must exist without files gets its own Directory row, a component whose `Directory_`
is that folder and whose `KeyPath` is null (the folder itself is the key path), a
`CreateFolder` row pointing at both, and a `FeatureComponents` row. `CreateFolders` makes it at
install time; `RemoveFolders` removes it at uninstall if it is empty. [observed] With component
attribute 16 (permanent) the folder stays after uninstall while everything else is removed.
[observed]

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

The Upgrade table matches by UpgradeCode, version and language only - not by architecture. If an
x64 and an x86 build share one UpgradeCode, installing one removes the other as "older". Give each
architecture its own UpgradeCode when both may be installed side by side (verified: two
architectures with separate codes install and uninstall independently). [observed]

An error custom action (type 19, `Target` = the message; it is a formatted string, so `[ProductName]`
works) conditioned on `NEWER_FOUND` stops the installation with error 1603 when a same-or-newer
version is present. Every version needs a new `ProductCode` (and every package file a new package
code).

## Running an installed program to register and unregister

Some products must register themselves (an input method, a shell extension) through their own
program rather than through table rows. The pattern that keeps install, repair, upgrade and removal
atomic uses the component action state of that program's component `C` (`$C` = what this
installation does to it, `?C` = its state before; 2 absent, 3 local) [spec], with type 18 actions
(`Source` = the program's File key, `Target` = its arguments), all deferred (0x400) and not
impersonated (0x800):

| Action | Type | Where | Condition | Runs |
|---|---|---|---|---|
| `…UndoRollback` | 18+0x100+0x400+0x800+0x40 | before `RemoveFiles` | `$C=2 AND ?C=3` | register (rollback of the next row) |
| `…Undo` | 18+0x400+0x800 | after it, before `RemoveFiles` | `$C=2 AND ?C=3` | unregister |
| `…DoRollback` | 18+0x100+0x400+0x800+0x40 | after `InstallFiles` | `$C>2 AND ?C<>3` | unregister |
| `…RedoRollback` | same | after it | `$C>2 AND ?C=3` | register |
| `…Do` | 18+0x400+0x800 | after both | `$C>2` | register |

A rollback action must be sequenced *before* the action it undoes: rollback runs the script
backwards, and only rollback actions already in the script are run. Rollback actions ignore their
exit code (0x40); forward actions do not, so a failing register or unregister fails the
installation and rolls it back. Observed on Windows 11: repair runs `…Do` again; a failing
unregister rolls the removal back and the program (still present) registers again; when an upgrade
fails, the new package's `…DoRollback` runs, then the old version's files return and the old
package's own `…UndoRollback` registers the old program again. [observed]

**Nothing that writes to the script may stand between `InstallInitialize` and
`RemoveExistingProducts`**: with any deferred or rollback custom action there, every upgrade stops
with error 2613 ("RemoveExistingProducts action sequenced incorrectly"). [observed]

## Registry values

`Registry` (**Registry** s72, Root i2, Key l255, Name L255, Value L0, Component_ s72) is written by
`WriteRegistryValues` (5000) and undone at uninstall; `RemoveRegistry` (same columns without Value)
by `RemoveRegistryValues` (2600). Root 0 = HKCR, 1 = HKCU, 2 = HKLM. The value's first characters
choose its type: `#x` binary (hex), `#%` expandable string, `#` followed by digits a DWORD (up to
`#4294967295`), `[~]` anywhere a multi-string (`[~]a[~]b[~]`); a plain string that starts with `#`
is written as `##`. A Name of `-` in RemoveRegistry deletes the whole key. A failed installation
restores overwritten values and removed keys. [observed]

A component whose key path is a registry value has attribute 4 and `KeyPath` = the Registry row.
The component's 64-bit attribute (256) chooses the registry view: without it, a 64-bit package
writes to the 32-bit view (`WOW6432Node`). A 32-bit component in a 64-bit package is legal; a
64-bit component in a 32-bit package is not.

REG_QWORD cannot be expressed in the Registry table. rubrapack writes it with a DLL custom action
from the `Binary` table (type 1): an immediate action reads the component action states and the
current values and passes two lists as `CustomActionData` - one to a deferred action that writes
or deletes, one to its rollback twin that restores what was there. The DLL's bitness must match
the package (the engine loads it into a custom action server of the package's architecture).
[observed: x64 and x86]

File types and URL schemes are plain `Registry` rows under Root 0 (HKCR) in the program's
component, not the `Extension`/`Verb`/`ProgId` tables (those bring advertisement and repair on
first use): `.ext` (default) = the ProgId; `ProgId` (default) = its description,
`ProgId\DefaultIcon` = `[#File],0`, `ProgId\shell\open\command` = `"[#File]" "%1"`; for a scheme,
`scheme` (default) = `URL:<description>`, `URL Protocol` = empty (a Null Value with a Name writes an
empty string), and the same `DefaultIcon` and `shell\open\command`. Root 0 follows the
installation: HKLM\Software\Classes per machine, HKCU\Software\Classes per user. Opening a file
of a type only this program claims then starts it directly. [observed]

## Shortcuts

`Shortcut` (**Shortcut** s72, Directory_ s72, Name l128 `SHORT|Long` without `.lnk`, Component_ s72,
Target a formatted path for a non-advertised shortcut (`[#FileKey]` or `[DirKey]name`), Arguments (formatted), Description (plain text),
Hotkey, Icon_, IconIndex, ShowCmd, WkDir = a Directory key) is written by `CreateShortcuts` (4500)
and removed by `RemoveShortcuts` (3200). A shortcut in its target file's component works, but Microsoft's ICE43/ICE57 want each
non-advertised shortcut in a component keyed by an `HKCU` value (see [verify.md](verify.md)); rubrapack
gives every shortcut such a component and targets `[DirKey]name` (ICE69). A folder
made only for shortcuts is not removed by itself: add a `RemoveFile` row (FileName null,
DirProperty = the folder, InstallMode 2) for it and each parent below the standard folder. With
`ALLUSERS=1`, `ProgramMenuFolder` and `DesktopFolder` are the all-users Start menu and the Public
Desktop. A failed installation leaves no shortcut or folder; repair recreates a deleted one.
[observed]

## Removing files and duplicating them

`RemoveFile` (**FileKey** s72, Component_ s72, FileName L255 with `*`/`?` or null for the folder,
DirProperty s72, InstallMode i2: 1 install, 2 uninstall, 3 both) runs in `RemoveFiles` when its
component is installed (1) or removed (2). A component with no key path file and a `Directory_`
works as the carrier. `DuplicateFile` (**FileKey** s72, Component_ s72 = the source's component,
File_ s72, DestName L255 `SHORT|Long`, DestFolder S72) runs in `DuplicateFiles` (4210) and
`RemoveDuplicateFiles`. Files removed at install come back when the installation fails; a folder
that existed before the installation is left in place at uninstall, even when empty. [observed]

## Environment variables

`Environment` (**Environment** s72, Name l255, Value L255 formatted, Component_ s72), written by
`WriteEnvironmentStrings` (5200) and `RemoveEnvironmentStrings`. Name prefixes: `*` system
variable (without it a per-user one), `=` create or set, `-` undo when the component is removed.
Value `[~];x` appends `;x`, `x;[~]` prepends. With `-`, uninstall deletes a set variable and takes
only the appended or prepended part out of a variable that existed before; without `-` nothing is
undone. A failed installation restores the previous values. [observed]

## INI files

`IniFile` (**IniFile** s72, FileName l255 `SHORT|Long`, DirProperty S72, Section l96, Key l128, Value
l255 formatted, Action i2: 0 add line, 1 create line, 3 add tag, Component_ s72) is applied by
`WriteIniValues` (5100) and undone when the component is removed; `RemoveIniFile` (same columns,
Value nullable, Action 2 remove line, 4 remove tag) by `RemoveIniValues`, which runs before
`InstallFiles`. An add tag appends `,value` to a comma list and uninstall removes just that item.
Non-ASCII file, section, key and value names work. A failed installation restores the file.
[observed]

## Searches and launch conditions

`AppSearch` (**Property**, **Signature_**) at 50 in both sequences sets the property from a locator
with the same signature: `RegLocator` (Root, Key, Name, Type 2 = raw value, +16 = 64-bit view),
`DrLocator` (Path may start with a folder property, like `[System64Folder]`) with a `Signature` row
for a file (FileName, MinVersion), or `CompLocator` (ComponentId, Type 1 = key file). The property
must be public and listed in `SecureCustomProperties` to reach the server side.

A search can give a directory its default: a directory's property set before `CostFinalize` is where
the directory resolves. rubrapack searches into a property of its own (`RpFound_<ID>`) and copies
it with a type-51 action (Source = the directory, Target = `[RpFound_<ID>]`) at 51 in both sequences
under the condition `RpFound_<ID> AND NOT <DIR>`, so a directory given on the command line is left
alone. The registry locator then has Type 0 (a folder, which must exist; the value comes back with
a trailing backslash) instead of 2: **AppSearch looks a folder-type RegLocator up in the
`Signature` table, which must exist, even empty** - without it the installation stops with 2228.
[observed: a /qn major upgrade installs into the folder the earlier version recorded; `DIR=` on the
command line wins]

rubrapack's install folder guard (`guard = true`) is an immediate DLL custom action (type 1, the
helper DLL in `Binary`) at 1010 in InstallExecuteSequence, after `CostFinalize` has resolved the
directories and before `InstallValidate`, under `NOT Installed`. It reads the Directory keys from a
property, gets each path with `MsiGetTargetPath`, and refuses a path in which an existing part is a
reparse point or not a folder, or whose folder exists with an owner (`GetNamedSecurityInfo`) other
than S-1-5-18, S-1-5-32-544 or TrustedInstaller; it then shows the message with
`MsiProcessMessage(INSTALLMESSAGE_ERROR)` and returns `ERROR_INSTALL_FAILURE`, so the installation
ends with 1603 before any file is written. [observed]

`LaunchCondition`
(**Condition**, Description formatted) at 100 in both sequences stops the installation with the
description when a condition is false. [observed]

## Services, fonts, permissions

`ServiceInstall` (Name, DisplayName and Description formatted, ServiceType 0x10, StartType 2/3/4,
ErrorControl 1, StartName null or `NT AUTHORITY\LocalService`, Arguments formatted, Component_ =
the exe's component) with `InstallServices` (5800); `ServiceControl` Event flags 0x1 start on
install, 0x2 stop on install, 0x20 stop on removal, 0x80 delete on removal, Wait 1, with
`StopServices` (1900), `DeleteServices` (2000), `StartServices` (5900). `Font` (File_, FontTitle
null = read from the font) with `RegisterFonts`/`UnregisterFonts`; the file must be in
`FontsFolder` itself. `MsiLockPermissionsEx` (**MsiLockPermissionsEx**, LockObject, Table =
`CreateFolder`, `File`, `Registry` or `ServiceInstall`, **SDDLText** (the column name), Condition)
needs Windows Installer 5.0 (summary page count 500); a folder gets it through a CreateFolder row.
A failed installation leaves no service or font. [observed]

## Cabinets, administrative images, advertisement

Several cabinets: one `Media` row each (DiskId 1..n, LastSequence = the last file's sequence in
it); embedded ones are streams named in `Cabinet` with `#`, external ones are files next to the
package named in `Cabinet` without `#` (long names work). `AdminExecuteSequence` (CostInitialize
800, FileCost 900, CostFinalize 1000, InstallValidate 1400, InstallInitialize 1500,
InstallAdminPackage 3900, InstallFiles 4000, InstallFinalize 6600) and `AdminUISequence` make
`msiexec /a` write an uncompressed image that installs like the original; `AdvtExecuteSequence`
(CostInitialize, CostFinalize, InstallValidate, InstallInitialize, PublishFeatures 6300,
PublishProduct 6400, InstallFinalize) makes `msiexec /jm` advertise the product. [observed]

## Per-user and dual packages

The single-package form: `ALLUSERS=2` with `MSIINSTALLPERUSER=1` installs per user - the engine
points `ProgramFilesFolder`/`ProgramFiles64Folder` at `%LOCALAPPDATA%\Programs` and the menu and
desktop folders at the user's - and `ALLUSERS=1 MSIINSTALLPERUSER=""` installs per machine.
Summary Word Count bit 8 says no elevation is needed. Registry Root -1 (HKMU) is HKCU per user and
HKLM per machine; an Environment name without `*` is a user variable. Once a per-user product is
installed the engine deletes `MSIINSTALLPERUSER`, so a launch condition that tests it must read
`Installed OR ...` or the product can no longer be removed; in general, write every launch
condition as `Installed OR (...)`. [observed]

## Files in use

When a file to be replaced or removed is held by a running program (a DLL loaded into it):

- With Restart Manager, the default, Windows Installer tries to **shut down every program holding
  such a file, even in a silent installation** (`/qn`), and restarts it afterwards; if one does not
  close, the whole installation fails (1601). [observed]
- With `MSIRESTARTMANAGERCONTROL=Disable` in the Property table, the engine moves the held file
  aside (`C:\Config.Msi\*.rbf`), puts the new file in place at once and returns 0; the program keeps
  running on the old copy, programs started afterwards load the new file, and the old copy is
  deleted at the next restart. [observed]
- A property given on the command line reaches only the package being installed. During an
  upgrade, the removal of the old version follows **the old package's** Property table - author
  the property from the first version on. [observed]

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
CostInitialize 800, FileCost 900, CostFinalize 1000, MigrateFeatureStates 1200, ExecuteAction 1300
(and the dialogs, see [Dialogs](#dialogs)).

`RemoveExistingProducts` right after `InstallInitialize` puts the removal of the old version
inside the new installation's transaction, so a failed upgrade rolls back to the old version.
[observed: a deferred custom action that fails right after `InstallFiles` makes the upgrade end
with 1603, and the old version is registered again with its files byte for byte; a failed first
installation leaves no files and no registration.]

## Dialogs

A package with dialogs carries these tables (types as above):

| Table | Columns |
|---|---|
| Dialog | **Dialog** s72, HCentering i2, VCentering i2, Width i2, Height i2, Attributes I4, Title L128, Control_First s50, Control_Default S50, Control_Cancel S50 |
| Control | **Dialog_** s72, **Control** s50, Type s20, X i2, Y i2, Width i2, Height i2, Attributes I4, Property S72, Text L0, Control_Next S50, Help L50 |
| ControlEvent | **Dialog_** s72, **Control_** s50, **Event** s50, **Argument** s255, **Condition** S255, Ordering I2 |
| ControlCondition | **Dialog_** s72, **Control_** s50, **Action** s50, **Condition** s255 |
| EventMapping | **Dialog_** s72, **Control_** s50, **Event** s50, Attribute s50 |
| TextStyle | **TextStyle** s72, FaceName s32, Size i2, Color I4, StyleBits I2 |
| UIText | **Key** s72, Text L255 |
| Binary | **Name** s72, Data v0 (a stream) |
| RadioButton | **Property** s72, **Order** i2, Value s64, X i2, Y i2, Width i2, Height i2, Text L0, Help L50 |
| ComboBox | **Property** s72, **Order** i2, Value s64, Text L64 |

What the engine checks when it shows a dialog (each seen as an error dialog carrying the number):

- **Tab order.** `Control_Next` must form one cycle through every control that has one, starting
  at `Control_First`. A chain that leaves a control out, or ends at a control whose `Control_Next`
  is empty, stops the installation with 2810 or 2809. Static text takes no part in the cycle.
  [observed] rubrapack checks every dialog it builds and refuses to write a broken one (RP0010).
- **The error dialog.** The dialog named by the `ErrorDialog` property needs a `Text` control
  named `ErrorText` and an `Icon` control named `ErrorIcon` (2835 otherwise); its Attributes carry
  the error-dialog bit (0x10000), and push buttons named `A`, `C`, `I`, `N`, `O`, `R`, `Y` with
  `EndDialog` arguments `ErrorAbort` .. `ErrorYes` are shown as the message needs. [observed]
- **Brackets in texts.** `Text` is a formatted string: `[Next]` is read as a property and shows as
  nothing. [observed] Quote button names in running text instead.
- **Files in use.** With Restart Manager off, the engine shows the dialog named `FilesInUse`
  (buttons ending with `Retry`, `Ignore`, `Exit`) with a `ListBox` bound to `FileInUseProcess`. The
  **ListBox table must exist, even empty**: without it the engine logs 2205 and skips the dialog,
  and the installation goes on as if Ignore was chosen. In a major upgrade the dialog comes twice
  (the new files, then the old version's removal). After Ignore the held file is moved aside and
  the installation ends with 0. [observed]
- **Error icon.** The engine puts its own icon for the message type into `ErrorIcon` (a warning
  triangle for a launch condition), whatever the control's Binary holds. [observed]
- **List order.** A `ComboBox` lists its items alphabetically unless the control has the Sorted
  attribute (0x10000); with it they follow the Order column. [observed] rubrapack sets it.
- **Elevation.** A per-machine installation started interactively by a non-elevated user asks for
  consent (UAC) when the execute sequence starts, on the secure desktop. [observed]
- **Code page.** With the database code page 65001, Korean titles, texts and RTF license text show
  correctly under a Korean and under an English (1252) system locale. [observed, see
  [msi-database.md](msi-database.md#code-page)]

rubrapack's sets: every dialog is 370 x 270 dialog units with a banner strip (a `Bitmap` control
0,0,370,44 whose picture is `Binary.RpBanner`), the title in bold (`{\RpTitle}`, a TextStyle) and a
line of description in it, a `Line` at 44 and at 234, and Back / Next / Cancel at y 243. The
InstallUISequence adds the welcome (1230, `NOT Installed`) or the maintenance dialog (1240,
`Installed AND NOT RESUME AND NOT Preselected`), the progress dialog (1280, modeless, subscribed to
`ActionText` and `SetProgress` through EventMapping) and the three exit dialogs at -1 (success), -2
(cancelled) and -3 (fatal). The install folder dialog puts the folder's Directory key in a
`PathEdit`, and its Next runs `SetTargetPath` before `NewDialog`, which checks the path. The
license text is a `ScrollableText` control holding RTF: plain text becomes `\uN?` escapes
(characters above U+FFFF as a surrogate pair), one `\par` per line.

Author pages are ordinary Dialog rows in the same frame; `Control_Next` runs through their
controls in position order and on to Back, Next and Cancel. A radio group is a
`RadioButtonGroup` control whose buttons are RadioButton rows (positions relative to the group); a
drop-down list is a `ComboBox` control with ComboList (0x20000) and Sorted, filled from ComboBox
rows. Their properties are added to `SecureCustomProperties`, so values chosen in the dialogs or
given on the command line reach the execute sequence. [observed]

### Choices during installation

What rubrapack writes for the choices a user makes [observed, Windows 11 26100]:

- **Features.** `Feature.Attributes` 0x10 (UIDisallowAbsent) keeps "will be unavailable" out of
  the tree's menu; 0x02 (FollowParent) installs a feature where its parent is. A feature that is off
  unless a condition holds is a `Condition` row (**Feature_** s38, **Level** i2, Condition S255)
  with Level 0 and the condition `NOT Installed AND NOT (<when>)`. **Without `NOT Installed` the
  row turns the feature off again at removal, when the property is gone, and its files are left
  behind.**
- **Components.** `when` on a file, a registry value, an environment variable or an INI value is
  the component's `Condition`, evaluated when the component is first installed (no Transitive
  attribute: a repair keeps what is there). Every shortcut gets a component of its own in the shortcut's folder, in the target file's feature,
  whose key path is a registry value under root 1 (HKCU), with the attribute 0x4; `when` is that
  component's `Condition`.
- **Icons.** An `Icon` row (**Name** s72 ending in `.ico`, Data v0) holds the `.ico`;
  `ARPPRODUCTICON` names it for Installed apps, which the engine registers as the product's
  `ProductIcon` under `HKLM\SOFTWARE\Classes\Installer\Products` (not as `DisplayIcon`), and
  `Shortcut.Icon_` / `IconIndex` 0 give a shortcut its own.
- **Keep.** A file left at removal is a component with 0x10 (Permanent).
- **Launch.** A type-34 custom action with 0xC0 (asynchronous, no wait): Source = the program's
  folder, Target = `"[#FileKey]" args`. The finished dialog's Finish runs it through `DoAction`
  before `EndDialog` under `RPLAUNCH = "1" AND NOT Installed`; `Installed` still has its value from
  the start of the setup, so repair and removal do not start it. It runs in the setup's client
  process, with the rights of the user who started the setup.
- **Change.** The maintenance dialog's Change is a `NewDialog` to the dialog with the
  `SelectionTree`; that dialog's Back goes to the maintenance dialog under `Installed` and to the
  install folder dialog under `NOT Installed`; the ready dialog's `EndDialog Return` then applies
  the new feature states.
- **Scope.** In a dual package (ALLUSERS=2, MSIINSTALLPERUSER=1 by default) the folder properties
  follow the per-user default when the dialogs start. The scope page's Next sets `[ALLUSERS]` and
  `[MSIINSTALLPERUSER]` (`{}` clears it), then puts the install folder's path under
  `[%ProgramW6432]` (or `[%ProgramFiles(x86)]` for an x86 package) or `[LocalAppDataFolder]Programs`
  into the directory's property and runs `SetTargetPath`; the execute sequence resolves the other
  folders itself with the final scope.

### Several languages in one package

The dialog tables hold one language at a time, but every text they show can come from a property,
so one set of dialogs can speak several languages. What the engine does [observed, Windows 11
26100]:

- A `Text`, `PushButton`, `CheckBox`, `RadioButton` or `ComboBox` text of `[RpT_X]`, and a Dialog
  `Title` of `[P]`, show the property's value; each dialog reads it when it is created, so a value
  changed by one dialog shows on the next.
- A text style prefix at the start of the value (`{\RpTitle_ko}...`) applies.
- **The value is not formatted again**: `[ProductName]` inside a property's value shows literally.
  The text must be formatted when it is put into the property - by a `[RpT_X]` ControlEvent, whose
  Argument is formatted when the event runs, or by a type-51 custom action, whose Target is.
- Setting `[DefaultUIFont]` by an event changes the face of every control without its own style on
  the next dialog.
- A dialog's `Control_First` must be visible when the dialog opens (2836 otherwise); a control
  hidden by ControlCondition may stay in the tab cycle.

rubrapack's layout with `[ui] languages`: English is language 0. For every text X and language L,
`RpT_X_L` in the Property table holds the text with `[ProductName]`, `[Manufacturer]` and
`[ProductVersion]` put in at build time (and the title style for headings); `RpT_X` holds the English
one. The InstallUISequence starts with type-51 actions: `RPLANGUAGE` = L when unset and
`UserLanguageID` (17), then `SystemLanguageID` (18), is one of L's LANGIDs, else `en` (19); then at
21 one action per text copies `[RpT_X_L]` into `RpT_X` under `RPLANGUAGE = "L"`, and one sets
`DefaultUIFont`. A text that uses other properties is copied as its formatted source instead. The
language page (`RpLanguageDlg`, 1225, a `RadioButtonGroup` on `RPLANGUAGE`) replaces the welcome and
maintenance rows; its Next runs the same copies as ControlEvents, then `NewDialog` to the welcome or
the maintenance page. A license per language is a `ScrollableText` per language in the same place,
each with `Show`/`Hide` ControlConditions on `RPLANGUAGE`. The UIText table has one language:
English.

## What this recipe does not cover yet

Custom actions other than the error type, the register pair and the REG_QWORD helper above; and
`_Validation` (needed by validation tools, not by the installer). These
pages grow as rubrapack implements them.
