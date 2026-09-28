#!/usr/bin/env python3
"""build.py - the rubrapack manual as a book: a PDF (through Typst) and a web edition per language,
made from the Markdown in manual/ (English, the original) and manual-ko/ (Korean). Part I is the
user manual (rpk.md), Part II the file format manual (formats/).

  python3 manual/book/build.py [--out build/book] [--typst typst] [--font-path DIR] [--no-pdf]

The output directory is the whole site (index.html, en/, ko/, the two PDFs); it is published as-is.

Needs Python 3 and, for the PDF, Typst 0.13 or later with the fonts Noto Serif, Noto Sans, Noto
Serif CJK KR, Noto Sans CJK KR and D2Coding (--font-path, or TYPST_FONT_PATHS). A PDF whose fonts
were not found is thrown away rather than published with substitutes.
"""
import argparse
import html
import os
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

LANGS = {
    "en": {"dir": "manual", "title": "rubrapack Manual", "sub": "Building and signing MSI and MSIX packages - and how the formats work",
           "parts": ["User manual", "File format manual"], "toc": "Contents", "part": "Part", "prev": "Previous", "next": "Next",
           "pdf": "PDF edition", "other": "한국어", "lang_name": "English", "home": "Contents",
           "note": "The English edition is the original.", "version": "Version"},
    "ko": {"dir": "manual-ko", "title": "rubrapack 매뉴얼", "sub": "MSI·MSIX 패키지를 만들고 서명하기 - 그리고 그 형식이 어떻게 짜였는가",
           "parts": ["사용자 매뉴얼", "파일 형식 매뉴얼"], "toc": "차례", "part": "제", "prev": "이전", "next": "다음",
           "pdf": "PDF 판", "other": "English", "lang_name": "한국어", "home": "차례",
           "note": "영어판이 원본이고 이 판은 그 번역이다. 둘이 어긋나면 영어판을 따른다.", "version": "판"},
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


INLINE = re.compile(r"(`+)(.+?)\1|\*\*(.+?)\*\*|\[([^\]]+)\]\(([^)\s]+)\)|<(https?://[^>\s]+)>|(?<![\w*])\*(?!\s)([^*]+?)\*(?![\w*])")


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

def chapters(lang):
    base = os.path.join(REPO, LANGS[lang]["dir"])
    out = [(0, "rpk", os.path.join(base, "rpk.md"))]
    for name in FORMAT_ORDER:
        out.append((1, "formats-" + name.lower(), os.path.join(base, "formats", name + ".md")))
    book = []
    for part, slug, path in out:
        lines = open(path, encoding="utf-8").read().splitlines()
        # The pages' own "English: ..." / "Korean: ..." cross-links do not belong in a book.
        lines = [l for l in lines if not re.match(r"^(English|Korean):\s*\[", l)]
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


def ty_inline(text, chapter, book):
    out = []
    for t in inline_tokens(text):
        if t[0] == "text":
            out.append(ty_text(t[1]))
        elif t[0] == "code":
            out.append("#raw(" + ty_str(t[1]) + ");")
        elif t[0] == "strong":
            out.append("#strong[" + ty_inline(t[1], chapter, book) + "];")
        elif t[0] == "em":
            out.append("#emph[" + ty_inline(t[1], chapter, book) + "];")
        else:
            r = resolve_link(t[2], chapter, book)
            label = ty_inline(book_label(t[1], r, book), chapter, book)
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
            cols = "(auto, 1fr)" if n == 2 else "(" + ", ".join(["auto"] * (n - 1) + ["1fr"]) + ")"
            cells = ["table.header(" + ", ".join("[*" + ty_inline(c, chapter, book) + "*]" for c in b[1]) + ")"]
            for row in b[2]:
                row = (row + [""] * n)[:n]
                cells += ["[" + ty_inline(c, chapter, book) + "]" for c in row]
            out.append("#tbl(columns: " + cols + ", " + ", ".join(cells) + ")\n")
        elif b[0] == "list":
            out.append(ty_list(b, chapter, book, 0))
    return "\n".join(out)


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
#let mono = ("D2Coding",)
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
#let tbl(..args) = block(width: 100%, text(size: 8.5pt, table(stroke: 0.4pt + luma(170), inset: 4pt, align: left, ..args)))
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

#page(numbering: none)[
  #outline(title: TOC-WORD, depth: 2, indent: auto)
]

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
            .replace("TOC-WORD", ty_str(L["toc"])).replace("REPO-URL", ty_str(REPO_URL)).replace("SITE-URL", ty_str(SITE))
            .replace("LANG", ty_str(lang)))
    parts = []
    last = None
    for c in book:
        if c["part"] != last:
            num = ("Part " + ["I", "II"][c["part"]]) if lang == "en" else ("제" + str(c["part"] + 1) + "부")
            parts.append("#part(" + ty_str(L["parts"][c["part"]]) + ", " + ty_str(num) + ")\n")
            last = c["part"]
        parts.append("= " + ty_inline(c["title"], c, book) + " <ch-" + c["slug"] + ">\n")
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


