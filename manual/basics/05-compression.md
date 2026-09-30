# Compression

An MSI's files travel in a cabinet, an MSIX's in a ZIP archive, and in both they are usually
*compressed*: written in fewer bytes, from which the exact original comes back. This chapter shows
how the method both use, **deflate**, does that - down to the bits of a real example.

## Why data can be made smaller

Compression without loss works because real data is not random. Two things repeat:

1. **Sequences.** A program's machine code repeats instruction patterns; a text repeats words; a
   picture of a blue sky repeats the same colour.
2. **Symbols, unevenly.** In English text `e` is far more common than `z`; in a program the byte
   `00` is very common.

Deflate uses both, one after the other: **LZ77** replaces repeated sequences by short references,
then **Huffman coding** writes common symbols in fewer bits.

Data with neither - random numbers, or data that is compressed already, such as PNG and JPEG
pictures, ZIP files, video - cannot be made smaller. Trying costs time and adds a few bytes.

## LZ77: "copy from earlier"

LZ77 (Lempel and Ziv, 1977) reads the data from the start and, wherever the next bytes already
appeared recently, writes a **back-reference** instead: "go back *distance* bytes and copy
*length* bytes from there". Everything else is written as **literals**, byte by byte.

Take the 12 bytes `abcabcabcabc`. Deflate writes them as four literals and one reference:

```text
  literal a
  literal b
  literal c
  literal a
  copy: distance 3, length 8
```

The copy may overlap what it produces: going back 3 bytes lands on `bca`, and copying byte by byte
keeps reading bytes that were just written:

```text
  written so far:  a b c a
                     ^ start 3 back
  copy 8:          a b c a b c a b c a b c
                           \_______________/ 8 copied bytes
```

So a run of any length is one short reference. Deflate allows distances up to 32,768 bytes (the
*window*) and lengths from 3 to 258.

## Huffman coding: short codes for common symbols

After LZ77 the data is a series of symbols: literal bytes, lengths and distances. A normal byte
takes 8 bits whatever it is. A **Huffman code** gives each symbol its own number of bits - fewer for
frequent ones - such that no code is the beginning of another, so a reader always knows where one
ends.

A small example with four symbols:

| Symbol | Frequency | Fixed 2-bit code | Huffman code |
|---|---|---|---|
| A | 50% | `00` | `0` |
| B | 25% | `01` | `10` |
| C | 12.5% | `10` | `110` |
| D | 12.5% | `11` | `111` |

On average this takes 0.5 x 1 + 0.25 x 2 + 0.125 x 3 + 0.125 x 3 = 1.75 bits per symbol instead
of 2. `ABAD` becomes `0 10 0 111`, and reading `0100111` from the left can only mean A, B, A, D.

Deflate has two kinds of code tables: a **fixed** table defined once in the standard (good for
small data), and **dynamic** tables, built for each block from its own symbol counts and stored at
the block's start (better for large data).

## Deflate, bit by bit

Deflate (RFC 1951) writes blocks. Each starts with a three-bit header:

| Bits | Meaning |
|---|---|
| 1 bit: BFINAL | 1 = this is the last block |
| 2 bits: BTYPE | 0 = stored (bytes as they are), 1 = fixed Huffman codes, 2 = dynamic Huffman codes |

In the fixed table, the symbols are:

| Symbols | Meaning | Code length | Codes |
|---|---|---|---|
| 0-143 | literal byte 0-143 | 8 bits | `00110000` + symbol |
| 144-255 | literal byte 144-255 | 9 bits | `110010000` + (symbol - 144) |
| 256 | end of block | 7 bits | `0000000` |
| 257-279 | a length (plus extra bits) | 7 bits | `0000001` ... |
| 280-287 | a length (plus extra bits) | 8 bits | `11000000` ... |

After a length comes a distance code of 5 bits, and some codes add a few *extra bits* to pick an
exact value within a range.

Here is `abcabcabcabc` compressed by deflate - seven bytes instead of twelve:

```text
4b 4c 4a 4e 84 21 00
```

Deflate fills each byte starting with its lowest bit (bit 0, chapter 2). Written out that way, the
bit stream reads:

```text
byte 4b = 01001011  -> bits in order: 1 1 0 1 0 0 1 0
byte 4c = 01001100  -> bits in order: 0 0 1 1 0 0 1 0
...
```

and decodes as:

| Bits (in order) | Meaning |
|---|---|
| `1` | BFINAL = 1: the last block |
| `1 0` | BTYPE = 1 (the two bits read as a number, low bit first): fixed codes |
| `10010001` | literal `a` (0x61 = 97; `00110000` + 97 = `10010001`) |
| `10010010` | literal `b` |
| `10010011` | literal `c` |
| `10010001` | literal `a` |
| `0000110` | symbol 262: length 8 |
| `00010` | distance code 2: distance 3 |
| `0000000` | symbol 256: end of block |

That is 3 + 4 x 8 + 7 + 5 + 7 = 54 bits; the last two bits of the seventh byte are unused. (Huffman
codes are the one thing deflate writes from the code's *highest* bit first; numbers such as BTYPE
and the extra bits go lowest bit first. Part IV spells out every table.)

## MSZIP: deflate in a cabinet

A cabinet (`.cab`) splits the files' data into blocks of at most 32 KiB. With **MSZIP** each block
is the two letters `CK` (`43 4b`) followed by one deflate stream, and each block may refer back
into the previous one. `compress = "mszip:6"` in a source is deflate at effort 6 of 9: a higher
number searches longer for matches - smaller output, more time - and the result unpacks the same
way.

## Deflate in an MSIX

An MSIX is a ZIP archive, and ZIP stores each file either as it is ("stored", method 0) or
compressed with deflate (method 8). The tutorial's `hello.msix` (chapter 17):

| File | Size | In the package | |
|---|---|---|---|
| `hello.exe` | 17,920 | 6,706 | deflate: 37% |
| `Registry.dat` | 8,192 | 430 | deflate: mostly zero bytes, 5% |
| `AppxManifest.xml` | 2,960 | 1,010 | deflate: text, 34% |
| `Assets\Square150x150.png` | 301 | 301 | stored: PNG is compressed already |
| `guide.txt` | 6 | 14 | deflate: too small to gain |

The last line shows the cost of compressing tiny data: a deflate block has a few bits of header and
an end code, and six bytes contain nothing to repeat.

## Where this is used

- Tutorial chapter [15](../tutorial/15-cabinets-and-architectures.md): `compress`, `cab`,
  `cab-max-size`.
- Tutorial chapter [17](../tutorial/17-msix-and-bundles.md): `--msix-compress`.
- Part IV: [Cabinets, MSZIP and deflate](../formats/cab-mszip.md), [MSIX](../formats/msix.md).
