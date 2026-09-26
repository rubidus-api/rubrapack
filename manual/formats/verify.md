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

## Things that look fine but are not

- An `msi.dll`-made database is not a complete specification: `msi.dll` stores 65001 summary
  strings correctly and still cannot read them back ([msi-summary.md](msi-summary.md)).
- `MsiDatabaseExport` prints summary dates in local time; compare dates in UTC.
- A successful `MsiOpenDatabase` says nothing about row order or reference counts - only
  installation does. Test with an install, not just an open.
