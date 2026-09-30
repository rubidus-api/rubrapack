# Bits, bytes and hexadecimal

This part explains the ideas the rest of the manual stands on, from the beginning. Nothing is
assumed beyond using a computer. Each chapter ends with the places in the tutorial and in Part IV
where its ideas are used.

## Bits

Everything a computer stores - a program, a photo, this manual, an installer - is in the end a
long row of **bits**. A bit has two values, written `0` and `1` (in the hardware: no charge or
charge, one magnetic direction or the other). One bit can answer one yes/no question.

Several bits together can tell more values apart. Each bit added doubles the count:

| Bits | Different values | All of them |
|---|---|---|
| 1 | 2 | `0` `1` |
| 2 | 4 | `00` `01` `10` `11` |
| 3 | 8 | `000` `001` `010` `011` `100` `101` `110` `111` |
| 8 | 256 | `00000000` ... `11111111` |
| 16 | 65,536 | |
| 32 | 4,294,967,296 | |

With *n* bits there are 2 x 2 x ... x 2 (*n* times) = 2^*n* values.

## Binary numbers

A row of bits can be read as a number, the same way we read decimal digits. In decimal, each
place is worth ten times the place to its right: `305` is 3 x 100 + 0 x 10 + 5 x 1. In
**binary** (base 2), each place is worth two times the place to its right:

```text
  bit:      1    0    1    1    0    0    1    0
  worth:  128   64   32   16    8    4    2    1
          ---        --   --              -
          128    +   32 + 16        +     2      = 178
```

So `10110010` in binary is 178 in decimal. The rightmost bit, worth 1, is called bit 0; the
leftmost of these eight, worth 128, is bit 7. Bit *k* is worth 2^*k*.

## Bytes

Bits are grouped by eight into **bytes**. A byte is the smallest piece a file is made of: a file of
28,672 bytes - the size of the first `hello.msi` of the tutorial - is 28,672 x 8 = 229,376 bits.
A byte holds one of 256 values, 0 to 255.

What a byte *means* depends entirely on how a program reads it: the same byte `01001000` is the
number 72, or the letter `H` in a text file (chapter 3), or part of a larger number (chapter 2), or
part of a machine instruction in a program. The file does not say; the file's *format* does. Part
IV of this manual is the description of such formats.

## Hexadecimal

Binary is long to write and hard to read. Decimal does not line up with bytes. So bytes are
usually written in **hexadecimal** (base 16, "hex"): digits `0`-`9` and then `A`-`F` for ten to
fifteen. One hex digit is exactly four bits, so one byte is exactly two hex digits:

| Decimal | Binary | Hex | | Decimal | Binary | Hex |
|---|---|---|---|---|---|---|
| 0 | `0000` | `0` | | 8 | `1000` | `8` |
| 1 | `0001` | `1` | | 9 | `1001` | `9` |
| 2 | `0010` | `2` | | 10 | `1010` | `A` |
| 3 | `0011` | `3` | | 11 | `1011` | `B` |
| 4 | `0100` | `4` | | 12 | `1100` | `C` |
| 5 | `0101` | `5` | | 13 | `1101` | `D` |
| 6 | `0110` | `6` | | 14 | `1110` | `E` |
| 7 | `0111` | `7` | | 15 | `1111` | `F` |

To turn a byte into hex, split its eight bits into two halves of four and look each up:

```text
  10110010
  1011 0010
     B    2      -> B2
```

And back: hex `B2` is B x 16 + 2 = 11 x 16 + 2 = 178. The same number three ways: `178`,
`10110010`, `B2`.

To make clear that a number is hex, programmers write `0x` before it (`0xB2`), or `h` after it
(`B2h`), or just say so. This manual writes bytes as two hex digits with spaces between them,
`B2 00 FF`, and numbers in hex with `0x`: `0x1000` is 4096.

Windows' Calculator has a *Programmer* mode that converts between decimal, hex and binary: type a
number in one and read the others.

## Reading a hex dump

A **hex dump** shows a file's bytes in hex, sixteen to a line. Here are the first 64 bytes of the
first `hello.msi` of the tutorial:

```text
offset    bytes                                            as text
00000000  d0 cf 11 e0 a1 b1 1a e1 00 00 00 00 00 00 00 00  ................
00000010  00 00 00 00 00 00 00 00 3e 00 04 00 fe ff 0c 00  ........>.......
00000020  06 00 00 00 00 00 00 00 01 00 00 00 01 00 00 00  ................
00000030  01 00 00 00 00 00 00 00 00 10 00 00 02 00 00 00  ................
```

- The **offset** on the left is the position of the line's first byte, counted from 0, in hex:
  `00000010` is byte 16, `00000030` byte 48. The byte at offset `0x1C` is the thirteenth of the
  second line: `fe`.
- The **bytes** follow, two hex digits each.
- The last column shows each byte as a letter where it is one (32 to 126, chapter 3) and `.`
  otherwise. Here there is hardly any text; in a text file the column would be readable.

The first eight bytes, `d0 cf 11 e0 a1 b1 1a e1`, are the *signature* of the Compound File format
that every `.msi` uses - read with some imagination, "DOCFILE". Every file format begins with such
a fixed pattern, a **magic number**, so programs can tell what a file is: a program (`.exe`) begins
with `4d 5a` ("MZ"), a ZIP archive and so an MSIX with `50 4b 03 04` ("PK"), a cabinet with
`4d 53 43 46` ("MSCF"). The file's name can lie; its first bytes rarely do.

To look at a file this way yourself, in PowerShell:

```text
PS C:\work\hello> Format-Hex hello.msi | Select-Object -First 4
```

(Its layout differs a little from the one above; the bytes are the same.)

## Units: KB, MB and KiB, MiB

Sizes are counted in bytes, with prefixes for large counts. There are two sets of prefixes, and
confusing them is common:

| Decimal (SI) | Bytes | Binary (IEC) | Bytes |
|---|---|---|---|
| 1 kB (kilobyte) | 1,000 | 1 KiB (kibibyte) | 1,024 = 2^10 |
| 1 MB (megabyte) | 1,000,000 | 1 MiB (mebibyte) | 1,048,576 = 2^20 |
| 1 GB (gigabyte) | 1,000,000,000 | 1 GiB (gibibyte) | 1,073,741,824 = 2^30 |
| 1 TB (terabyte) | 10^12 | 1 TiB (tebibyte) | 2^40 |

Binary sizes appear because memory and file formats work in powers of two. The dump above shows
one: at offset `0x1E` the Compound File header stores `0c 00`, the number 12, and its sectors are
2^12 = 4096 bytes (4 KiB, `0x1000`) long - the format stores the power, not the size. An MSIX block
map entry covers 64 KiB. Windows Explorer says "KB" and "MB"
but means KiB and MiB; disk makers mean the decimal ones, which is why a "1 TB" disk shows as
931 GB.

rubrapack uses the binary units where a limit is a power of two: `cab-max-size` is in MiB, and
Windows Installer's limit on an `.msi` is 2 GiB (2,147,483,648 bytes). When this manual writes
"MB" for a program's size (as in "a 200 MB program"), it is a rough size, and the difference does
not matter.

## Where this is used

- Every chapter of Part IV shows file contents as hex dumps and offsets, starting with
  [Compound File Binary](../formats/cfb.md).
- Tutorial chapter [15](../tutorial/15-cabinets-and-architectures.md): `cab-max-size` in MiB and
  the 2 GiB limit.
- Tutorial chapter [18](../tutorial/18-checking-and-looking-inside.md): file sizes in
  `inspect --files` are bytes.
