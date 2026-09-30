#!/usr/bin/env python3
"""build.py - the rubrapack manual as a book: a PDF (through Typst) and a web edition per language,
made from the Markdown in manual/ (English, the original) and manual-ko/ (Korean), in the parts
listed in PARTS: the tutorial (tutorial/), the reference (rpk.md), the background knowledge
(basics/) and the file formats (formats/). A part without chapters yet is left out.

  python3 manual/book/build.py [--out build/book] [--typst typst] [--font-path DIR] [--no-pdf]

The output directory is the whole site (index.html, en/, ko/, the two PDFs); it is published as-is.

Needs Python 3 and, for the PDF, Typst 0.13 or later with the fonts Noto Serif, Noto Sans, Noto
Serif CJK KR, Noto Sans CJK KR and D2Coding (--font-path, or TYPST_FONT_PATHS). A PDF whose fonts
were not found is thrown away rather than published with substitutes. With --font-path, the web
edition also gets subsets of those fonts (needs fontTools and brotli); without it, installed fonts.
The web design (manual/book/book.css) follows the Proven C Book's web edition.
"""
import argparse
import html
import json
import os
import shutil
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
VERSION_H = os.path.join(REPO, "include", "rubrapack", "version.h")
SITE = "https://rubidus-api.github.io/rubrapack/"
REPO_URL = "https://github.com/rubidus-api/rubrapack"

FORMAT_ORDER = ["README", "cfb", "msi-database", "msi-summary", "msi-package", "pe", "cab-mszip", "identity",
                "authenticode", "verify", "msix", "registry"]

# The book's parts, in order: English and Korean titles, and where their chapters are - a folder
# of NN-name.md files, one file, or "formats" (FORMAT_ORDER).
PARTS = [
    ("Tutorial", "따라 하며 배우기", "tutorial"),
    ("Reference", "참조", "rpk.md"),
    ("Background knowledge", "기초 지식", "basics"),
    ("File formats", "파일 형식", "formats"),
]
ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII", "VIII"]

LANGS = {
    "en": {"dir": "manual", "title": "rubrapack Manual", "sub": "Building and signing MSI and MSIX packages - and how the formats work",
           "toc": "Contents", "part": "Part", "prev": "Previous", "next": "Next",
           "pdf": "PDF edition", "other": "한국어", "lang_name": "English", "home": "Contents",
           "note": "The English edition is the original.", "version": "Version", "chtoc": "In this chapter"},
    "ko": {"dir": "manual-ko", "title": "rubrapack 매뉴얼", "sub": "MSI·MSIX 패키지를 만들고 서명하기 - 그리고 그 형식이 어떻게 짜였는가",
           "toc": "차례", "part": "제", "prev": "이전", "next": "다음",
           "pdf": "PDF 판", "other": "English", "lang_name": "한국어", "home": "차례",
           "note": "영어판이 원본이고 이 판은 그 번역이다. 둘이 어긋나면 영어판을 따른다.", "version": "판", "chtoc": "이 장의 차례"},
}


def version():
    m = re.search(r'RUBRAPACK_VERSION_STRING\s+"([^"]+)"', open(VERSION_H, encoding="utf-8").read())
    return m.group(1) if m else "0"


# ---- Markdown (the subset these manuals use) -> blocks ---------------------------------------------

def parse_blocks(lines):
    """Headings, paragraphs, fenced code, pipe tables, (nested) lists."""
    blocks, i = [], 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            blocks.append(("code", m.group(1), "\n".join(lines[i + 1:j])))
            i = j + 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            blocks.append(("heading", len(m.group(1)), m.group(2).strip()))
            i += 1
            continue
        if line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|\s*$", lines[i + 1]):
            rows = []
            j = i
            while j < len(lines) and lines[j].startswith("|"):
                if j != i + 1:
                    rows.append(split_row(lines[j]))
                j += 1
            blocks.append(("table", rows[0], rows[1:]))
            i = j
            continue
        if re.match(r"^(\s*)([-*]|\d+\.)\s+", line):
            items, i = parse_list(lines, i, len(re.match(r"^(\s*)", line).group(1)))
            blocks.append(items)
            continue
        j = i
        para = []
        while j < len(lines) and lines[j].strip() and not re.match(r"^(#{1,6}\s|```|\||\s*([-*]|\d+\.)\s+)", lines[j]):
            para.append(lines[j].strip())
            j += 1
        blocks.append(("para", " ".join(para)))
        i = j
    return blocks


def split_row(line):
    cells, cur, code = [], "", False
    for ch in line.strip()[1:-1] if line.strip().endswith("|") else line.strip()[1:]:
        if ch == "`":
            code = not code
        if ch == "|" and not code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += ch
    cells.append(cur.strip())
    return cells


def parse_list(lines, i, indent):
    ordered = bool(re.match(r"^\s*\d+\.", lines[i]))
    items = []
    while i < len(lines):
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
        if not m or len(m.group(1)) != indent:
            break
        body = [m.group(3)]
        i += 1
        sub = []
        while i < len(lines) and lines[i].strip():
            n = re.match(r"^(\s*)([-*]|\d+\.)\s+", lines[i])
            lead = len(re.match(r"^(\s*)", lines[i]).group(1))
            if n and lead > indent:
                sublist, i = parse_list(lines, i, lead)
                sub.append(sublist)
                continue
            if n or lead <= indent:
                break
            body.append(lines[i].strip())
            i += 1
        items.append((" ".join(body), sub))
        if i < len(lines) and not lines[i].strip():
            k = i
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k < len(lines) and re.match(r"^\s{%d}([-*]|\d+\.)\s+" % indent, lines[k]) and not re.match(r"^\s{%d}\s" % indent, lines[k]):
                i = k
                continue
            break
    return ("list", ordered, items), i


# Emphasis may touch letters of scripts that attach particles without a space (Korean), so only
# ASCII word characters next to a `*` keep it literal (a*b*c).
INLINE = re.compile(r"(`+)(.+?)\1|\*\*(.+?)\*\*|\[([^\]]+)\]\(([^)\s]+)\)|<(https?://[^>\s]+)>|(?<![A-Za-z0-9_*])\*(?!\s)([^*]+?)\*(?![A-Za-z0-9_*])")


