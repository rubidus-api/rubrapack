# Summary information

Every package has a stream `\x05SummaryInformation` (the first character is U+0005; the name is
not packed). It is an OLE property set as specified in [MS-OLEPS]; Microsoft Learn ("Summary
Information Stream Property Set") says which properties an installer package uses.

## Layout [spec] [observed]

```text
u16  byte order      FFFE
u16  version         0 (msi.dll writes 0)
u32  OS field        msi.dll writes 0x00020206
16   CLSID           zero
u32  section count   1
16   FMTID           F29F85E0-4FF9-1068-AB91-08002B27B3D9, stored E0 85 9F F2 F9 4F 68 10 AB 91 08 00 2B 27 B3 D9
u32  section offset  48

section:
u32  section size
u32  property count
count x (u32 property id, u32 offset from the section start)
values, each padded to a multiple of 4 bytes:
  VT_I2 (2)        u32 type, i16 value, 2 bytes padding
  VT_I4 (3)        u32 type, i32 value
  VT_LPSTR (30)    u32 type, u32 byte count including the NUL, bytes, NUL, padding
  VT_FILETIME (64) u32 type, u64 100-ns intervals since 1601-01-01 UTC
```

## Properties an installer package sets

| Id | Name | Type | Value |
|---|---|---|---|
| 2 | Title | LPSTR | e.g. `Installation Database` |
| 3 | Subject | LPSTR | product name |
| 4 | Author | LPSTR | manufacturer |
| 5 | Keywords | LPSTR | e.g. `Installer` |
| 7 | Template | LPSTR | `<platform>;<language id>`: `x64;1033`, `Arm64;1042`, `Intel;1033` |
| 9 | Revision Number | LPSTR | the **package code**: a GUID, new for every different package file |
| 12, 13 | Create / Last Save time | FILETIME | optional |
| 14 | Page Count | I4 | minimum installer version: 200 for x64 and x86, 500 for Arm64 |
| 15 | Word Count | I4 | bit 1 = files are compressed (in cabinets); 2 for a normal package |
| 18 | Creating Application | LPSTR | any |

## The code page trap - keep summary strings ASCII [observed]

Property 1 (`CodePage`, VT_I2) says how the LPSTR bytes are encoded. On Windows 11 25H2:

- **65001:** `msi.dll` writes the strings correctly as UTF-8, but reads the code page back as the
  signed value -535, fails to convert every string (the installation log says "Failed to retrieve
  # 9 summary info property. GetLastError returned 87, the code page is -535") and refuses the
  package with error 1620.
- **1200 (UTF-16):** refused the same way (log: "... the code page is 1200"), even with complete
  UTF-16 strings and byte counts that follow [MS-OLEPS].
- **No code page property and ASCII strings:** installs.

So: leave property 1 out and write every summary string in ASCII. Localized product names belong
in the `Property` table (`ProductName`), which uses the database code page - and 65001 works there
(see [msi-database.md](msi-database.md)).

## Worked example: the tutorial's hello.msi

The tutorial's first package: the stream is 332 bytes. Its header:

| Offset | Bytes | Field | Value |
|---|---|---|---|
| `0x00` | `fe ff` | byte order | 0xFFFE |
| `0x04` | `06 02 02 00` | OS | 0x00020206 |
| `0x18` | `01 00 00 00` | section count | 1 |
| `0x1C` | `e0 85 9f f2 ...` | FMTID | `F29F85E0-...` |
| `0x2C` | `30 00 00 00` | section offset | 48 |
| `0x30` | `1c 01 00 00` | section size | 284 |
| `0x34` | `09 00 00 00` | property count | 9 |

Then 9 pairs of (property id, offset), and the values:

| Property | Offset | Type | Raw bytes | Value |
|---|---|---|---|---|
| 2 | `0x80` | VT_LPSTR | `1e 00 00 00 16 00 00 00 49 6e 73 74 61 6c 6c 61 ...` | `Installation Database` |
| 3 | `0xA0` | VT_LPSTR | `1e 00 00 00 06 00 00 00 48 65 6c 6c 6f 00` | `Hello` |
| 4 | `0xB0` | VT_LPSTR | `1e 00 00 00 11 00 00 00 45 78 61 6d 70 6c 65 20 ...` | `Example Software` |
| 5 | `0xCC` | VT_LPSTR | `1e 00 00 00 0a 00 00 00 49 6e 73 74 61 6c 6c 65 ...` | `Installer` |
| 7 | `0xE0` | VT_LPSTR | `1e 00 00 00 09 00 00 00 78 36 34 3b 31 30 33 33 ...` | `x64;1033` |
| 9 | `0xF4` | VT_LPSTR | `1e 00 00 00 27 00 00 00 7b 31 31 38 39 31 45 36 ...` | `{11891E65-44B0-8DAC-A727-1C2A3FAF5877}` |
| 14 | `0x124` | VT_I4 | `03 00 00 00 c8 00 00 00` | 200 |
| 15 | `0x12C` | VT_I4 | `03 00 00 00 02 00 00 00` | 2 |
| 18 | `0x134` | VT_LPSTR | `1e 00 00 00 10 00 00 00 72 75 62 72 61 70 61 63 ...` | `rubrapack 0.8.0` |

A string value is its type (30), its byte count including the NUL, the bytes, the NUL, and padding
to a multiple of 4. Property 9, the package code, is derived from the content under
`--reproducible`; without it every build gets a new one.
