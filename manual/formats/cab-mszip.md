# Cabinets, MSZIP and deflate

## Cabinet layout [spec: MS-CAB]

```text
CFHEADER (36 bytes when no reserve fields):
  "MSCF", u32 0, u32 cabinet size, u32 0, u32 offset of the first CFFILE, u32 0,
  u8 minor version 3, u8 major version 1, u16 folder count, u16 file count, u16 flags (0),
  u16 set id, u16 cabinet index (0)
CFFOLDER (8 bytes each):
  u32 offset of the folder's first CFDATA, u16 number of CFDATA blocks, u16 compression
  (0 = none, 1 = MSZIP)
CFFILE (per file):
  u32 uncompressed size, u32 offset inside the folder's uncompressed data, u16 folder index,
  u16 date, u16 time, u16 attributes, NUL-terminated name
CFDATA (per block):
  u32 checksum, u16 compressed bytes, u16 uncompressed bytes (at most 32768), data
```

- In an MSI, file names in the cabinet are the `File` table keys, which are ASCII.
- DOS date `(year - 1980) << 9 | month << 5 | day`, time `hour << 11 | minute << 5 | second / 2`.
  A fixed date (rubrapack writes 1980-01-01 00:00) keeps output deterministic; attributes `0x20`.
- One folder holds at most 65535 blocks (about 2 GiB); more data needs more folders or cabinets.

### Block checksum [spec]

```text
checksum(bytes, seed):
  xor every complete little-endian u32 of the bytes into seed;
  the 1-3 remaining bytes form one more u32 with the FIRST remaining byte in the HIGHEST
  position used (3 left: b0<<16 | b1<<8 | b2; 2 left: b0<<8 | b1; 1 left: b0), xor it in.
block checksum = checksum(the 4 bytes "compressed size, uncompressed size" as stored,
                          checksum(block data, 0))
```

A checksum of 0 means "not computed"; readers accept it.

## MSZIP [spec: MS-MCI]

Each CFDATA block of a folder with compression type 1 holds:

```text
"CK" (43 4B), then one complete raw deflate stream (RFC 1951) for this block's
uncompressed bytes (at most 32768), ending with a final block.
```

The decompressor keeps the previous blocks of the same folder as its dictionary, so an encoder
*may* refer back into earlier blocks. An encoder that never does (each block compressed on its
own) produces valid MSZIP too. rubrapack gives each block the previous block's 32 KiB of
uncompressed data as a preset dictionary (about 9% smaller on mixed data than blocks on their own),
and since that dictionary is the input, not the output, the blocks are still compressed on several
threads at once, with the same bytes whatever their number. It starts a new folder at a file
boundary before 65535 blocks; the dictionary does not cross folders. Windows extracts all of it:
a package with twelve folders installs, repairs and removes with every file byte-identical.
[observed]

## LZX [spec: MS-PATCH 2, MS-CAB]

A folder with compression type `3 | w << 8` (w = 15 .. 21) is one LZX stream with a window of 2^w
bytes, cut into **frames** of 32768 output bytes (the last shorter), one frame per CFDATA block.
The window, the three repeated offsets and the code lengths carry on from frame to frame; after
each frame the bit stream is padded to a 16-bit boundary.

- Bits are read from 16-bit **little-endian words, most significant bit first**.
- The folder begins with one bit: 1 means *E8 translation* follows, with a 32-bit size. The
  compressor rewrote the 4 bytes after every `E8` byte (an x86 `call`) from relative to absolute
  addresses so that repeated calls match; the decompressor turns them back, in the first 32768
  frames only.
- A block: 3 bits type (1 verbatim, 2 aligned offset, 3 uncompressed), 24 bits of uncompressed
  size. Blocks and matches may cross frames.
- Verbatim and aligned blocks send the **main tree** (256 literals, then 8 symbols per *position
  slot*: 30 slots for w = 15 up to 50 for w = 21) and the **length tree** (249 symbols), each range
  as differences (mod 17) from the previous block's lengths through a 20-symbol **pretree** (17 and
  18: runs of zeros, 19: a short run of one value). An aligned block first sends an 8-symbol tree
  for the low 3 bits of long offsets.