def inline_tokens(text):
    out, pos = [], 0
    for m in INLINE.finditer(text):
        if m.start() > pos:
            out.append(("text", text[pos:m.start()]))
        if m.group(1):
            out.append(("code", m.group(2)))
        elif m.group(3) is not None:
            out.append(("strong", m.group(3)))
        elif m.group(4) is not None:
            out.append(("link", m.group(4), m.group(5)))
        elif m.group(6):
            out.append(("link", m.group(6), m.group(6)))
        else:
            out.append(("em", m.group(7)))
        pos = m.end()
    if pos < len(text):
        out.append(("text", text[pos:]))
    return out


def slugify(title):
    t = re.sub(r"[`*]", "", title).strip().lower()
    t = "".join(ch for ch in t if ch.isalnum() or ch in " -_")
    return t.replace(" ", "-")


# ---- the book: chapters in order ------------------------------------------------------------------

def part_files(base, where):
    """(slug, path) of one part's chapters, in order."""
    if where == "formats":
        return [("formats-" + n.lower(), os.path.join(base, "formats", n + ".md")) for n in FORMAT_ORDER]
    if where.endswith(".md"):
        return [(where[:-3], os.path.join(base, where))]
    d = os.path.join(base, where)
    if not os.path.isdir(d):
        return []
    return [(where + "-" + f[:-3], os.path.join(d, f)) for f in sorted(os.listdir(d)) if f.endswith(".md") and f != "README.md"]


def chapters(lang):
    base = os.path.join(REPO, LANGS[lang]["dir"])
    out = []
    for index, (_, _, where) in enumerate(PARTS):
        files = part_files(base, where)
        if files:
            out += [(index, slug, path) for slug, path in files]
    book = []
    for part, slug, path in out:
        lines = open(path, encoding="utf-8").read().splitlines()
        # The pages' own "English: ..." / "Korean: ..." cross-links do not belong in a book.
        lines = [l for l in lines if not re.match(r"^(English|Korean):\s*\[", l)]
        # A fence indented into a list item is not supported: it would come out as run-on text.
        for n, l in enumerate(lines, 1):
            if re.match(r"^ +```", l):
                sys.exit("build.py: %s:%d: indented code fence; put the block at the left margin"
                         % (os.path.relpath(path, REPO), n))
        blocks = parse_blocks(lines)
        title = blocks[0][2] if blocks and blocks[0][0] == "heading" and blocks[0][1] == 1 else slug
        body = blocks[1:] if blocks and blocks[0][0] == "heading" and blocks[0][1] == 1 else blocks
        book.append({"part": part, "slug": slug, "path": path, "title": title, "blocks": body})
    return book


def resolve_link(url, chapter, book):
    """(kind, target): ("ext", url), ("chapter", slug, fragment) or ("none", None)."""
    if re.match(r"^[a-z]+:", url):
        return ("ext", url)
    if url.startswith("#"):
        return ("chapter", chapter["slug"], url[1:])
    path, _, frag = url.partition("#")
    target = os.path.normpath(os.path.join(os.path.dirname(chapter["path"]), path))
    for c in book:
        if os.path.normpath(c["path"]) == target:
            return ("chapter", c["slug"], frag)
    if target.endswith(".md") and os.path.basename(target) == "README.md":
        return ("chapter", "formats-readme", frag) if "formats" in target else ("none", None)
    return ("none", None)


def book_label(label, r, book):
    """A link that reads as a file name ("msix.md") reads as the chapter's title in the book."""
    if r[0] == "chapter" and re.fullmatch(r"`?[\w.-]+\.md`?", label.strip()):
        for c in book:
            if c["slug"] == r[1]:
                return c["title"]
    return label


# ---- Typst ------------------------------------------------------------------------------------------

TYPST_SPECIAL = set('\\#$*_@<>[]`~=/-+"\'')


def ty_text(s):
    return "".join("\\" + ch if ch in TYPST_SPECIAL else ch for ch in s)


def ty_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def breakable(code):
    """Inline code in a table cell may break after a separator (a zero-width space there); a long
    path or element name would otherwise run over into the next column."""
    return re.sub(r"([\\/._:,>%)\]-])(?=[^\s\\/._:,>%)\]-])", "\\1\u200b", code)


def ty_inline(text, chapter, book, cell=False):
    out = []
    for t in inline_tokens(text):
        if t[0] == "text":
            out.append(ty_text(t[1]))
        elif t[0] == "code":
            out.append("#raw(" + ty_str(breakable(t[1]) if cell else t[1]) + ");")
        elif t[0] == "strong":
            out.append("#strong[" + ty_inline(t[1], chapter, book, cell) + "];")
        elif t[0] == "em":
            out.append("#emph[" + ty_inline(t[1], chapter, book, cell) + "];")
        else:
            r = resolve_link(t[2], chapter, book)
            label = ty_inline(book_label(t[1], r, book), chapter, book, cell)
            if r[0] == "ext":
                out.append("#link(" + ty_str(r[1]) + ")[" + label + "];")
            elif r[0] == "chapter":
                out.append("#link(<ch-" + r[1] + ">)[" + label + "];")
            else:
                out.append(label)
    return "".join(out)


def ty_blocks(blocks, chapter, book):
    out = []
    for b in blocks:
        if b[0] == "heading":
            out.append("=" * min(b[1], 4) + " " + ty_inline(b[2], chapter, book) + "\n")
        elif b[0] == "para":
            out.append(ty_inline(b[1], chapter, book) + "\n")
        elif b[0] == "code":
            lang = b[1] if b[1] in ("toml", "sh", "xml", "c") else ""
            out.append("#raw(block: true, lang: " + ty_str(lang) + ", " + ty_str(b[2]) + ")\n")
        elif b[0] == "table":
            n = len(b[1])
            cells = ["[" + ty_inline(c, chapter, book, True) + "]" for c in b[1]]
            for row in b[2]:
                row = (row + [""] * n)[:n]
                cells += ["[" + ty_inline(c, chapter, book, True) + "]" for c in row]
            out.append("#tbl(columns: (" + ", ".join(column_widths(b[1], b[2])) + ",), " + ", ".join(cells) + ")\n")
        elif b[0] == "list":
            out.append(ty_list(b, chapter, book, 0))
    return "\n".join(out)


def plain_len(text):
    """Length of a cell as it reads: markup, link targets and backticks do not count."""
    t = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    return len(re.sub(r"[`*]", "", t))


