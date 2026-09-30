# Checking your output against Windows

Microsoft does not publish the MSI table encoding, so the reference is Windows itself. These
checks need only a Windows machine and the Windows Installer API (`msi.dll`); no SDK tools.

## The database opens and decodes

1. `MsiOpenDatabase` your file read-only - the compound file and string pool are acceptable.
2. `MsiDatabaseExport` every table (and `_ForceCodepage`, `_SummaryInformation`) to IDT files.
3. Compare with what your own reader exports, or with the IDT of a reference database made
   through `msi.dll` (`MsiOpenDatabase` with create mode, `MsiDatabaseImport` of a
   `_ForceCodepage.idt` containing `65001`, SQL `CREATE TABLE`/`INSERT` via views). Byte-identical
   IDT means your encoding matches.

## The package installs

Drive the installer from a program rather than `msiexec`, so every step is synchronous and
returns an error code:

| Step | API | Expect |
|---|---|---|
| silent UI | `MsiSetInternalUI(INSTALLUILEVEL_NONE)` | |
| verbose log | `MsiEnableLog` (all modes) | read it when anything fails |
| install | `MsiInstallProduct(path, "")` | 0 |
| names | `FindFirstFile` on each installed path, compare the entry's own UTF-16 name exactly | equal |
| content | `MsiGetFileHash` of each installed file vs your `MsiFileHash` row | equal |
| registration | `MsiGetProductInfo(ProductCode, InstalledProductName)` | your ProductName |
| repair | delete one installed file, `MsiReinstallProduct(ProductCode, ...)` | file back, same name |
| upgrade | install version N+1 over N | old ProductCode gone, new one installed |
| downgrade | install version N again | refused (1603, the log names your refusing action) |
| features | `MsiQueryFeatureState`; `MsiConfigureFeature(..., INSTALLSTATE_LOCAL)` | levels above `INSTALLLEVEL` absent until turned on |
| rollback | copy of version N+1 with a deferred custom action that fails after `InstallFiles` (type 34 + 1024, `"[SystemFolder]cmd.exe" /c exit 1`) and a new package code, installed over N | 1603 (log: error 1722); N still registered, its files unchanged |
| uninstall | `MsiConfigureProduct(ProductCode, ..., INSTALLSTATE_ABSENT)` | 0; folders, keys and registration gone |

Per-machine installs need an elevated process. Keep test products on their own UpgradeCodes and
remove them in the same run.

## Checks before writing

`msi.dll` accepts many broken databases and only fails at install time, if at all (it does not
even enforce column widths). A writer should check its own tables first. rubrapack's `build`
refuses to write a package that breaks any of these (diagnostics `RP2001`-`RP2015`):

- every cell has the column's kind; no null in a non-nullable column; strings no longer than the
  column width, counted in UTF-16 units (a non-BMP character counts two), and valid UTF-8;
  16-bit integers within -32767..32767 and 32-bit ones not -2147483648 (those values mean null);
- no duplicate primary keys; references (`Component.Directory_`, `File.Component_`,
  `FeatureComponents`, `CreateFolder`, `MsiFileHash.File_`, `Directory_Parent`, `Feature_Parent`)
  point at existing rows; every sequence action is a standard action, a `CustomAction` or a
  `Dialog`;
- `ProductCode`, `ProductName`, `ProductVersion`, `Manufacturer`, `ProductLanguage` are present;
  GUIDs are upper case in braces, component GUIDs are unique, ProductCode differs from UpgradeCode;
- each `File.Sequence` is unique and covered by a `Media.LastSequence`;
- standard actions keep their order (costing before `InstallValidate`, file actions between
  `InstallInitialize` and `InstallFinalize`);
- the 64-bit component attribute (256) matches the summary Template platform (`x64`/`Arm64` vs
  `Intel`);
- a file key path is a file of the same component;
- each `Upgrade.ActionProperty` is upper case (public) and listed in `SecureCustomProperties` -
  otherwise a per-machine install does not pass it to the server side.

## Microsoft's ICE rules [observed]

The Windows SDK's "MSI Tools" include MsiVal2 and `darice.cub`, the Internal Consistency Evaluators
(ICEs) that Microsoft's own authoring tools run: `MsiVal2 package.msi darice.cub -f` prints what
fails. They run only on Windows, and they go further than the checks above - they know the
meaning of the tables, not only their shape. What they report for rubrapack's test packages
(every source of its test suite, 39 packages), and why:

- **ICE03, "Missing specifications in _Validation"** for every column: the `_Validation` table
  describes each column for validators only; Windows Installer does not read it, and rubrapack
  does not write it.
- **ICE82**, duplicate sequence numbers: the language actions of a package with several dialog
  languages share one number. They are independent set-property actions, so their order does not
  matter.
- **ICE34**, the language page's radio property has no `Property` row: on purpose, because the
  command line (`RPLANGUAGE=ko`) must win over the detection, which only runs `NOT RPLANGUAGE`.
- **ICE43 and ICE57**, a non-advertised shortcut in a component whose key path is a file: they
  assume shortcut folders are per user. In a per-machine package (`ALLUSERS=1`) the Start menu and
  desktop are the all-users folders, and a per-user or dual package installs once per user, so
  the "first user only" problem these ICEs guard against does not arise.
- **ICE52**, a private property in `AppSearch` (`RpFound_<ID>`, a remembered folder): private on
  purpose, so the command line cannot set it; `AppSearch` runs in both sequences, so it needs no
  passing to the server side.

Three findings were real and are fixed (0.4.3): the finished, cancelled and failed pages were
missing from `AdminUISequence`, so an administrative installation (`msiexec /a`) with the full UI
ended without them (ICE20); a component whose key path is its folder (an INI entry, a `[remove]`)
was not listed in `CreateFolder` (ICE18); and a package with dialogs had no `ControlCondition`
table, which ICE17 reads for every dialog (it stopped with error 2228 instead of checking).

## Things that look fine but are not

- An `msi.dll`-made database is not a complete specification: `msi.dll` stores 65001 summary
  strings correctly and still cannot read them back ([msi-summary.md](msi-summary.md)).
- `MsiDatabaseExport` prints summary dates in local time; compare dates in UTC.
- A successful `MsiOpenDatabase` says nothing about row order or reference counts - only
  installation does. Test with an install, not just an open.