- A main symbol above 255 is a match: low 3 bits = length - 2 (7: add a length-tree symbol, up to
  257), the rest the position slot. Slots 0-2 reuse the last three offsets; slot *s* >= 3 gives
  the offset `base[s] - 2 + extra bits`, and the new offset moves into the repeated ones.
- An uncompressed block pads to 16 bits (a whole word when already aligned), then holds the three
  repeated offsets as 32-bit little-endian values, the bytes, and one padding byte if their count is
  odd.

rubrapack writes one verbatim block per frame (or an uncompressed one when that is smaller),
matches that never cross a frame, the repeated offsets when they are about as long as a new match,
one step of lazy matching, and no E8 translation. It reads every block type and E8 translation.
Windows' `expand.exe` and msiexec unpack its cabinets, and it unpacks cabinets `makecab.exe` made
with E8 translation on. [observed]

## A deflate encoder in brief [spec: RFC 1951]

1. **Match finding (LZ77):** hash the next 3 bytes, walk a chain of earlier positions with the
   same hash within 32 KiB, keep the longest match (3..258 bytes). Longer chain walks give better
   compression; a "lazy" step that also tries the next position and emits a literal when that
   match is longer helps noticeably.
2. **Blocks:** group up to a few ten thousand symbols. For each block compute the size of three
   encodings and write the smallest:
   - fixed Huffman codes (literal/length 0-143: 8 bits, 144-255: 9, 256-279: 7, 280-287: 8;
     distances 5 bits);
   - dynamic Huffman codes: code lengths limited to 15 bits (7 for the code-length alphabet);
     package-merge gives optimal limited lengths; send HLIT, HDIST, HCLEN, the code-length code
     in the order 16 17 18 0 8 7 9 6 10 5 11 4 12 3 13 2 14 1 15, then the run-length coded
     lengths (16 = repeat previous 3-6, 17 = zeros 3-10, 18 = zeros 11-138);
   - stored (byte-aligned LEN, NLEN, raw bytes; at most 65535 per stored block).
3. Huffman codes are written most significant bit first; everything else least significant bit
   first.
4. Always give at least two symbols a code length (pair a lone symbol with a dummy), so every
   code is complete - some decoders reject incomplete codes.

Check an encoder by inflating its output with an independent implementation (for example the
zlib library with raw deflate, window bits -15) and an MSI built with it by installing it and
comparing installed files' hashes ([verify.md](verify.md)).

## Worked example: the tutorial's hello.msi

The cabinet of the tutorial's first package is the stream `cab1.cab`, 6776 bytes. CFHEADER:

| Offset | Bytes | Field | Value |
|---|---|---|---|
| `0x00` | `4d 53 43 46` | signature | `MSCF` |
| `0x08` | `78 1a 00 00` | cabinet size | 6776 |
| `0x10` | `2c 00 00 00` | first CFFILE | 44 |
| `0x18` | `03 01` | version | 1.3 |
| `0x1A` | `01 00` | folders | 1 |
| `0x1C` | `01 00` | files | 1 |
| `0x1E` | `00 00` | flags | 0 |
| `0x20` | `00 00` | set id | 0 |

CFFOLDER at `0x24`: `42 00 00 00 01 00 01 00` - first CFDATA at 66, 1 block, compression 1 (MSZIP).

CFFILE entries - the name is the `File` table key:

| Offset | size | offset in folder | folder | date | attr | Name |
|---|---|---|---|---|---|---|
| `0x2C` | 17920 | 0 | 0 | 1980-01-01 | `0x20` | `Hello` |

The first CFDATA at 66: checksum `80 35 aa c0`, 6702 compressed bytes for 17920 uncompressed. Its
data begins `43 4b ed 5c 0d 74`: `CK`, then deflate. The first deflate byte `ed` = `11101101`; its
lowest bit is BFINAL = 1, the next two BTYPE = 2 (dynamic codes).
