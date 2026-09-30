# The MSI database inside the compound file

Microsoft documents the *meaning* of the Windows Installer tables (Microsoft Learn, "Database
Tables"), but not how a database is laid out in the compound file. Everything on this page about
the layout was established by creating databases through `msi.dll`, reading the streams back
byte for byte, and checking that databases written from these rules open, export and install.
[observed] unless marked otherwise.

## Streams

Every table is one stream directly under the root storage. There are no sub-storages in an
ordinary package.

### Stream names

A table named `T` is stored under the name `U+4840` followed by `T` **packed** two characters per
UTF-16 unit:

1. Map each character of this 64-character alphabet to 0..63:
   `0`-`9` -> 0..9, `A`-`Z` -> 10..35, `a`-`z` -> 36..61, `.` -> 62, `_` -> 63.
2. Two alphabet characters in a row become one unit `0x3800 + first + (second << 6)`.
3. A single alphabet character (the last one, or one followed by a character outside the
   alphabet) becomes `0x4800 + value`.
4. Any other character is written as itself.

The packed name (with the `U+4840` prefix for tables) must fit the 31-unit CFB name limit, which
allows table names of up to 60 alphabet characters. Unpacking reverses the steps.

Example: `_Columns` -> `U+4840 U+3B3F U+43F2 U+4438 U+45B1`.

Other streams:

| Stream | Name |
|---|---|
| a Binary/Icon cell (`OBJECT` column) | `<Table>.<key>` packed the same way **without** the `U+4840` prefix, e.g. `Binary.Logo` (compound keys joined with `.`) |
| an embedded cabinet | the name after `#` in `Media.Cabinet` (e.g. `cab1.cab`), packed, no prefix |
| summary information | `\x05SummaryInformation` - not packed (see [msi-summary.md](msi-summary.md)) |

## The string pool: `_StringPool` and `_StringData`

All strings of all tables live once in a shared pool; table cells hold string ids.

`_StringPool`:

```text
u32  header       low 31 bits: the database code page; bit 31: string references are 3 bytes
then per string id 1, 2, 3, ... (id 0 is "no string" and is not stored):
u16  length       byte length in _StringData
u16  refs         bits 0-14: reference count; bit 15: set exactly when the string has a byte >= 0x80
```

- **Long strings** (64 KiB of bytes or more): the entry is `length = 0`, `refs` as above (non-zero),
  followed by an extra `u32` with the full byte length. The pair still describes one id. Seen:
  a 70002-byte string used once -> `0000 8001 00011172`.
- An entry with `length = 0` and `refs = 0` is a free id.
- **Reference count** = the number of cells that hold the string, plus, per table, 1 for its
  `_Tables` row and, per column, 1 for the table name in `_Columns` and 1 for the column name.
  (Checked by recounting every database `msi.dll` produced.)
- `_StringData` is all the strings' bytes concatenated in id order, in the database code page.
  Lengths are **bytes**, not characters.
- **An empty string is never stored**: writing `""` into a cell stores null.
- With more than 65535 distinct strings the header's bit 31 is set and every string reference in
  every table becomes 3 bytes (low 16 bits, then the high byte).

### Code page

The database code page is the pool header's low bits: 0 when never set, otherwise what was set
through the `_ForceCodepage` import. **65001 (UTF-8) works for every table** on current Windows:
Korean, Japanese and supplementary-plane (e.g. U+20000, U+1F600) file and folder names, registry
keys and values, shortcut names and the product name all install with exactly the right UTF-16
text, and dialogs display them - also under a system locale whose ANSI code page cannot hold
Korean (en-US, 1252), so no separate Unicode setup program is needed. A UTF-16 code page (1200) is
not possible here: the pool holds single-byte-unit strings. [observed] Windows Installer does not normalize names: a
folder named with decomposed Hangul (U+1100 U+1161) is installed with exactly those code units,
next to - not instead of - one named U+AC00. [observed]

## System tables

| Table | Columns | Key |
|---|---|---|
| `_Tables` | `Name` (string) | `Name` |
| `_Columns` | `Table` (string), `Number` (i2, 1-based), `Name` (string), `Type` (i2) | `Table`, `Number` |

`_Tables` does not list itself or `_Columns`. A table with no rows has no stream at all, only
its `_Tables` and `_Columns` rows. `_Validation`, when present, is an ordinary table.

### Column type bits (`_Columns.Type`)

| Bits | Meaning |
|---|---|
| `0x00FF` | width: maximum string length (0 = unlimited), or 2 / 4 for integers |
| `0x0100` | valid |
| `0x0200` | localizable |
| `0x0400` | non-binary (set for text, clear for `OBJECT`) |
| `0x0800` | string (set for text and `OBJECT`) |
| `0x1000` | nullable |
| `0x2000` | part of the primary key |

Values `msi.dll` uses for common SQL column types:

| SQL | Type |
|---|---|
| `CHAR(72) NOT NULL` (key) | `0x2D48` |
| `CHAR(72)` (nullable) | `0x1D48` |
| `CHAR(255) NOT NULL LOCALIZABLE` | `0x0FFF` |
| `LONGCHAR NOT NULL LOCALIZABLE` | `0x0F00` |
| `SHORT NOT NULL` | `0x0502` |
| `SHORT` (nullable) | `0x1502` |
| `SHORT NOT NULL` (key) | `0x2502` |
| `LONG NOT NULL` | `0x0104` |
| `OBJECT NOT NULL` | `0x0900` |

Integer columns do not have the non-binary bit.

## Table streams

- A table stream is **column-major**: every row's cell of column 1, then every row's cell of
  column 2, and so on. The row count is the stream size divided by the row width.
- Cell widths: string reference 2 bytes (3 with long references), 16-bit integer 2 bytes, 32-bit
  integer 4 bytes, `OBJECT` 2 bytes.
- Integers are stored with the sign bit flipped: `stored = value ^ 0x8000` (16-bit) or
  `value ^ 0x80000000` (32-bit). **Stored 0 means null.** Consequently -32768 and -2147483648
  cannot be stored.
- An `OBJECT` cell stores 1 when a stream exists, 0 for null.
- **Rows are sorted by the primary-key cells' stored numbers** - string ids, flipped integers -
  key columns in order. Windows Installer relies on this order.

A writer that assigns string ids in byte order of the strings gets rows sorted by string value
for free, which is also the order an IDT export uses.

## Transforms (`.mst`)

A transform is a compound file of the same kind, of class `{000C1082-0000-0000-C000-000000000046}`
(a database is `{000C1084-...}`), that holds only differences: a string pool of its own, a stream
per changed table, a stream per binary cell it adds or changes, and summary information.

- A transform's table stream is **row-major**: records one after another, each starting with a
  16-bit mask. `0` deletes the row whose key cells follow. An odd mask inserts (or replaces) a
  whole row; its high byte is the number of cells that follow. Any other mask updates a row: the
  key cells, then the cells of the columns whose bits are set (bit n for column n + 1).
- Cells are stored as in a database - flipped integers, string ids (of the transform's pool), an
  `OBJECT` marker 1 whose bytes are the transform's stream `<Table>.<key>`.
- A new table is a `_Tables` insert and one `_Columns` insert per column, with a **null** Number:
  Windows numbers the columns in record order. A dropped table is a `_Tables` delete.
- msi.dll writes a table's records as the target's rows in key order, then the deletes in key
  order.
- Summary information: property 7 is the base's platform and languages, 8 the target's, 9
  `{base ProductCode}version;{target ProductCode}version;{UpgradeCode}`, 14 the minimum
  Windows Installer version, 16 the checks before applying (high word, e.g. `0x0802` product and
  upgrade code) and the errors to ignore (low word).
- A transform cannot be read alone: its records carry no column types, which come from the base.

## IDT archive files (what `MsiDatabaseExport` writes)

Useful as a reference output: a reader that exports byte-identical IDT files decodes the
database the way Windows does.

```text
line 1: column names separated by TAB
line 2: column types: s/S (string), l/L (localizable string), i/I (integer), v/V (binary),
        upper case = nullable, followed by the width (0 = unlimited; 2 or 4 for integers)
line 3: [<code page> TAB] <table name> TAB <key column> [TAB <key column> ...]
rows  : cells separated by TAB, lines end with CR LF
```

- The code page prefix on line 3 appears **only when the table holds a non-ASCII string**.
- Rows are sorted by primary-key **values** (strings by bytes, integers signed) - not by stored
  order.
- Null prints as an empty field. Binary cells print `<key>.ibd`; the bytes go to `<Table>/<key>.ibd`.
- TAB, CR and LF inside values are written as `0x10`, `0x11` and `0x19`. [spec]
- `_ForceCodepage.idt` is two empty lines then `<code page> TAB _ForceCodepage`.
- `_SummaryInformation.idt` has header lines `PropertyId TAB Value`, `i2 TAB l255`,
  `_SummaryInformation TAB PropertyId` and one row per property. Dates are printed
  `yyyy/mm/dd hh:mm:ss` in the **local time of the machine doing the export**.

## Reading safely

Check that the pool lengths add up to exactly the size of `_StringData`, that every string
reference in a table points to a used id, that table stream sizes are a multiple of the row
width, and that `_Columns` numbers each table's columns 1..n without gaps.

## Worked example: the tutorial's hello.msi

The table `File` of the tutorial's first package (see
[cfb.md](cfb.md#worked-example-the-tutorials-hellomsi)) is the stream named `U+4840` `U+430F`
`U+422F`:

```text
0x4840  (table)
F i: 0x3800 + 15 + (44 << 6) = 0x430F
l e: 0x3800 + 47 + (40 << 6) = 0x422F
```

`_StringPool` starts with `e9 fd 00 00`: the code page 65001 (UTF-8), bit 31 clear (2-byte string
references). Then two u16 per string id; the first ids:

| Id | Bytes | Length | Refs | String |
|---|---|---|---|---|
| 1 | `09 00 01 00` | 9 | 1 | `#cab1.cab` |
| 2 | `01 00 01 00` | 1 | 1 | `.` |
| 3 | `01 00 01 00` | 1 | 1 | `1` |
| 4 | `05 00 03 00` | 5 | 3 | `1.0.0` |
| 5 | `07 00 01 00` | 7 | 1 | `1.2.3.4` |
| 6 | `04 00 02 00` | 4 | 2 | `1033` |

132 strings in all; their bytes, back to back, are the 1544 bytes of `_StringData`.

`_Columns` describes the table - its rows for `File`, the type decoded with the bit table above:

| # | Column | Type | Meaning |
|---|---|---|---|
| 1 | `File` | `0x2D48` | key, string, non-binary, valid, width 72 |
| 2 | `Component_` | `0x0D48` | string, non-binary, valid, width 72 |
| 3 | `FileName` | `0x0FFF` | string, non-binary, localizable, valid, width 255 |
| 4 | `FileSize` | `0x0104` | valid, width 4 |
| 5 | `Version` | `0x1D48` | nullable, string, non-binary, valid, width 72 |
| 6 | `Language` | `0x1D14` | nullable, string, non-binary, valid, width 20 |
| 7 | `Attributes` | `0x1502` | nullable, non-binary, valid, width 2 |
| 8 | `Sequence` | `0x0104` | valid, width 4 |

The `File` stream itself is 20 bytes: one row, stored column by column:

```text
39 00 0e 00 81 00 00 46 00 80 05 00 06 00 00 82 01 00 00 80
```

| Column | Stored |
|---|---|
| `File` | `39 00` -> id 57 `Hello` |
| `Component_` | `0e 00` -> id 14 `C_185f8db32271fe25f561` |
| `FileName` | `81 00` -> id 129 `hello.exe` |
| `FileSize` | `00 46 00 80` -> 0x80004600 ^ 0x80000000 = 17920 |
| `Version` | `05 00` -> id 5 `1.2.3.4` |
| `Language` | `06 00` -> id 6 `1033` |
| `Attributes` | `00 82` -> 0x8200 ^ 0x8000 = 512 |
| `Sequence` | `01 00 00 80` -> 0x80000001 ^ 0x80000000 = 1 |

String cells are string ids; integers are stored with the top bit flipped, so 17920 (`0x00004600`)
is stored `0x80004600` and 0 stays free to mean null.
