# Tables, keys and references

Inside an `.msi` (chapter 6) the streams are mostly **tables**: the package is a small
**database**. Windows Installer does not follow a script of steps written by you; it reads the
tables and works out what to do. This chapter explains the few database ideas needed to read them.

## Tables, rows and columns

A **table** is a grid. Each **column** has a name and a type; each **row** is one thing - one file,
one shortcut, one registry value. Here is the `File` table of the tutorial's first package (chapter
2 of the tutorial), as `rubrapack inspect hello.msi File` prints it:

```text
File	Component_	FileName	FileSize	Version	Language	Attributes	Sequence
s72	s72	l255	i4	S72	S20	I2	i4
File	File
Hello	C_185f8db32271fe25f561	hello.exe	17920	1.2.3.4	1033	512	1
```

Laid out as a grid:

| File | Component_ | FileName | FileSize | Version | Language | Attributes | Sequence |
|---|---|---|---|---|---|---|---|
| `Hello` | `C_185f8db32271fe25f561` | `hello.exe` | 17920 | `1.2.3.4` | `1033` | 512 | 1 |

The second line of the text gives the **type** of each column:

| Type | Meaning |
|---|---|
| `s72` | a string of at most 72 characters |
| `l255` | a *localizable* string (one a translation may change) of at most 255 characters |
| `i2`, `i4` | an integer of 2 or 4 bytes (chapter 2) |
| capital `S`, `L`, `I` | the same, but the cell may be empty ("null") |

A column's type is fixed by Windows Installer for each standard table, and rubrapack checks every
value against it (`lint`, tutorial chapter 18): a 100,000 in an `i2` column would not fit.

## Primary keys: what makes a row unique

The third line, `File	File`, names the table and its **primary key** - the column (or
columns) whose value is different in every row. In `File` it is the column `File`: `Hello` names
this row and no other row may use that name. This is why rubrapack's IDs must be unique (tutorial
chapter 2): the ID you write in `[file.Hello]` becomes this key.

Some tables need two columns together. `FeatureComponents` says which component belongs to which
feature; one feature has many components and one component may be in several features, so only the
pair is unique:

```text
Feature_	Component_
s38	s72
FeatureComponents	Feature_	Component_
Main	C_185f8db32271fe25f561
```

## Foreign keys: rows that point at rows

A column whose name ends in `_` holds the key of a row in *another* table - a **foreign key**, or
reference. Following them connects the tables into one picture. Start at the file and follow the
references:

```text
File              Hello
  Component_  --> Component   C_185f8db32271fe25f561
                    ComponentId   {B80BEC59-F582-8F10-8EB1-638C6687B899}
                    Directory_  --> Directory   INSTALLDIR
                                      DefaultDir   Hello
                                      Directory_Parent --> Directory   ProgramFiles64Folder
                                                             DefaultDir  .
                                                             Directory_Parent --> TARGETDIR
                    KeyPath     --> File        Hello   (back to the start)
FeatureComponents Main + C_185f8db32271fe25f561
  Feature_    --> Feature     Main
                    Level  1   (installed by default)
```

Read aloud: the file `hello.exe` belongs to a component, which installs into the folder `Hello`
inside `ProgramFiles64Folder` (Program Files), and which the feature `Main` installs. The
component's *key path* is the file itself: if `hello.exe` is there, the component counts as
installed (tutorial chapter 4).

A reference to a row that does not exist is an error Windows Installer would stop on - one of the
things `rubrapack lint` checks in any `.msi`, from any tool.

## Keys you give, keys rubrapack makes

In the tables, the IDs from your source appear as they are - `Hello`, `INSTALLDIR`, `Main`. Where
Windows Installer needs a row that has no table of its own in the source, rubrapack makes a key: a
component per file is named `C_` and 20 hex digits, a file found by a glob `F_` and 20 hex digits.
The digits come from a hash of the thing's place (chapter 4), so they stay the same from build to
build.

## Storing strings once: the string pool

Tables are full of repeated strings - `INSTALLDIR`, a component key, `Hello` - and an MSI keeps
each distinct string only once. All strings of the package live in one list, the **string pool**
(two streams, `_StringPool` and `_StringData`), and a string cell in a table holds just the
string's *number* in that list, in 2 bytes (or 3 in very large packages). The tutorial's first
package has 111 distinct strings for all 16 tables:

```text
C:\work\hello> rubrapack inspect hello.msi
code page: 65001
strings: 111
tables: 16
```

The pool also counts how many cells use each string. Part IV shows its bytes.

## Tables about tables

A database describes itself. Two tables list the others: `_Tables` has one row per table, and
`_Columns` one row per column of every table, with its type. That is where `inspect` finds the
column names and types it prints on the first two lines. The *sequence* tables
(`InstallExecuteSequence`, `InstallUISequence` and three more for administrative and advertised
installations) list the installation's steps in order, with a condition each (tutorial chapters 9
and 13) - even the procedure is a table.

## The IDT text format

The text `inspect` prints is Windows Installer's own **IDT** format: the three header lines, then
one line per row, columns separated by tabs. The tools in Microsoft's Windows SDK import and export
tables in this format, so a table from `rubrapack inspect` can be compared with one exported by
another tool.

## Where this is used

- Tutorial chapters [2](../tutorial/02-a-first-installer.md) and
  [4](../tutorial/04-files-and-folders.md): IDs, components, key paths.
- Tutorial chapter [18](../tutorial/18-checking-and-looking-inside.md): `inspect` and `lint`.
- Part IV: [the MSI database](../formats/msi-database.md) (the string pool and table encoding),
  [the tables that install](../formats/msi-package.md).
