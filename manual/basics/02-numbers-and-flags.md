# Numbers in files, and bit flags

A file format stores numbers - sizes, counts, positions, versions - as a fixed number of bytes.
This chapter shows how, and how one number can carry many yes/no settings at once.

## One, two, four, eight bytes

A number stored in a file takes a fixed number of bytes, chosen by the format:

| Size | Bits | Unsigned range | Common names |
|---|---|---|---|
| 1 byte | 8 | 0 to 255 | byte, `u8`, `uint8` |
| 2 bytes | 16 | 0 to 65,535 | word, `u16`, `uint16`, short |
| 4 bytes | 32 | 0 to 4,294,967,295 | double word (DWORD), `u32`, `uint32`, int |
| 8 bytes | 64 | 0 to about 1.8 x 10^19 | quad word (QWORD), `u64`, `uint64` |

*Unsigned* means no negative numbers. A number too big for its size cannot be stored - which is
why an MSI table column declared `i2` (2 bytes) cannot hold 100,000, and why a Compound File,
whose positions are 4-byte numbers of 4 KiB sectors, has an upper size limit.

## Byte order: little-endian

A number of several bytes is split into bytes. Take the size of `hello.exe` in the tutorial, 17,920
bytes. In hex it is `0x00004600` - as four bytes, `00 00 46 00`, the most significant first. But
files for Windows almost always store the bytes the other way round, **least significant first**:

```text
  17920 = 0x00004600

  most significant first (big-endian):     00 00 46 00
  least significant first (little-endian): 00 46 00 00
                                           |  |  |  |
                                           |  |  |  +-- x 16,777,216 (256^3) = 0
                                           |  |  +----- x 65,536     (256^2) = 0
                                           |  +-------- x 256                = 17,920  (0x46 = 70)
                                           +----------- x 1                  = 0
```

This order is called **little-endian** ("the little end first"); the other is **big-endian**. Intel
and Arm processors in Windows computers work little-endian, and so do the MSI, CAB, ZIP and
program file formats. Network protocols and certificates (chapter 8) use big-endian. When a Part IV
table says "u32 LE", it means "an unsigned 4-byte number, little-endian".

To read a little-endian number from a dump: take its bytes, reverse them, read the result as hex.
From the Compound File header of chapter 1:

| Offset | Bytes | Reversed | Value | Meaning |
|---|---|---|---|---|
| `0x18` | `3e 00` | `00 3e` | 62 | minor version |
| `0x1A` | `04 00` | `00 04` | 4 | major version: 4 KiB sectors |
| `0x1C` | `fe ff` | `ff fe` | 0xFFFE | the byte order mark: a reader that gets 0xFEFF knows it read the wrong way round |
| `0x1E` | `0c 00` | `00 0c` | 12 | sector size is 2^12 = 4096 |
| `0x38` | `00 10 00 00` | `00 00 10 00` | 4096 | streams smaller than this go into the mini stream |

## Negative numbers: two's complement

Where a format needs negative numbers, it uses **two's complement**: the same bits, with the top
half of the range read as negative. In one byte, 0 to 127 are themselves and 128 to 255 stand for
-128 to -1:

| Byte | Unsigned | Signed |
|---|---|---|
| `00` | 0 | 0 |
| `7f` | 127 | 127 |
| `80` | 128 | -128 |
| `fe` | 254 | -2 |
| `ff` | 255 | -1 |

The rule: a signed value is negative when its top bit is 1, and its value is the unsigned value
minus 2^*bits*. In four bytes, `ff ff ff ff` is -1 and `fe ff ff ff` is -2. Formats like to use
such "all ones" values as markers: in a Compound File, the sector number `0xFFFFFFFE` (-2) means
"end of the chain" and `0xFFFFFFFF` (-1) "free".

## Bit flags

Often a format needs many yes/no settings for one thing. Rather than a byte for each, it gives
each setting one **bit** of a number, and the number is the sum of the bits that are on. Each bit
is worth a power of two - 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, ... - so every sum
has exactly one way to be made, and the settings can be read back from it.

The tutorial met several such numbers. Here they are taken apart - write the number in binary, and
each `1` is one setting:

| Number | Binary | Bits on | Meaning (tutorial chapter) |
|---|---|---|---|
| 258 | `1 0000 0010` | 256 + 2 | Upgrade row: minimum version included, only detect (3) |
| 272 | `1 0001 0000` | 256 + 16 | Component: 64-bit, permanent (4) |
| 512 | `10 0000 0000` | 512 | File: vital (2, 18) |
| 163 | `1010 0011` | 128 + 32 + 2 + 1 | ServiceControl: delete at removal, stop at removal, stop at install, start at install (12) |
| 3090 | `1100 0001 0010` | 2048 + 1024 + 16 + 2 | Custom action: no impersonation, deferred, source is a File, an `.exe` (13) |
| 3410 | `1101 0101 0010` | 2048 + 1024 + 256 + 64 + 16 + 2 | the same, plus rollback and ignore the exit code (13) |

To take a number apart by hand, subtract the largest power of two that fits, and repeat:
3090 - 2048 = 1042; 1042 - 1024 = 18; 18 - 16 = 2; 2 - 2 = 0. So 3090 = 2048 + 1024 + 16 + 2.
(Microsoft's documentation calls the custom action's 2 + 16 together "type 18": run an `.exe`
that the package installs.)

Hex makes flags easy to see, because each hex digit is four bits: 3090 = `0xC12`, and `C` =
`1100` means 2048 and 1024, `1` = `0001` means 16, `2` = `0010` means 2.

## Testing and setting a bit: AND, OR

Programs test a flag with **AND**: keep only the bits that are 1 in both numbers.

```text
     3090 = 1100 0001 0010
  AND 1024 = 0100 0000 0000
          = 0100 0000 0000   not zero: "deferred" is on
```

They set one with **OR** (a bit is 1 if it is 1 in either) and clear one with AND of the
opposite. Windows Installer conditions (tutorial chapter 9) have the same test as the operator
`><`: `MYFLAGS >< 2` is true when the bit worth 2 is on in the property `MYFLAGS`. You will not
need these for rubrapack - it computes all these numbers - but they explain why the numbers look
the way they do.

## Where this is used

- Every "u16", "u32 LE" in Part IV, starting with the header and sector chains of
  [Compound File Binary](../formats/cfb.md).
- Tutorial chapters [2](../tutorial/02-a-first-installer.md),
  [3](../tutorial/03-versions-and-upgrades.md), [4](../tutorial/04-files-and-folders.md),
  [12](../tutorial/12-services-fonts-permissions.md), [13](../tutorial/13-running-your-program.md):
  the numbers in the tables.
