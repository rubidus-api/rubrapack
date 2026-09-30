# Text: characters, code points and encodings

A file holds bytes, and a byte is a number (chapter 1). Text - a product name, a file name, a
license - is stored by giving every character a number and writing the numbers as bytes. There
are several ways to do both, and a package that uses the wrong one shows garbage. This chapter
explains the ways rubrapack and Windows use.

## Characters and code points

**Unicode** is the world's catalogue of characters: letters of every script, digits, punctuation,
symbols, emoji - about 160,000 of them. Each has a number, its **code point**, written `U+` and at
least four hex digits:

| Character | Code point | Name in the catalogue |
|---|---|---|
| `H` | U+0048 | LATIN CAPITAL LETTER H |
| `é` | U+00E9 | LATIN SMALL LETTER E WITH ACUTE |
| `€` | U+20AC | EURO SIGN |
| `한` | U+D55C | HANGUL SYLLABLE HAN |
| `😀` | U+1F600 | GRINNING FACE |

Code points go up to U+10FFFF. A code point says *which* character; it does not yet say which
bytes to write. That is the job of an **encoding**.

## ASCII

The oldest encoding still everywhere is **ASCII** (1963): 128 characters, code points 0 to 127,
one byte each. It has the English letters, digits, punctuation and some control characters:

| Bytes | Characters |
|---|---|
| `00`-`1f` | control characters: `09` tab, `0a` line feed (new line), `0d` carriage return |
| `20` | space |
| `30`-`39` | `0`-`9` |
| `41`-`5a` | `A`-`Z` |
| `61`-`7a` | `a`-`z` |
| `2e` `2f` `3a` `5c` | `.` `/` `:` `\` |

The tutorial's `dist\docs\guide.txt` holds six bytes:

```text
47 75 69 64 65 0a
 G  u  i  d  e  (new line)
```

The first 128 code points of Unicode are ASCII's, and every encoding below writes them as the same
single byte. That is why English text looks the same in almost any encoding - and why problems
show only with other letters.

Windows ends a line with two bytes, `0d 0a` (carriage return, line feed); Linux and macOS with
`0a` alone. Most programs read both.

## Code pages: one byte, 256 characters

ASCII leaves the values 128 to 255 of a byte unused. For decades each language region filled them
differently, and each filling is a **code page**, numbered by Microsoft:

| Code page | Region | `é` | `€` | `한` |
|---|---|---|---|---|
| 1252 | Western Europe | `e9` | `80` | - |
| 949 | Korean | - | `a2 e6` | `c7 d1` |
| 65001 | all: this is UTF-8 | `c3 a9` | `e2 82 ac` | `ed 95 9c` |

A code page can hold only its own region's characters (949 uses two bytes for Korean). And bytes
mean different things in different code pages: the byte `e9` is `é` in 1252 but half of a Korean
character in 949. Text written in one and read in another comes out as garbage, known by the
Japanese word *mojibake*.

Every MSI package has one code page for all its text. rubrapack always writes 65001, UTF-8, so
any language fits into one package; tutorial chapter [7](../tutorial/07-several-languages.md)
shows `inspect` reporting it. Part IV shows a trap: the
[summary information](../formats/msi-summary.md) has a code page of its own.

## UTF-8

**UTF-8** writes every Unicode code point in one to four bytes. ASCII stays one byte, so an
ASCII file is already UTF-8. Larger code points use more bytes, following a fixed bit pattern:

| Code points | Bytes | Bit pattern (`x` = bits of the code point) |
|---|---|---|
| U+0000 - U+007F | 1 | `0xxxxxxx` |
| U+0080 - U+07FF | 2 | `110xxxxx 10xxxxxx` |
| U+0800 - U+FFFF | 3 | `1110xxxx 10xxxxxx 10xxxxxx` |
| U+10000 - U+10FFFF | 4 | `11110xxx 10xxxxxx 10xxxxxx 10xxxxxx` |

Worked out for `한`, U+D55C:

```text
  D55C in binary:       1101 0101 0101 1100      (16 bits)
  split 4 + 6 + 6:      1101   010101   011100
  into the pattern:  1110 1101  10 010101  10 011100
  in hex:                ED         95         9C