def column_widths(header, rows):
    """Typst column widths for a table that must fit the page. A column whose cells are all short
    takes its content's width; the others share the rest in proportion to how much they hold
    (the mean cell, bounded), so one long column cannot squeeze another to a letter's width."""
    out = []
    for i in range(len(header)):
        cells = [r[i] if i < len(r) else "" for r in rows]
        lens = [plain_len(c) for c in cells] or [0]
        widest = max(lens + [plain_len(header[i])])
        if widest <= 14:
            out.append("auto")
        else:
            mean = sum(lens) / len(lens)
            out.append("%dfr" % max(8, min(60, round(mean))))
    return out


def ty_list(lst, chapter, book, depth):
    marker = "+ " if lst[1] else "- "
    out = []
    for body, sub in lst[2]:
        out.append("  " * depth + marker + ty_inline(body, chapter, book))
        for s in sub:
            out.append(ty_list(s, chapter, book, depth + 1).rstrip("\n"))
    return "\n".join(out) + "\n"


TYPST_PREAMBLE = r"""
#let serif = ("Noto Serif", "Noto Serif CJK KR")
#let sans = ("Noto Sans", "Noto Sans CJK KR")
#let mono = if LANG == "ko" { ("D2Coding",) } else { ("Noto Sans Mono",) }
#set document(title: BOOK-TITLE + " " + BOOK-VERSION, author: "rubidus-api")
#set text(font: if LANG == "ko" { ("Noto Serif CJK KR",) + serif } else { serif }, size: 10pt, lang: LANG)
#set par(justify: true, leading: 0.72em, spacing: 1.05em)
#set page(paper: "a4", margin: (x: 22mm, top: 24mm, bottom: 22mm))
#show heading: set text(font: if LANG == "ko" { ("Noto Sans CJK KR",) + sans } else { sans })
#show heading: set block(above: 1.4em, below: 0.8em)
#show heading.where(level: 2): set text(size: 13pt)
#show heading.where(level: 3): set text(size: 11pt)
#show heading.where(level: 4): set text(size: 10pt)
#show raw: set text(font: mono)
#show raw.where(block: false): set text(size: 1.12em)
#show raw.where(block: true): it => block(fill: luma(245), inset: 7pt, radius: 3pt, width: 100%, text(size: 8pt, it))
#show link: it => if type(it.dest) == str { text(fill: rgb("#1A4F8A"), it) } else { it }
// Tables as in the Proven C Book: a darker outer line, light inner lines, a shaded header row in
// bold sans, and with three columns or more the first column shaded as the key.
#let tbl-outer = 0.6pt + rgb("#8a8a8a")
#let tbl-inner = 0.4pt + rgb("#c4c4c4")
#let tbl(columns: (), ..cells) = {
  let n = columns.len()
  let items = cells.pos()
  let rows = calc.ceil(items.len() / n)
  let key = n >= 3
  set par(justify: false)
  align(center, text(size: 9pt, table(
    columns: columns, inset: 5pt, align: left,
    fill: (col, row) => if row == 0 { rgb("#ececec") } else if key and col == 0 { rgb("#f5f5f5") } else { none },
    stroke: (x, y) => (
      left: if x == 0 { tbl-outer } else { tbl-inner },
      right: if x == n - 1 { tbl-outer } else { none },
      top: if y == 0 { tbl-outer } else if y == 1 { 0.5pt + rgb("#8a8a8a") } else { tbl-inner },
      bottom: if y == rows - 1 { tbl-outer } else { none },
    ),
    table.header(..items.slice(0, n).map(c => text(font: sans, weight: "bold", c))),
    ..items.slice(n).enumerate().map(((i, c)) =>
      if key and calc.rem(i, n) == 0 { text(font: sans, weight: "bold", size: 0.98em, c) } else { c }),
  )))
}
#set list(indent: 0.6em)
#set enum(indent: 0.6em)

// Title page.
#page(numbering: none)[
  #v(5cm)
  #text(font: sans, size: 30pt, weight: "bold", BOOK-TITLE)
  #v(0.6cm)
  #text(font: sans, size: 13pt, BOOK-SUB)
  #v(2.5cm)
  #text(size: 11pt)[#VERSION-WORD #BOOK-VERSION]
  #v(0.3cm)
  #text(size: 10pt, fill: luma(90))[#link(REPO-URL) \ #link(SITE-URL)]
  #v(1fr)
  #text(size: 9pt, fill: luma(90))[#BOOK-NOTE \ MIT License.]
]

// The contents: parts, chapters and their sections, each row joined to its page number by dots.
#let toc-row(label, pageno, dest) = block(width: 100%, inset: (left: 1.2em), above: 1.10em, below: 0.58em)[
  #link(dest)[#label]
  #box(width: 1fr, inset: (x: 0.5em), text(fill: rgb("#888888"), tracking: 0.35em, repeat[.]))
  #link(dest)[#pageno]
]
#let toc-subrow(no, title, pageno, dest) = block(width: 100%, inset: (left: 3.0em), above: 0.6em, below: 0.6em)[
  #link(dest)[#no #title]
  #box(width: 1fr, inset: (x: 0.4em), text(fill: rgb("#bbbbbb"), tracking: 0.35em, repeat[.]))
  #link(dest)[#pageno]
]
#let toc-part(title) = block(above: 1.5em, below: 0.7em, sticky: true)[
  #text(font: sans, size: 10.5pt, weight: "bold", title)
]
#page(numbering: none)[
  #set par(leading: 1.0em, justify: false)
  #text(font: sans, size: 15pt, weight: "bold", TOC-WORD)
  #v(0.9em)
  #context {
    let page-of = h => counter(page).at(h.location()).first()
    for x in query(selector(<part-mark>).or(heading.where(level: 1)).or(heading.where(level: 2))) {
      if x.func() == metadata {
        toc-part(x.value)
      } else if x.level == 1 {
        let no = counter(heading).at(x.location()).first()
        toc-row([#no. #x.body], page-of(x), x.location())
      } else {
        toc-subrow(numbering("1.1", ..counter(heading).at(x.location())), x.body, page-of(x), x.location())
      }
    }
  }
]

// "In this chapter": the chapter's sections with their pages, under the chapter title, in the
// book's box form - a thick left rail on the label, a thin one on the list.
#let rail-fill = rgb("#d5d5d5")
#let chapter-toc() = context {
  let nexts = query(heading.where(level: 1).after(here()))
  let sel = heading.where(level: 2).or(heading.where(level: 3)).after(here())
  let sel = if nexts.len() > 0 { sel.before(nexts.first().location()) } else { sel }
  let hs = query(sel)
  if hs.len() < 2 { return }
  block(width: 100%, above: 1.1em, below: 1.2em, breakable: true)[
    #set par(first-line-indent: 0em, justify: false)
    #block(width: 100%, above: 0pt, below: 3pt, sticky: true,
           inset: (left: 9pt, top: 3.5pt, bottom: 3.5pt), stroke: (left: 3.2pt + rail-fill))[
      #text(font: sans, weight: "bold", size: 1.05em, CHAPTER-CONTENTS-LABEL)
    ]
    #block(width: 100%, above: 0pt, below: 0pt, inset: (left: 9pt, top: 7pt, bottom: 7pt),
           stroke: (left: 1.2pt + rail-fill))[
      #for h in hs {
        let no = if h.level == 2 { numbering("1.1", ..counter(heading).at(h.location())) } else { "" }
        block(width: 100%, above: 0.6em, below: 0pt, inset: (left: if h.level == 2 { 0pt } else { 12pt }))[
          #link(h.location())[#box(width: 34pt)[#no]#h.body]
          #box(width: 1fr, repeat(text(fill: rgb("#bbbbbb"))[.]))
          #link(h.location())[#counter(page).at(h.location()).first()]
        ]
      }
    ]
  ]
}

#set page(numbering: "1", header: context {
  let n = counter(page).get().first()
  let hs = query(heading.where(level: 1).before(here()))
  let t = if hs.len() > 0 { hs.last().body } else { [] }
  set text(font: sans, size: 8pt, fill: luma(110))
  if calc.odd(n) { align(right)[#t] } else { align(left)[#BOOK-TITLE #BOOK-VERSION] }
})
#counter(page).update(1)
#set heading(numbering: (..n) => if n.pos().len() <= 2 { numbering("1.1", ..n) })
#show heading.where(level: 1): it => {
  pagebreak(weak: true)
  v(1.5cm)
  block(text(size: 20pt, weight: "bold", it))
  v(0.6cm)
}
#let part(name, number) = {
  pagebreak(weak: true)
  page(numbering: none, header: none)[
    #metadata(number + " - " + name) <part-mark>
    #v(7cm)
    #align(center, text(font: sans, size: 12pt, fill: luma(90), number))
    #v(0.4cm)
    #align(center, text(font: sans, size: 26pt, weight: "bold", name))
  ]
}
"""


