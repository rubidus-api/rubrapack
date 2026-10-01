# Windows folders and environment variables

A package names places on a PC its author has never seen: "the program files folder", "this
user's settings folder", "the Windows folder". On one PC the program files folder is
`C:\Program Files`, on another `D:\Program Files`; the user is called someone different on each.
This chapter explains how Windows names such places - environment variables, known folders and
Windows Installer's folder properties - and *when* each name is turned into a real path.

## What an environment variable is

Every running program carries a small list of **environment variables**: names with text values,
such as `TEMP=C:\Users\kim\AppData\Local\Temp`. A program gets a copy of the list from the program
that started it; changing its copy changes nothing for anyone else. Names are not case-sensitive:
`%TEMP%`, `%Temp%` and `%temp%` are the same variable.

Each tool writes them its own way:

| Where | How a variable is written |
|---|---|
| Command Prompt (`cmd.exe`), batch files | `%TEMP%` |
| PowerShell | `$env:TEMP` |
| A registry value of type REG_EXPAND_SZ, a shortcut's target | `%TEMP%`, replaced when the value is read |
| A Windows Installer formatted string | `[%TEMP]`, replaced during installation |

Where the list comes from: when a user logs on, Windows builds it from

- the **system** variables, the same for every user (`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`);
- the **user's** variables (`HKCU\Environment`); a user `Path` is added to the end of the system one;
- values Windows works out at logon: the user's name and profile folders, the domain, the logon
  server (`HKCU\Volatile Environment`), and the standard folders.

A change to the stored variables reaches only programs started afterwards. A package that sets a
variable (`[env.*]`, tutorial chapter 11) therefore asks for a new Command Prompt before it is
seen.

## The common variables

Typical values on Windows 10 and 11, for a user named `kim` on drive `C:`.

### Folders

| Variable | Typical value | Notes |
|---|---|---|
| `SystemDrive` | `C:` | the drive Windows started from; no backslash |
| `SystemRoot` | `C:\Windows` | the Windows folder |
| `windir` | `C:\Windows` | the older name for the same folder |
| `ProgramFiles` | `C:\Program Files` | in a 32-bit program on 64-bit Windows: `C:\Program Files (x86)` |
| `ProgramFiles(x86)` | `C:\Program Files (x86)` | 64-bit Windows only |
| `ProgramW6432` | `C:\Program Files` | 64-bit Windows only; the 64-bit folder, also for a 32-bit program |
| `CommonProgramFiles` | `C:\Program Files\Common Files` | shared parts of several programs; `(x86)` and `W6432` forms as above |
| `ProgramData` | `C:\ProgramData` | data shared by all users |
| `ALLUSERSPROFILE` | `C:\ProgramData` | the older name for the same folder |
| `PUBLIC` | `C:\Users\Public` | files every user may see |
| `USERPROFILE` | `C:\Users\kim` | the user's profile folder |
| `HOMEDRIVE`, `HOMEPATH` | `C:`, `\Users\kim` | the user's home, in two parts; in a domain it may be a network share (`HOMESHARE`) |
| `APPDATA` | `C:\Users\kim\AppData\Roaming` | settings that travel with a roaming profile |
| `LOCALAPPDATA` | `C:\Users\kim\AppData\Local` | settings and caches of this PC only |
| `TEMP`, `TMP` | `C:\Users\kim\AppData\Local\Temp` | temporary files; for services `C:\Windows\Temp` |

Older guides give paths such as `C:\Documents and Settings\kim\Application Data`: those are
Windows XP's. Since Windows Vista the profiles are under `C:\Users`, and `ALLUSERSPROFILE` became
`C:\ProgramData`.

The two Program Files folders are why a 32-bit program asked for `%ProgramFiles%` gets
`C:\Program Files (x86)`: Windows gives each kind of program the folder of its own kind.

### Not folders

| Variable | Typical value | Notes |
|---|---|---|
| `USERNAME` | `kim` | the account's name |
| `USERDOMAIN` | `OFFICE`, or the PC's name | the domain of the account; the computer's name for a local account |
| `LOGONSERVER` | `\\DC01`, or `\\` and the PC's name | the computer that checked the password |
| `COMPUTERNAME` | `KIM-PC` | this PC's name |
| `ComSpec` | `C:\Windows\system32\cmd.exe` | the command interpreter |
| `Path` | `C:\Windows\system32;C:\Windows;...` | folders searched for a program typed without a folder, separated by `;` |
| `PATHEXT` | `.COM;.EXE;.BAT;.CMD;...` | the extensions tried when a program is typed without one |
| `OS` | `Windows_NT` | |
| `PROCESSOR_ARCHITECTURE` | `AMD64`, `ARM64` or `x86` | `x86` in a 32-bit program, which finds the real one in `PROCESSOR_ARCHITEW6432` |
| `NUMBER_OF_PROCESSORS` | `8` | |