```

So `한` is `ed 95 9c` in UTF-8. The pattern makes UTF-8 easy to check: a byte starting `10` is
never the first byte of a character, so a reader can always find where characters begin, and
bytes of another encoding almost never form valid UTF-8 by accident. rubrapack stops at the first
byte of a source that breaks the pattern (`RP1001: not valid UTF-8`) rather than guessing.

## UTF-16

**UTF-16** writes code points up to U+FFFF as one 2-byte unit, and the rest as two units (a
*surrogate pair*). Windows uses it inside: file names, the registry, and every text in a Compound
File's directory are UTF-16, little-endian (chapter 2):

```text
  H     e     l     l     o
  48 00 65 00 6c 00 6c 00 6f 00        "Hello" in UTF-16LE: ASCII letters get a 00 after them

  한
  5c d5                                U+D55C: the two bytes of D55C, little end first

  😀
  3d d8 00 de                          U+1F600: too big for one unit, so the pair D83D DE00
```

A text in a dump that looks like letters with dots between them - `H.e.l.l.o.` - is UTF-16.

## The byte order mark

A file may start with the code point U+FEFF, the **byte order mark** (BOM), to say which encoding
it is: `ef bb bf` for UTF-8, `ff fe` for UTF-16LE. Windows Notepad used to add one to UTF-8 files.
`rubrapack build` and `lint` read a source in UTF-8, with or without a BOM, and in UTF-16 with its
BOM; `edit` changes UTF-8 sources only.

## Unicode normalization

Some characters can be written in Unicode in more than one way. `é` is one code point, U+00E9 -
or two: the letter `e`, U+0065, followed by U+0301, a *combining* acute accent that sits on the
letter before it. Both look exactly the same on screen:

| Form | Code points | UTF-8 bytes |
|---|---|---|
| composed (NFC) | U+00E9 | `c3 a9` |
| decomposed (NFD) | U+0065 U+0301 | `65 cc 81` |

Korean is the largest case. Every modern Hangul syllable has a composed code point, and can
also be written as its letters (*jamo*):

| Form | Code points | UTF-8 bytes |
|---|---|---|
| NFC | U+D55C `한` | `ed 95 9c` |
| NFD | U+1112 `ᄒ` U+1161 `ᅡ` U+11AB `ᆫ` | `e1 84 92 e1 85 a1 e1 86 ab` |

(The composed code point is computed from the letters: 0xAC00 + (18 x 21 + 0) x 28 + 4 = 0xD55C,
where 18, 0 and 4 are the numbers of ㅎ, ㅏ and ㄴ in the Unicode tables.)

**Normalization** turns text into one agreed form. NFC ("composed") is the form Windows, the web
and almost every keyboard produce. NFD ("decomposed") is what macOS file systems store file names
in - so a file copied from a Mac may have a name whose bytes differ from the same name typed on
Windows. For Windows the two are *different names*: a package could then install `한.txt` twice,
or a shortcut could miss its target.

That is why rubrapack has `build --nfc` (tutorial chapter 15), which turns file, folder and shortcut
names into NFC, and why `lint` warns (`RP2105`) about text that is not in NFC (tutorial chapter 18).
rubrapack's normalization follows the Unicode 17.0 tables.

## When text looks wrong

| You see | Likely cause |
|---|---|
| `Ã©` instead of `é` | UTF-8 bytes `c3 a9` read as code page 1252 |
| `�븳` or `?` instead of `한` | UTF-8 bytes read as code page 949, or the other way round |
| `H.e.l.l.o.` in a dump | UTF-16 read as bytes |
| two names that look the same but are not | one NFC, one NFD |

## Where this is used

- Tutorial chapter [7](../tutorial/07-several-languages.md): texts stored as UTF-8, code page 65001.
- Tutorial chapters [15](../tutorial/15-cabinets-and-architectures.md) and
  [18](../tutorial/18-checking-and-looking-inside.md): `--nfc` and `RP2105`.
- Part IV: the string pool of an [MSI database](../formats/msi-database.md), the UTF-16 names in a
  [Compound File](../formats/cfb.md), the [summary information](../formats/msi-summary.md)'s code
  page.