def typst_source(lang, book):
    L = LANGS[lang]
    head = (TYPST_PREAMBLE.replace("BOOK-TITLE", ty_str(L["title"])).replace("BOOK-VERSION", ty_str(version()))
            .replace("BOOK-SUB", ty_str(L["sub"])).replace("BOOK-NOTE", ty_str(L["note"])).replace("VERSION-WORD", ty_str(L["version"]))
            .replace("TOC-WORD", ty_str(L["toc"])).replace("CHAPTER-CONTENTS-LABEL", ty_str(L["chtoc"])).replace("REPO-URL", ty_str(REPO_URL)).replace("SITE-URL", ty_str(SITE))
            .replace("LANG", ty_str(lang)))
    parts = []
    last = None
    for c in book:
        if c["part"] != last:
            num = part_number(lang, book, c["part"])
            parts.append("#part(" + ty_str(part_title(lang, c["part"])) + ", " + ty_str(num) + ")\n")
            last = c["part"]
        parts.append("= " + ty_inline(c["title"], c, book) + " <ch-" + c["slug"] + ">\n\n#chapter-toc()\n")
        parts.append(ty_blocks(c["blocks"], c, book))
    return head + "\n" + "\n".join(parts)


def build_pdf(lang, book, out_pdf, typst, font_path):
    src = os.path.join(os.path.dirname(out_pdf), "book-" + lang + ".typ")
    open(src, "w", encoding="utf-8").write(typst_source(lang, book))
    cmd = [typst, "compile", "--root", os.path.dirname(src)]
    if font_path:
        cmd += ["--font-path", font_path]
    cmd += [src, out_pdf]
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = r.stdout + r.stderr
    os.remove(src)
    if r.returncode != 0:
        sys.exit("build.py: typst failed for " + lang + ":\n" + log)
    if "unknown font family" in log:
        os.remove(out_pdf)
        sys.exit("build.py: fonts not found (" + lang + "); the PDF with substitute fonts was removed:\n" + log)
    print("build.py: " + os.path.relpath(out_pdf, REPO))


# ---- HTML ---------------------------------------------------------------------------------------------

def h_inline(text, chapter, book):
    out = []
    for t in inline_tokens(text):
        if t[0] == "text":
            out.append(html.escape(t[1], quote=False))
        elif t[0] == "code":
            out.append("<code>" + html.escape(t[1], quote=False) + "</code>")
        elif t[0] == "strong":
            out.append("<strong>" + h_inline(t[1], chapter, book) + "</strong>")
        elif t[0] == "em":
            out.append("<em>" + h_inline(t[1], chapter, book) + "</em>")
        else:
            r = resolve_link(t[2], chapter, book)
            label = h_inline(book_label(t[1], r, book), chapter, book)
            if r[0] == "ext":
                out.append('<a href="' + html.escape(r[1]) + '">' + label + "</a>")
            elif r[0] == "chapter":
                href = (r[1] + ".html" if r[1] != chapter["slug"] else "") + ("#" + r[2] if r[2] else "")
                out.append('<a href="' + html.escape(href or "#") + '">' + label + "</a>")
            else:
                out.append(label)
    return "".join(out)


# The web edition follows the Proven C Book's (same author): a sticky bar, a menu panel with the
# reading settings, the contents by part on the cover, the detailed contents with a heading search.