The Command Prompt also answers `%CD%`, `%DATE%`, `%TIME%`, `%RANDOM%` and `%ERRORLEVEL%`, but it
makes them up when asked; they are not in the environment, and nothing else sees them.

## Folders without a variable

The desktop, the Start menu and its Programs folder, the Startup folder, the Fonts folder and
Documents have no environment variable. Windows names them as **known folders**, and a program asks
for one with `SHGetKnownFolderPath`. That is also the reliable way for the folders that do have a
variable: whoever starts a program can give it any environment, and a user may have moved
Documents or the desktop elsewhere (into OneDrive, for example), which only the known folder
reflects.

## Three moments

A name becomes a path at one of three moments, and the moment decides *whose* path it is.

1. **Build time**, on the machine that runs `rubrapack build`. Nothing about the target PC is known
   yet. rubrapack's `$(NAME)` variables are replaced here; they never read the build machine's
   environment (pass a value with `-D NAME=value`).
2. **Install time**, on the target PC. Windows Installer works out its folder properties and the
   `[%NAME]` parts of formatted strings, for the user who installs. In a per-machine package
   installed by an administrator, the "local application data" folder is the administrator's.
3. **Run time**, whenever a program reads the value. A REG_EXPAND_SZ value holding
   `%LOCALAPPDATA%\Hello\log` gives every user their own folder, because each user's program
   expands it with that user's environment.

So a log folder for each user is written for run time:

```toml
[registry.LogDir]
root = "HKLM"
key = 'SOFTWARE\Example Software\Hello'
name = "LogDir"
type = "expand"
value = '%LOCALAPPDATA%\Hello\log'        # expanded by each user's program
```

whereas `value = '[LocalAppDataFolder]Hello\log'` would store the installing user's folder for
everybody.

## Windows Installer's names

Windows Installer has its own property for most folders, worked out at install time:

| Variable | Windows Installer property |
|---|---|
| `ProgramFiles` | `ProgramFiles64Folder` in a 64-bit package, `ProgramFilesFolder` in a 32-bit one |
| `ProgramFiles(x86)` | `ProgramFilesFolder` |
| `CommonProgramFiles` | `CommonFiles64Folder` / `CommonFilesFolder` |
| `ProgramData`, `ALLUSERSPROFILE` | `CommonAppDataFolder` |
| `APPDATA` | `AppDataFolder` |
| `LOCALAPPDATA` | `LocalAppDataFolder` |
| `TEMP` | `TempFolder` |
| `SystemRoot`, `windir` | `WindowsFolder` |
| `SystemDrive` | `WindowsVolume` (with a backslash: `C:\`) |
| (`System32`) | `System64Folder` in a 64-bit package; `SystemFolder` is the 32-bit system folder |
| `USERNAME` | `LogonUser` |
| `COMPUTERNAME` | `ComputerName` |

Every folder property ends in a backslash, so `[ProgramFiles64Folder]Hello` needs none between.
Beware of `USERNAME`: Windows Installer's property of that name is the name typed for registration,
not the account; the account is `LogonUser`. Any variable without a property is reached as
`[%NAME]`.

## In rubrapack

- **Path bases.** The first part of a `[dir.*]` or `[search.*]` path names a standard folder, which
  rubrapack turns into the matching Windows Installer property or, in an MSIX, the matching
  package folder: `ProgramFiles`, `ProgramFiles32`, `CommonFiles`, `AppData`, `LocalAppData`,
  `CommonAppData`, `StartMenu`, `Programs`, `Desktop`, `Startup`, `Windows`, `System`, `Fonts`,
  `Temp` (tutorial chapter 4).
- **Install-time values.** `[registry.*]`, `[env.*]`, `[ini.*]` values and shortcut arguments are
  Windows Installer formatted strings: `[INSTALLDIR]`, `[LogonUser]`, `[%USERPROFILE]` are filled in
  during installation.
- **Run-time values.** `%NAME%` passes through unchanged; in a `type = "expand"` registry value
  Windows expands it when the value is read (tutorial chapter 11).
- **MSIX.** An MSIX has no install time: a value with a `[...]` part is refused (RP1612), while a
  `%NAME%` in an expandable value still works, since the program expands it.

## Where this is used

- Tutorial chapter [4](../tutorial/04-files-and-folders.md): install folders and their bases.
- Tutorial chapter [11](../tutorial/11-registry-environment-ini.md): registry values, environment
  variables and `%TEMP%`.
- Reference: [Registry values](../rpk.md#registry-values-registryid),
  [Environment variables](../rpk.md#environment-variables-envid), [Variables](../rpk.md#variables).