def h_blocks(blocks, chapter, book):
    out = []
    for b in blocks:
        if b[0] == "heading":
            lvl = min(b[1], 4)
            out.append('<h%d id="%s">%s</h%d>' % (lvl, html.escape(slugify(b[2])), h_inline(b[2], chapter, book), lvl))
        elif b[0] == "para":
            out.append("<p>" + h_inline(b[1], chapter, book) + "</p>")
        elif b[0] == "code":
            out.append('<pre><code class="lang-' + (b[1] or "text") + '">' + html.escape(b[2], quote=False) + "</code></pre>")
        elif b[0] == "table":
            rows = ["<tr>" + "".join("<th>" + h_inline(c, chapter, book) + "</th>" for c in b[1]) + "</tr>"]
            for r in b[2]:
                rows.append("<tr>" + "".join("<td>" + h_inline(c, chapter, book) + "</td>" for c in r) + "</tr>")
            out.append('<div class="table"><table>' + "".join(rows) + "</table></div>")
        elif b[0] == "list":
            out.append(h_list(b, chapter, book))
    return "\n".join(out)


def h_list(lst, chapter, book):
    tag = "ol" if lst[1] else "ul"
    items = []
    for body, sub in lst[2]:
        items.append("<li>" + h_inline(body, chapter, book) + "".join(h_list(s, chapter, book) for s in sub) + "</li>")
    return "<%s>%s</%s>" % (tag, "".join(items), tag)


CSS = """:root { color-scheme: light dark; --fg:#1b1b1b; --bg:#fdfdfb; --muted:#666; --rule:#ddd; --code:#f3f3f1; --accent:#1a4f8a; }
@media (prefers-color-scheme: dark) { :root { --fg:#e4e4e4; --bg:#16181a; --muted:#9aa0a6; --rule:#333; --code:#23262a; --accent:#7aa9d6; } }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg); line-height:1.7;
  font-family: "Noto Serif", "Noto Serif CJK KR", "Apple SD Gothic Neo", "Malgun Gothic", Georgia, serif; }
header.bar, footer.bar { font-family: "Noto Sans", "Noto Sans CJK KR", system-ui, sans-serif; font-size:.9rem; }
header.bar { border-bottom:1px solid var(--rule); padding:.6rem 1rem; display:flex; gap:1rem; flex-wrap:wrap; align-items:baseline; }
header.bar .book { font-weight:700; }
header.bar .spacer { flex:1; }
main { max-width:48rem; margin:0 auto; padding:1.5rem 1.1rem 3rem; }
h1, h2, h3, h4 { font-family: "Noto Sans", "Noto Sans CJK KR", system-ui, sans-serif; line-height:1.35; }
h1 { font-size:1.9rem; margin:.5rem 0 1.2rem; }
h2 { font-size:1.35rem; margin-top:2.2rem; border-bottom:1px solid var(--rule); padding-bottom:.2rem; }
h3 { font-size:1.1rem; margin-top:1.8rem; }
a { color:var(--accent); text-decoration:none; } a:hover { text-decoration:underline; }
code { font-family: D2Coding, "Noto Sans Mono", ui-monospace, Consolas, monospace; font-size:.9em; background:var(--code); padding:.05em .3em; border-radius:3px; }
pre { background:var(--code); padding:.8rem 1rem; border-radius:4px; overflow-x:auto; line-height:1.45; }
pre code { background:none; padding:0; font-size:.84rem; }
.table { overflow-x:auto; margin:1rem 0; }
table { border-collapse:collapse; font-size:.88rem; min-width:60%; }
th, td { border:1px solid var(--rule); padding:.35rem .55rem; vertical-align:top; text-align:left; }
th { background:var(--code); }
footer.bar { border-top:1px solid var(--rule); padding:.8rem 1rem; display:flex; justify-content:space-between; gap:1rem; }
.toc ol { padding-left:1.4rem; } .toc li { margin:.2rem 0; }
.part { font-family: "Noto Sans", "Noto Sans CJK KR", sans-serif; color:var(--muted); margin-top:1.6rem; font-weight:700; }
.muted { color:var(--muted); }
"""