UI = {
    "en": dict(short="rubrapack Manual", other="Ko", toc="Contents", prev="Prev", next="Next", top="Contents",
               toc_full="Detailed contents", toc_full_desc="The contents opened out to the section level - for going straight to a place.",
               toc_here="In this chapter", skip="Skip to content", permalink="Link to this section",
               search="Search", search_ph="Find by heading", search_none="Nothing found",
               search_hint="Searches chapter and section headings - not the full text",
               menu="Menu", settings="Settings", set_width="Limit line width", set_theme="Theme",
               th_auto="System", th_light="Light", th_dark="Dark", set_colors="Colours", c_fg="Text",
               c_bg="Background", c_link="Link", set_note="Settings are kept in this browser only.", pdf="PDF",
               l_home="All projects", l_repos="All repos", t_repo="rubrapack on GitHub",
               t_home="rubidus-api.github.io - every published project", t_repos="github.com/rubidus-api - every repository",
               version="Version", licence="MIT License", chapter=lambda n: "ch. %d" % n,
               blurb="A user manual for rubrapack, and a file format manual that explains Windows Installer and MSIX packages byte by byte, so that anyone can implement them."),
    "ko": dict(short="rubrapack 매뉴얼", other="En", toc="목차", prev="이전", next="다음", top="목차로",
               toc_full="상세 차례", toc_full_desc="장 아래 절까지 펼친 차례다. 찾는 자리를 바로 짚을 때 쓴다.",
               toc_here="이 장의 차례", skip="본문으로 건너뛰기", permalink="이 절의 주소",
               search="검색", search_ph="제목으로 찾기", search_none="찾은 것이 없다",
               search_hint="장과 절의 제목에서 찾는다 - 본문 전체가 아니다",
               menu="메뉴", settings="설정", set_width="가로폭 제한", set_theme="테마",
               th_auto="시스템", th_light="밝게", th_dark="어둡게", set_colors="색", c_fg="글자",
               c_bg="배경", c_link="링크", set_note="설정은 이 브라우저에만 저장된다.", pdf="PDF",
               l_home="전체 사이트", l_repos="저장소 목록", t_repo="rubrapack 의 GitHub 저장소",
               t_home="rubidus-api.github.io - 공개된 모든 프로젝트", t_repos="github.com/rubidus-api - 모든 저장소",
               version="판", licence="MIT 라이선스", chapter=lambda n: "%d장" % n,
               blurb="rubrapack 의 사용자 매뉴얼과, Windows Installer 와 MSIX 패키지를 바이트 단위로 풀어 누구나 구현할 수 있게 쓴 파일 형식 매뉴얼을 함께 싣는다."),
}

APPLY_JS = (
    "(function(){try{var s=JSON.parse(localStorage.getItem('rp-read')||'{}');"
    "var r=document.documentElement;"
    "if(s.theme&&s.theme!=='auto')r.setAttribute('data-theme',s.theme);"
    "if(s.fg)r.style.setProperty('--fg',s.fg);"
    "if(s.bg)r.style.setProperty('--bg',s.bg);"
    "if(s.link)r.style.setProperty('--link',s.link);"
    "if(s.width)r.setAttribute('data-measure','on');"
    "}catch(e){}})();"
)

PANEL_JS = (
    "(function(){"
    "var b=document.querySelector('.here-btn'),p=document.getElementById('here-panel');"
    "function open(o){if(!p)return;p.hidden=!o;if(b)b.setAttribute('aria-expanded',o);}"
    "if(b)b.addEventListener('click',function(){open(p.hidden);});"
    "if(p)p.addEventListener('click',function(e){"
    "if(e.target.tagName==='A'&&e.target.getAttribute('href').charAt(0)==='#')open(false);});"
    "document.addEventListener('keydown',function(e){if(e.key==='Escape')open(false);});"
    "var box=document.getElementById('setbox'),so=document.getElementById('set-open');"
    "if(!box||!so)return;"
    "var r=document.documentElement;"
    "function load(){try{return JSON.parse(localStorage.getItem('rp-read')||'{}');}"
    "catch(e){return {};}}"
    "function save(s){try{localStorage.setItem('rp-read',JSON.stringify(s));}catch(e){}}"
    "function css(n){return getComputedStyle(r).getPropertyValue(n).trim();}"
    "function hex(v){var m=v.match(/^rgba?\\((\\d+)[ ,]+(\\d+)[ ,]+(\\d+)/);"
    "if(!m)return v.charAt(0)==='#'?v:'#000000';"
    "return '#'+[1,2,3].map(function(i){"
    "return ('0'+parseInt(m[i],10).toString(16)).slice(-2);}).join('');}"
    "function paint(){var s=load();"
    "document.getElementById('s-width').checked=!!s.width;"
    "var t=s.theme||'auto';"
    "[].forEach.call(box.querySelectorAll('.segb'),function(x){"
    "x.setAttribute('aria-pressed',x.dataset.theme===t);});"
    "document.getElementById('s-fg').value=s.fg||hex(css('--fg'));"
    "document.getElementById('s-bg').value=s.bg||hex(css('--bg'));"
    "document.getElementById('s-link').value=s.link||hex(css('--link'));}"
    "so.addEventListener('click',function(){box.hidden=!box.hidden;"
    "so.setAttribute('aria-expanded',!box.hidden);if(!box.hidden)paint();});"
    "document.getElementById('s-width').addEventListener('change',function(){"
    "var s=load();s.width=this.checked;save(s);"
    "if(s.width)r.setAttribute('data-measure','on');else r.removeAttribute('data-measure');});"
    "[].forEach.call(box.querySelectorAll('.segb'),function(x){"
    "x.addEventListener('click',function(){var s=load();s.theme=x.dataset.theme;"
    # Choosing a theme also lets go of chosen colours; otherwise they would hide the theme.
    "delete s.fg;delete s.bg;delete s.link;save(s);"
    "['--fg','--bg','--link'].forEach(function(n){r.style.removeProperty(n);});"
    "if(s.theme==='auto')r.removeAttribute('data-theme');"
    "else r.setAttribute('data-theme',s.theme);"
    "setTimeout(paint,0);});});"
    "[['s-fg','--fg','fg'],['s-bg','--bg','bg'],['s-link','--link','link']]"
    ".forEach(function(a){document.getElementById(a[0]).addEventListener('input',"
    "function(){var s=load();s[a[2]]=this.value;save(s);"
    "r.style.setProperty(a[1],this.value);});});"
    "})();"
)

