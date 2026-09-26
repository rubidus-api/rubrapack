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
| uninstall | `MsiConfigureProduct(ProductCode, ..., INSTALLSTATE_ABSENT)` | 0; folders, keys and registration gone |

Per-machine installs need an elevated process. Keep test products on their own UpgradeCodes and
remove them in the same run.

## Things that look fine but are not

- An `msi.dll`-made database is not a complete specification: `msi.dll` stores 65001 summary
  strings correctly and still cannot read them back ([msi-summary.md](msi-summary.md)).
- `MsiDatabaseExport` prints summary dates in local time; compare dates in UTC.
- A successful `MsiOpenDatabase` says nothing about row order or reference counts - only
  installation does. Test with an install, not just an open.