def page(lang, title, body, prev=None, nxt=None, other=None):
    L = LANGS[lang]
    nav_other = '<a href="../%s/%s">%s</a>' % ("ko" if lang == "en" else "en", other or "index.html", LANGS["ko" if lang == "en" else "en"]["lang_name"])
    foot = '<footer class="bar"><span>%s</span><span>%s</span></footer>' % (
        ('&larr; <a href="%s">%s</a>' % prev) if prev else "", ('<a href="%s">%s</a> &rarr;' % nxt) if nxt else "")
    return ('<!doctype html>\n<html lang="%s">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n'
            '<title>%s - %s</title>\n<link rel="stylesheet" href="../book.css">\n</head>\n<body>\n'
            '<header class="bar"><a class="book" href="index.html">%s</a><span class="muted">%s</span><span class="spacer"></span>'
            '<a href="../rubrapack-manual-%s.pdf">%s</a>%s</header>\n<main>\n%s\n</main>\n%s\n</body>\n</html>\n') % (
        lang, html.escape(title), html.escape(L["title"]), html.escape(L["title"]), version(), lang, L["pdf"], nav_other, body, foot)


def build_html(lang, book, out):
    L = LANGS[lang]
    d = os.path.join(out, lang)
    os.makedirs(d, exist_ok=True)
    for i, c in enumerate(book):
        prev = (book[i - 1]["slug"] + ".html", html.escape(re.sub(r"`", "", book[i - 1]["title"]))) if i else None
        nxt = (book[i + 1]["slug"] + ".html", html.escape(re.sub(r"`", "", book[i + 1]["title"]))) if i + 1 < len(book) else None
        body = "<h1>" + h_inline(c["title"], c, book) + "</h1>\n" + h_blocks(c["blocks"], c, book)
        open(os.path.join(d, c["slug"] + ".html"), "w", encoding="utf-8").write(
            page(lang, re.sub(r"`", "", c["title"]), body, prev, nxt, c["slug"] + ".html"))
    toc = ['<h1>%s</h1><p class="muted">%s</p><p>%s %s &middot; <a href="../rubrapack-manual-%s.pdf">%s</a></p><p class="muted">%s</p><div class="toc">'
           % (html.escape(L["title"]), html.escape(L["sub"]), L["version"], version(), lang, L["pdf"], html.escape(L["note"]))]
    last = None
    for c in book:
        if c["part"] != last:
            if last is not None:
                toc.append("</ol>")
            toc.append('<div class="part">%s</div><ol>' % html.escape(L["parts"][c["part"]]))
            last = c["part"]
        sections = [b for b in c["blocks"] if b[0] == "heading" and b[1] == 2]
        subs = "".join('<li><a href="%s.html#%s">%s</a></li>' % (c["slug"], html.escape(slugify(s[2])), h_inline(s[2], c, book)) for s in sections)
        toc.append('<li><a href="%s.html">%s</a>%s</li>' % (c["slug"], h_inline(c["title"], c, book), "<ol>" + subs + "</ol>" if subs else ""))
    toc.append("</ol></div>")
    open(os.path.join(d, "index.html"), "w", encoding="utf-8").write(page(lang, L["toc"], "\n".join(toc)))


def build_landing(out):
    v = version()
    body = """<main>
<h1>rubrapack</h1>
<p class="muted">A command-line tool that builds and signs Windows installer packages - MSI and MSIX - on Windows and Linux.<br>
Windows 설치 패키지(MSI·MSIX)를 Windows 와 Linux 에서 만들고 서명하는 명령줄 도구.</p>
<h2>Manual &middot; 매뉴얼 (%s)</h2>
<ul>
<li><strong>English</strong> (original): <a href="en/index.html">web</a> &middot; <a href="rubrapack-manual-en.pdf">PDF</a></li>
<li><strong>한국어</strong>: <a href="ko/index.html">웹</a> &middot; <a href="rubrapack-manual-ko.pdf">PDF</a></li>
</ul>
<p>Part I is the user manual (the <code>.rpk</code> source and the command line); Part II the file format manual - the compound
file, the MSI database, cabinets, Authenticode, MSIX packages, bundles and signatures, registry hives - written so that anyone can
implement these formats without reading rubrapack.<br>
제1부는 사용자 매뉴얼(<code>.rpk</code> 원본과 명령줄), 제2부는 파일 형식 매뉴얼 - 복합 파일, MSI 데이터베이스, 캐비닛,
Authenticode, MSIX 패키지·묶음·서명, 레지스트리 하이브 - 이다. rubrapack 을 읽지 않고도 이 형식들을 구현할 수 있게 썼다.</p>
<h2>Source &middot; 소스</h2>
<p><a href="%s">%s</a> &middot; MIT License</p>
</main>""" % (v, REPO_URL, REPO_URL)
    open(os.path.join(out, "index.html"), "w", encoding="utf-8").write(
        '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<title>rubrapack</title>\n<link rel="stylesheet" href="book.css">\n</head>\n<body>\n' + body + "\n</body>\n</html>\n")
    open(os.path.join(out, "book.css"), "w", encoding="utf-8").write(CSS)
    open(os.path.join(out, ".nojekyll"), "w").write("")


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
    print("build.py: " + os.path.relpath(a.out, REPO) + "/ (web: index.html, en/, ko/)")


if __name__ == "__main__":
    main()
