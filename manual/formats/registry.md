# Registry hive files (REGF)

The file format of `Registry.dat` and `User.dat` in an MSIX package. Microsoft does not publish it;
every fact here is **[observed]**: hives written by Windows' Offline Registry Library (offreg.dll,
10.0.26100, the writer Windows' packaging tools use - its mark is in the base block) for chosen
key/value sets, and rubrapack's hives for the same sets read back on Windows with `OROpenHive` and
`RegLoadAppKey` into exactly the keys and values Windows enumerates in its own. Only what a
freshly written hive needs is described: no transaction logs, no volatile keys, no free-cell reuse.

## Layout

- A 4096-byte base block, then hive bins. Offsets in cells ("cell offsets") count from the end of
  the base block (file offset - 4096); "none" is 0xFFFFFFFF.
- A bin: `hbin`, u32 its own cell offset, u32 its size (4096 or a multiple), 8 bytes zero, u64 time
  (0), u32 spare (0); 32 bytes in all. Cells follow; a cell that does not fit in the current bin
  starts a new bin sized to hold it; the rest of a bin is one free cell.
- A cell: i32 size (negative while in use, a multiple of 8, the size field included), then its
  content. A cell never crosses its bin's end.

## The base block

| Offset | Field | Value |
|---|---|---|
| 0 | signature | `regf` |
| 4, 8 | sequence numbers | 1, 1 (unequal = not closed cleanly; readers refuse or replay a log) |
| 12 | u64 time | 0 is accepted |
| 20, 24 | version | 1, 5 |
| 28, 32 | type, format | 0, 1 |
| 36 | root key cell offset | |
| 40 | size of all bins | |
| 44 | clustering | 1 |
| 0xB0 | offreg's mark | `OfRg`, then u32 1 |
| 508 | checksum | XOR of the 127 dwords before it; 0 is written as 1, 0xFFFFFFFF as 0xFFFFFFFE |

Everything else is zero.

## Keys (`nk`)

`nk`, u16 flags, u64 time, u32 access bits (0), u32 parent (none for the root), u32 subkey count,
u32 volatile subkey count (0), u32 subkey list, u32 volatile list (none), u32 value count, u32 value
list, u32 security cell, u32 class (none), u32 longest subkey name, u32 longest class (0), u32
longest value name, u32 largest value data, u32 work var (0), u16 name length, u16 class length (0),
then the name.

- Flags: 0x20 when the name is stored as Latin-1 ("compressed": every UTF-16 unit below 256), else
  the name is UTF-16LE. The root key of an offreg hive is named `ROOT` and has flags 0x20 only.
- The longest names are counted in UTF-16 bytes, whatever the stored form.
- Names compare without case: each UTF-16 unit mapped by Unicode simple upper case.

## Subkey lists (`lh`, `ri`)

- `lh`, u16 count, then per subkey: u32 key cell, u32 hash. Sorted by upper-cased name.
- Hash: h = 0; for each UTF-16 unit of the name, h = h * 37 + upper(unit) (32-bit wrap).
- More than 507 subkeys: an `ri` (u16 count, u32 per `lh` list) of `lh` lists of up to 507.

## Values (`vk`) and value lists

- A value list is a plain cell of u32 value cell offsets, in the order the values were set.
- `vk`, u16 name length, u32 data size, u32 data offset, u32 type, u16 flags, u16 spare, then the
  name. Flag 1 = Latin-1 name; the unnamed default value has an empty name and flags 0.
- Data of up to 4 bytes (an empty value too) is kept in the data offset field, and the size's top
  bit is set.
- Up to 16344 bytes: one data cell. From 16345 bytes: a `db` cell (`db`, u16 segment count, u32
  segment list), a list cell of u32 segment cells, and segments of 16344 bytes each - every segment
  cell allocated at full size, the last one too: Windows stops reading a value whose last segment
  cell is shorter.

## The security cell (`sk`)

One `sk` cell shared by every key: `sk`, u16 0, u32 next and u32 previous (both itself), u32
reference count (the number of keys), u32 descriptor length, then a self-relative security
descriptor. offreg's: owner and group Administrators; SYSTEM and Administrators full access,
Everyone and RESTRICTED read, all container-inherit.

## Reproducible hives

With every time field 0 and cells laid out depth first (a key; after the root key the security
cell; the key's subkey lists, its value list, each value followed by its data; then its subkeys in
sorted order), the same keys and values give the same bytes on any host. Windows reads such a hive without complaint.
