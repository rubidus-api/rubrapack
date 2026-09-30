# Files inside a file

A package is one file that holds many: an `.msi` holds its tables, the summary and the cabinet; an
`.msix` holds the program files, the logos and the manifest. There are two classic ways to put
files inside a file - as a small **file system** or as an **archive** - and the two package
formats use one each. This chapter explains both with the tutorial's own packages.

## How a disk stores files

A disk is divided into equal pieces, **sectors** (or *clusters*) - say 4096 bytes each - numbered
0, 1, 2, ... A file rarely fits in one, and the sectors it gets need not be next to each other. So a
file system keeps two things besides the data:

- a **directory**: for each file its name, its size, and its *first* sector;
- an **allocation table**: for each sector, the number of the *next* sector of the same file, or a
  marker for "last sector" or "free".

To read a file, look up its first sector in the directory, then follow the table from sector to
sector - a **chain** - until the "last" marker:

```text
  directory:  report.txt   size 10,000   first sector 4

  allocation table:
    sector:  0     1     2     3     4     5     6     7
    next:    -     -     -     -     5     7     free  end

  report.txt = sector 4 -> sector 5 -> sector 7 -> end    (3 x 4096 bytes, the last one partly used)
```

This is how the FAT file system of USB sticks works (FAT: *file allocation table*). Deleting a file
only marks its sectors free; growing one only links more sectors to its chain.

## An MSI is a file system in a file

An `.msi` is a **Compound File** (Microsoft's "structured storage"): exactly the scheme above,
inside one file. Here is the first `hello.msi` of the tutorial, 32,768 bytes, taken apart:

```text
  offset 0x0000  header           (the 512-byte header, padded to one 4096-byte sector)
  offset 0x1000  sector 0         the allocation table (FAT)
  offset 0x2000  sector 1         the directory
  offset 0x3000  sector 2         the mini FAT (see below)
  offset 0x4000  sector 3         the mini stream (see below), first part
  offset 0x5000  sector 4         the mini stream, second part
  offset 0x6000  sector 5         cab1.cab, first part
  offset 0x7000  sector 6         cab1.cab, second part
```

Sector *n* starts at byte (*n* + 1) x 4096, because the header takes the first 4096 bytes. The
header (chapter 1) says where the rest is: `01 00 00 00` at offset `0x30` - the directory starts at
sector 1. The allocation table in sector 0 reads, as 4-byte little-endian numbers (chapter 2):

| Sector | Entry | Meaning |
|---|---|---|
| 0 | `0xFFFFFFFD` | this sector holds the allocation table itself |
| 1 | `0xFFFFFFFE` | end of chain: the directory is one sector |
| 2 | `0xFFFFFFFE` | end of chain: the mini FAT is one sector |
| 3 | `4` | next is sector 4 |
| 4 | `0xFFFFFFFE` | end of chain: the mini stream is sectors 3 and 4 |
| 5 | `6` | next is sector 6 |
| 6 | `0xFFFFFFFE` | end of chain: `cab1.cab` is sectors 5 and 6 |
| 7 ... | `0xFFFFFFFF` | free (there are no more sectors) |

A file inside is called a **stream**. The directory has one 128-byte entry per stream: its name in
UTF-16 (chapter 3), its type, its first sector and its size. The first entry is always the root:

```text
  52 00 6f 00 6f 00 74 00 20 00 45 00 6e 00 74 00 72 00 79 00 00 00 ...
   R     o     o     t           E     n     t     r     y   (end)
```

### Small streams: the mini stream

A 4096-byte sector would waste most of its space on a 20-byte table. So streams smaller than 4096
bytes (the number at offset `0x38` of the header) are kept together in one stream, the **mini
stream**, cut into **mini sectors** of 64 bytes, with their own table, the **mini FAT**. In this
package, twenty streams - nineteen tables of 4 to 1,896 bytes and the 332-byte summary - share the
6,144-byte mini stream in sectors 3 and 4; only the 6,776-byte cabinet is big enough for ordinary
sectors. It is the same scheme again, one level
down.

### The names

The stream names of an MSI look strange - `䡀㼿䕷䑬㹪䒲䠯` - because Windows Installer packs each
table name into characters of the CJK range to fit more letters in the 31 characters a name may
have. `rubrapack inspect hello.msi --streams` (tutorial chapter 18) shows them decoded: `File`,
`Component`, `cab1.cab`, ... One name is plain: `\005SummaryInformation`, the summary, which every
Compound File tool can read.

## An MSIX is an archive

An **archive** is simpler than a file system: the files are written one after the other, each after
a small header, and a list at the end says where each one is. It cannot grow a file in place, but a
package never needs to. The ZIP format, which MSIX uses, looks like this:

```text
  "PK 03 04"  local header: name, sizes, method   + the file's (compressed) bytes     hello.exe
  "PK 03 04"  local header                        + bytes                             guide.txt
  ...                                                                                  (9 files)
  "PK 01 02"  central directory: one entry per file, with where its local header is
  "PK 06 06"  end record: how many files, where the central directory starts
```

The tutorial's `hello.msix` (11,009 bytes) begins with `50 4b 03 04` - "PK", the initials of Phil
Katz, who made ZIP - followed by the name `hello.exe`. Its central directory starts at byte 10,098
and lists nine entries; a reader goes to the end of the file first, finds the end record, and from
there the list. (MSIX uses the ZIP64 form of the end record, `PK 06 06`, made for archives above
4 GiB.)

Each entry says its method (chapter 5): 8 for deflate, 0 for stored - here 0 for the three PNG
logos, 8 for everything else.

## Why two designs?

| | Compound File (MSI) | ZIP (MSIX) |
|---|---|---|
| Made for | documents edited in place (Word, Excel of the 1990s) | files packed once, read many times |
| Changing a stream | rewrite its chain | rewrite the archive |
| Small parts | cheap, in the mini stream | each has a header of 30+ bytes |
| Reading one part | follow its chain | look it up in the central directory |

Windows Installer was built in 1999 on the storage Office used; MSIX in 2018 on the packaging
standard (OPC) that `.docx` files use - which is a ZIP. Both are "files inside a file".

## Where this is used

- Tutorial chapter [18](../tutorial/18-checking-and-looking-inside.md): `inspect --streams`,
  `extract`.
- Part IV: [Compound File Binary](../formats/cfb.md), [the MSI database](../formats/msi-database.md)
  (stream names), [MSIX](../formats/msix.md) (ZIP layout).
