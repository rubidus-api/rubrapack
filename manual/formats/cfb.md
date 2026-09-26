# Compound File Binary (CFB)

An `.msi` file is a Compound File Binary file ("structured storage"): a small file system of
named streams inside one file. The format is specified in [MS-CFB]; this page is the part an MSI
writer needs, plus what Windows' own writer does.

## Sectors

- The file is a sequence of fixed-size sectors. **Version 3** uses 512-byte sectors (sector
  shift 9), **version 4** uses 4096-byte sectors (shift 12). [spec]
- The header is at offset 0. Sector number `N` starts at file offset `(N + 1) * sector_size`
  in both versions; in version 4 the header therefore occupies a whole 4096-byte sector whose
  bytes after the header are zero. [spec]
- `msi.dll` writes **version 4** for every new database, whatever its size (20 KB to 10 MB
  were tried), with minor version `0x003E`. Either version is accepted when installing; rubrapack
  writes version 4 by default and can write version 3. [observed]

Special sector numbers in chains and tables: [spec]

| Value | Meaning |
|---|---|
| `FFFFFFFA` | largest regular sector number |
| `FFFFFFFC` | DIFAT sector (in the FAT) |
| `FFFFFFFD` | FAT sector (in the FAT) |
| `FFFFFFFE` | end of chain |
| `FFFFFFFF` | free sector / no stream |

## Header (first 512 bytes) [spec]

| Offset | Size | Field | Value written |
|---|---|---|---|
| 0 | 8 | signature | `D0 CF 11 E0 A1 B1 1A E1` |
| 8 | 16 | CLSID | zero |
| 24 | 2 | minor version | `003E` |
| 26 | 2 | major version | 3 or 4 |
| 28 | 2 | byte order | `FFFE` (bytes `FE FF`) |
| 30 | 2 | sector shift | 9 or 12 |
| 32 | 2 | mini sector shift | 6 (64-byte mini sectors) |
| 34 | 6 | reserved | zero |
| 40 | 4 | number of directory sectors | 0 in version 3; the count in version 4 |
| 44 | 4 | number of FAT sectors | |
| 48 | 4 | first directory sector | |
| 52 | 4 | transaction signature | 0 |
| 56 | 4 | mini stream cutoff | 4096 |
| 60 | 4 | first mini FAT sector | `FFFFFFFE` if none |
| 64 | 4 | number of mini FAT sectors | |
| 68 | 4 | first DIFAT sector | `FFFFFFFE` if none |
| 72 | 4 | number of DIFAT sectors | |
| 76 | 436 | first 109 FAT sector numbers (DIFAT) | unused entries `FFFFFFFF` |

## FAT, DIFAT, mini FAT [spec]

- The **FAT** is an array of u32, one entry per sector, giving the next sector of the chain the
  sector belongs to. FAT sectors themselves are marked `FFFFFFFD`.
- The FAT's own sector numbers are listed in the **DIFAT**: the first 109 in the header, the rest
  in DIFAT sectors, each holding `sector_size / 4 - 1` numbers and, in its last u32, the next DIFAT
  sector (`FFFFFFFE` at the end). DIFAT sectors are marked `FFFFFFFC` in the FAT.
- With 4096-byte sectors, 109 FAT sectors already cover about 457 MB, so real MSI files in
  version 4 have no DIFAT sectors. With 512-byte sectors DIFAT starts at about 7 MB.
- Streams smaller than the **cutoff (4096 bytes)** live in the **mini stream**, in 64-byte mini
  sectors chained through the **mini FAT** (same layout as the FAT). The mini stream is itself
  stored as the root directory entry's own regular stream.

The FAT must cover its own sectors and the DIFAT's; a writer computes the counts by iterating
until they stop changing.

## Directory [spec]

The directory is a chain of sectors holding 128-byte entries. Entry 0 is the root.

| Offset | Size | Field |
|---|---|---|
| 0 | 64 | name, UTF-16LE, NUL-terminated (at most 31 characters + NUL) |
| 64 | 2 | name length in bytes, including the NUL |
| 66 | 1 | type: 0 unused, 1 storage, 2 stream, 5 root |
| 67 | 1 | colour: 0 red, 1 black |
| 68 | 4 | left sibling (`FFFFFFFF` = none) |
| 72 | 4 | right sibling |
| 76 | 4 | child (storages and root only) |
| 80 | 16 | CLSID |
| 96 | 4 | state bits |
| 100 | 8 | creation time |
| 108 | 8 | modification time |
| 116 | 4 | starting sector (regular or mini, by size) |
| 120 | 8 | stream size (in version 3 only the low 32 bits count) |

- The root entry is named `Root Entry`; its starting sector and size describe the mini stream.
- The root entry of an MSI carries the CLSID `{000C1084-0000-0000-C000-000000000046}`, stored as
  bytes `84 10 0C 00 00 00 00 00 C0 00 00 00 00 00 00 46`. [spec] [observed]
- Unused entries are all zero except the three link fields, which are `FFFFFFFF`. [spec]
- The children of a storage form a **red-black tree** ordered first by name length, then by
  comparing the upper-cased UTF-16 names. [MS-CFB] allows the simplest valid tree: every node
  black, the tree a plain balanced binary search tree. rubrapack builds that - a balanced tree
  over the sorted names, all nodes black - and `msi.dll` opens it. [spec] [observed]
- Times may be zero. Writing zero times (and no other clock input) is what makes a CFB writer
  deterministic. [spec]

## A simple, deterministic writer

rubrapack lays the file out in this order, all runs contiguous:

```text
header | FAT sectors | DIFAT sectors | directory | mini FAT | mini stream | large streams in order
```

1. Sort stream sizes into "mini" (< 4096) and "regular".
2. Mini streams are packed back to back in 64-byte units; the mini stream length is the sum.
3. Count directory, mini FAT and mini stream sectors, then FAT and DIFAT sectors (iterate).
4. Fill FAT chains for each contiguous run, write the directory tree, mini FAT, FAT, DIFAT, header.

## Reading safely

A reader must refuse, rather than follow: chains that loop (a chain never reaching `FFFFFFFE`
within the number of sectors in the file), sector numbers beyond the file, a chain shorter than
the stream size needs, directory links out of range, sibling trees that visit a node twice, and
counts beyond sane limits. None of this costs much, and a crafted `.msi` otherwise hangs or
crashes the reader.