# The search index is fetched only when it is used (on the first key typed).
SEARCH_JS = """
(function(){
  var q=document.getElementById('q'), out=document.getElementById('qr');
  if(!q||!out) return;
  var data=null, NONE=__NONE__;
  function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
  function run(){
    var v=q.value.trim().toLowerCase();
    out.innerHTML='';
    if(!v||!data) return;
    var hits=[];
    for(var i=0;i<data.length && hits.length<40;i++){
      if(data[i][0].toLowerCase().indexOf(v)>=0) hits.push(data[i]);
    }
    if(!hits.length){ out.innerHTML='<li>'+NONE+'</li>'; return; }
    out.innerHTML=hits.map(function(h){
      return '<li><a href="'+h[1]+'">'+esc(h[0])+'</a>'+
             (h[2]?'<span class="where">'+esc(h[2])+'</span>':'')+'</li>';
    }).join('');
  }
  q.addEventListener('input',function(){
    if(data){ run(); return; }
    fetch('search-index.json').then(function(r){return r.json();})
      .then(function(j){ data=j; run(); })
      .catch(function(){ out.innerHTML='<li>'+NONE+'</li>'; });
  });
})();
"""


def plain(html_text):
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", html_text))).strip()


def h_blocks(blocks, chapter, book, number):
    """The chapter body. `##` sections are numbered N.M (h3), `###` ones are not (h4), as in the PDF.
    Returns the HTML and the section list (level, number, title, id)."""
    out, secs, used, n2 = [], [], set(), 0
    for b in blocks:
        if b[0] == "heading":
            lvl = 3 if b[1] <= 2 else 4
            sid = slugify(b[2]) or "s"
            base, k = sid, 2
            while sid in used:
                sid, k = "%s-%d" % (base, k), k + 1
            used.add(sid)
            no = ""
            if lvl == 3:
                n2 += 1
                no = "%d.%d" % (number, n2)
            body = h_inline(b[2], chapter, book)
            secs.append((lvl, no, plain(body), sid))
            link = '<a class="plink" href="#%s" aria-label="%s" title="%s">#</a>' % (sid, UI[chapter["lang"]]["permalink"], UI[chapter["lang"]]["permalink"])
            out.append('<h%d id="%s">%s%s%s</h%d>' % (lvl, sid, (no + " ") if no else "", body, link, lvl))
        elif b[0] == "para":
            out.append("<p>" + h_inline(b[1], chapter, book) + "</p>")
        elif b[0] == "code":
            out.append('<pre><code class="lang-' + (b[1] or "text") + '">' + html.escape(b[2], quote=False) + "</code></pre>")
        elif b[0] == "table":
            key = ' class="key"' if len(b[1]) >= 3 else ""
            rows = ["<tr>" + "".join("<th>" + h_inline(c, chapter, book) + "</th>" for c in b[1]) + "</tr>"]
            for r in b[2]:
                r = (r + [""] * len(b[1]))[:len(b[1])]
                rows.append("<tr>" + "".join("<td>" + h_inline(c, chapter, book) + "</td>" for c in r) + "</tr>")
            out.append('<div class="tblwrap"><table%s>%s</table></div>' % (key, "".join(rows)))
        elif b[0] == "list":
            out.append(h_list(b, chapter, book))
    return "\n".join(out), secs


def h_list(lst, chapter, book):
    tag = "ol" if lst[1] else "ul"
    items = []
    for body, sub in lst[2]:
        items.append("<li>" + h_inline(body, chapter, book) + "".join(h_list(s, chapter, book) for s in sub) + "</li>")
    return "<%s>%s</%s>" % (tag, "".join(items), tag)


def sec_rows(secs, href=""):
    return "".join('<a class="lv%d" href="%s#%s"><span class="tf-no">%s</span>%s</a>'
                   % (lv, href, sid, no, html.escape(t)) for lv, no, t, sid in secs)


def part_title(lang, part):
    return PARTS[part][0 if lang == "en" else 1]


def part_number(lang, book, part):
    """Parts are numbered as they appear in the book (a part without chapters is not counted)."""
    order = []
    for c in book:
        if c["part"] not in order:
            order.append(c["part"])
    n = order.index(part)
    return "Part " + ROMAN[n] if lang == "en" else "제%d부" % (n + 1)


def part_label(lang, part, book):
    return part_number(lang, book, part) + " - " + part_title(lang, part)


def page(lang, title, inner, self_name, prev=None, nxt=None, label=None, secs=None):
    U = UI[lang]
    esc = html.escape
    bar_nav = ""
    nav = ""
    if prev is not None or nxt is not None:
        bar_nav = ('<span class="bar-nav">'
                   + ('<a href="%s" title="%s" aria-label="%s">&larr;</a>' % (prev, U["prev"], U["prev"]) if prev else '<span class="off" aria-hidden="true">&larr;</span>')
                   + '<a href="index.html" title="%s" aria-label="%s">&uarr;</a>' % (U["top"], U["top"])
                   + ('<a href="%s" title="%s" aria-label="%s">&rarr;</a>' % (nxt, U["next"], U["next"]) if nxt else '<span class="off" aria-hidden="true">&rarr;</span>')
                   + "</span>")
        nav = ('<div class="nav">%s<span class="sp"></span><a href="index.html">%s</a><span class="sp"></span>%s</div>'
               % ('<a href="%s">&larr; %s</a>' % (prev, U["prev"]) if prev else "<span></span>", U["top"],
                  '<a href="%s">%s &rarr;</a>' % (nxt, U["next"]) if nxt else "<span></span>"))
    other = "ko" if lang == "en" else "en"
    here_btn = ('<button class="here-btn" type="button" aria-expanded="false" aria-controls="here-panel">'
                '%s <span class="caret">&#9662;</span></button>' % esc(label or U["menu"]))
    tools = ('<span class="ver">%s</span>' % version()
             + '<button class="tool" type="button" id="set-open">&#9881; %s</button>' % U["settings"]
             + '<a class="tool" href="toc.html">&#9636; %s</a>' % U["toc_full"]
             + '<a class="tool" href="../%s/%s">%s</a>' % (other, self_name, U["other"])
             + '<a class="tool" href="../rubrapack-manual-%s.pdf">&#10515; %s</a>' % (lang, U["pdf"])
             + '<a class="tool" href="%s" title="%s">Git</a>' % (REPO_URL, esc(U["t_repo"]))
             + '<a class="tool" href="https://rubidus-api.github.io/" title="%s">&#8962; %s</a>' % (esc(U["t_home"]), U["l_home"])
             + '<a class="tool" href="https://github.com/rubidus-api" title="%s">&#8801; %s</a>' % (esc(U["t_repos"]), U["l_repos"]))
    settings = ('<div class="setbox" id="setbox" hidden>'
                '<label class="setrow"><input type="checkbox" id="s-width"><span>%s</span></label>'
                '<div class="setrow"><span class="setlbl">%s</span><span class="seg">'
                '<button type="button" class="segb" data-theme="auto">%s</button>'
                '<button type="button" class="segb" data-theme="light">%s</button>'
                '<button type="button" class="segb" data-theme="dark">%s</button></span></div>'
                '<div class="setrow"><span class="setlbl">%s</span>'
                '<label class="col"><span>%s</span><input type="color" id="s-fg"></label>'
                '<label class="col"><span>%s</span><input type="color" id="s-bg"></label>'
                '<label class="col"><span>%s</span><input type="color" id="s-link"></label></div>'
                '<p class="setnote">%s</p></div>') % (U["set_width"], U["set_theme"], U["th_auto"], U["th_light"], U["th_dark"],
                                                     U["set_colors"], U["c_fg"], U["c_bg"], U["c_link"], U["set_note"])
    rows = ""
    if secs:
        rows = '<div class="here-head"><strong>%s</strong></div><div class="here-body">%s</div>' % (U["toc_here"], sec_rows(secs))
    panel = '<div id="here-panel" class="here-panel" hidden><div class="tools">%s</div>%s%s</div>' % (tools, settings, rows)
    return """<!doctype html>
<html lang="%s">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title><link rel="stylesheet" href="../book.css">
<script>%s</script></head>
<body>
<a class="skip" href="#content">%s</a>
<div class="bar"><strong><a href="index.html">%s</a></strong>%s<span class="sp"></span>%s</div>%s
<main id="content"><div class="wrap">
%s
%s
</div></main>
<script>%s</script><script>%s</script></body></html>
""" % (lang, esc(title), APPLY_JS, U["skip"], U["short"], here_btn, bar_nav, panel, inner, nav, PANEL_JS,
       SEARCH_JS.replace("__NONE__", json.dumps(U["search_none"], ensure_ascii=False)))


def build_html(lang, book, out):
    L, U = LANGS[lang], UI[lang]
    d = os.path.join(out, lang)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        if f.endswith(".html") or f == "search-index.json":
            os.remove(os.path.join(d, f))
    pages = []
    for i, c in enumerate(book):
        c["lang"] = lang
        number = i + 1
        body, secs = h_blocks(c["blocks"], c, book, number)
        title = h_inline(c["title"], c, book)
        pages.append((c, number, title, body, secs))
    index_rows = []
    for i, (c, number, title, body, secs) in enumerate(pages):
        name = c["slug"] + ".html"
        prev = pages[i - 1][0]["slug"] + ".html" if i else "index.html"
        nxt = pages[i + 1][0]["slug"] + ".html" if i + 1 < len(pages) else None
        chtoc = ""
        if len(secs) >= 2:
            chtoc = ('<div class="chapter-toc"><div class="rail-label">%s</div><div class="rail-body">%s</div></div>'
                     % (U["toc_here"], sec_rows(secs)))
        inner = '<h2 id="top">%d %s</h2>\n%s\n%s' % (number, title, chtoc, body)
        open(os.path.join(d, name), "w", encoding="utf-8").write(
            page(lang, "%d %s - %s" % (number, plain(title), L["title"]), inner, name, prev, nxt, U["chapter"](number), secs))
        index_rows.append((c, number, title, secs, name))

    # The cover: title, edition, links, then the contents by part.
    toc, last = [], None
    toc.append('<div class="toc-group toc-front"><a class="toc-detail" href="toc.html">%s &rarr;</a></div>' % U["toc_full"])
    for c, number, title, secs, name in index_rows:
        if c["part"] != last:
            if last is not None:
                toc.append("</div>")
            toc.append('<h4 class="toc-part">%s</h4><div class="toc-group">' % html.escape(part_label(lang, c["part"], book)))
            last = c["part"]
        toc.append('<a href="%s">%d. %s</a>' % (name, number, title))
    toc.append("</div>")
    cover = ('<header class="cover"><h1>%s</h1><p class="cover-sub">%s</p>'
             '<p class="cover-meta">%s %s &middot; %s</p>'
             '<p class="cover-links"><a href="%s">github.com/rubidus-api/rubrapack</a>'
             '<a href="../rubrapack-manual-%s.pdf">PDF</a></p>'
             '<div class="cover-blurb"><p>%s</p></div></header>') % (
        html.escape(L["title"]), html.escape(L["sub"]), U["version"], version(), U["licence"], REPO_URL, lang,
        html.escape(U["blurb"]))
    inner = '%s<div class="note"><p>%s</p></div><h3>%s</h3><div class="toc">%s</div>' % (
        cover, html.escape(L["note"]), U["toc"], "".join(toc))
    first = index_rows[0][4]
    open(os.path.join(d, "index.html"), "w", encoding="utf-8").write(
        page(lang, L["title"], inner, "index.html", None, first, U["menu"]))

    # The detailed contents, with a search over the headings.
    rows, last, index = [], None, []
    for c, number, title, secs, name in index_rows:
        if c["part"] != last:
            rows.append('<h4 class="toc-part">%s</h4>' % html.escape(part_label(lang, c["part"], book)))
            last = c["part"]
        rows.append('<div class="tf-ch"><a href="%s"><span class="tf-no">%d</span>%s</a></div>' % (name, number, title))
        if secs:
            rows.append('<div class="tf-secs">%s</div>' % sec_rows(secs, name))
        where = "%d %s" % (number, plain(title))
        index.append([plain(title), name, ""])
        index += [[t, name + "#" + sid, where] for lv, no, t, sid in secs]
    search = ('<div class="searchbox"><input type="search" id="q" placeholder="%s" aria-label="%s">'
              '<p class="search-hint">%s</p><ul class="search-results" id="qr"></ul></div>') % (
        U["search_ph"], U["search"], U["search_hint"])
    inner = '<h2>%s</h2><p class="note">%s</p>%s<div class="toc-full">%s</div>' % (
        U["toc_full"], U["toc_full_desc"], search, "".join(rows))
    open(os.path.join(d, "toc.html"), "w", encoding="utf-8").write(
        page(lang, U["toc_full"] + " - " + L["title"], inner, "toc.html", "index.html", first, U["toc_full"]))
    json.dump(index, open(os.path.join(d, "search-index.json"), "w", encoding="utf-8"), ensure_ascii=False)


def build_landing(out):
    """The site's front door: both editions, both PDFs."""
    v = version()
    body = """<header class="cover"><h1>rubrapack</h1>
<p class="cover-sub">Building and signing Windows installer packages - MSI and MSIX - on Windows and Linux</p>
<p class="cover-sub">Windows 설치 패키지(MSI·MSIX)를 Windows 와 Linux 에서 만들고 서명하기</p>
<p class="cover-meta">%s &middot; MIT License</p></header>
<div class="toc"><h4 class="toc-part">English (original)</h4><div class="toc-group">
<a href="en/index.html">rubrapack Manual - web</a><a href="rubrapack-manual-en.pdf">rubrapack Manual - PDF</a></div>
<h4 class="toc-part">한국어</h4><div class="toc-group">
<a href="ko/index.html">rubrapack 매뉴얼 - 웹</a><a href="rubrapack-manual-ko.pdf">rubrapack 매뉴얼 - PDF</a></div>
<h4 class="toc-part">Source</h4><div class="toc-group"><a href="%s">github.com/rubidus-api/rubrapack</a>
<a href="https://rubidus-api.github.io/">rubidus-api.github.io</a></div></div>""" % (v, REPO_URL)
    open(os.path.join(out, "index.html"), "w", encoding="utf-8").write(
        '<!doctype html>\n<html lang="en">\n<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<title>rubrapack</title><link rel="stylesheet" href="book.css">\n<script>%s</script></head>\n<body>\n'
        '<div class="bar"><strong><a href="index.html">rubrapack</a></strong></div>\n'
        '<main id="content"><div class="wrap">\n%s\n</div></main>\n</body></html>\n' % (APPLY_JS, body))
    shutil.copyfile(os.path.join(HERE, "book.css"), os.path.join(out, "book.css"))
    open(os.path.join(out, ".nojekyll"), "w").write("")


# ---- web fonts ----------------------------------------------------------------------------------------

WEBFONTS = [  # (output name, source file under the font path)
    ("serif", "noto-latin/NotoSerif-Regular.ttf"), ("serif-bold", "noto-latin/NotoSerif-Bold.ttf"),
    ("serif-italic", "noto-latin/NotoSerif-Italic.ttf"),
    ("serif-kr", "noto-cjk-kr/NotoSerifCJKkr-Regular.otf"), ("serif-kr-bold", "noto-cjk-kr/NotoSerifCJKkr-Bold.otf"),
    ("sans", "noto-latin/NotoSans-Regular.ttf"), ("sans-bold", "noto-latin/NotoSans-Bold.ttf"),
    ("sans-kr", "noto-cjk-kr/NotoSansCJKkr-Regular.otf"), ("sans-kr-bold", "noto-cjk-kr/NotoSansCJKkr-Bold.otf"),
    ("mono", "d2coding/D2Coding-Ver1.3.2-20180524.ttf"), ("mono-bold", "d2coding/D2CodingBold-Ver1.3.2-20180524.ttf"),
    ("mono-latin", "noto-latin/NotoSansMono-Regular.ttf"), ("mono-latin-bold", "noto-latin/NotoSansMono-Bold.ttf"),
]

FONTS_README = """# Web fonts

The `.woff2` files here are the fonts below cut down to the characters this manual uses
(`manual/book/build.py` makes them; do not edit them by hand). Each keeps its own copyright and
licence records.

| Files | Font | Licence |
|---|---|---|
| `serif*`, `sans*` (without `-kr`) | Noto Serif, Noto Sans (Google) | SIL Open Font License 1.1 |
| `serif-kr*`, `sans-kr*` | Noto Serif CJK KR, Noto Sans CJK KR (Adobe, Google) | SIL Open Font License 1.1 |
| `mono`, `mono-bold` | D2Coding (NAVER) | SIL Open Font License 1.1 |
| `mono-latin*` | Noto Sans Mono (Google) | SIL Open Font License 1.1 |

The licence permits subsetting and redistribution: <https://openfontlicense.org/>. The full fonts:
<https://github.com/notofonts>, <https://github.com/notofonts/noto-cjk>,
<https://github.com/naver/d2codingfont>.
"""


def build_webfonts(out, font_path):
    """Subsets of the book's fonts holding only the characters the web pages use. Skipped (with a
    note) when fontTools or a font is missing: the pages then fall back to installed fonts."""
    try:
        from fontTools import subset
    except ImportError:
        print("build.py: fontTools not found; web fonts not made")
        return
    chars = set()
    for lang in ("en", "ko"):
        d = os.path.join(out, lang)
        for f in os.listdir(d):
            if f.endswith(".html"):
                chars.update(plain(open(os.path.join(d, f), encoding="utf-8").read()))
    chars.update(plain(open(os.path.join(out, "index.html"), encoding="utf-8").read()))
    chars.update("0123456789.")
    text = "".join(sorted(c for c in chars if ord(c) >= 0x20))
    fd = os.path.join(out, "fonts")
    os.makedirs(fd, exist_ok=True)
    for name, rel in WEBFONTS:
        src = os.path.join(font_path, rel)
        if not os.path.isfile(src):
            print("build.py: font not found, skipped: " + rel)
            continue
        opts = subset.Options()
        opts.flavor = "woff2"
        opts.layout_features = []
        opts.hinting = False
        opts.desubroutinize = True
        opts.name_IDs = ["*"]
        opts.name_languages = ["*"]
        font = subset.load_font(src, opts)
        sub = subset.Subsetter(opts)
        sub.populate(text=text)
        sub.subset(font)
        subset.save_font(font, os.path.join(fd, name + ".woff2"), opts)
    open(os.path.join(fd, "README.md"), "w", encoding="utf-8").write(FONTS_README)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(REPO, "build", "book"))
    ap.add_argument("--typst", default=os.environ.get("TYPST", "typst"))
    ap.add_argument("--font-path", default=os.environ.get("FONT_PATH", ""))
    ap.add_argument("--no-pdf", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    build_landing(a.out)
    for lang in ("en", "ko"):
        book = chapters(lang)
        build_html(lang, book, a.out)
        if not a.no_pdf:
            build_pdf(lang, book, os.path.join(a.out, "rubrapack-manual-%s.pdf" % lang), a.typst, a.font_path)
    if a.font_path:
        build_webfonts(a.out, a.font_path)
    print("build.py: " + os.path.relpath(a.out, REPO) + "/ (web: index.html, en/, ko/, fonts/)")


if __name__ == "__main__":
    main()
