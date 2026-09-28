# rubrapack manual

Korean: [`../manual-ko/`](../manual-ko/README.md).

- [`rpk.md`](rpk.md) - the `.rpk` source format and the `rubrapack` command line.
- [`formats/`](formats/README.md) - the file format manual: how Windows Installer and MSIX
  packages, their cabinets, registry hives and signatures are built, byte by byte, written so
  that anyone can implement a package writer or reader of their own without reading rubrapack.

Both, in English and Korean, as one book (web and PDF): <https://rubidus-api.github.io/rubrapack/>.
[`book/build.py`](book/build.py) makes it from these files (`python3 manual/book/build.py`; the PDF needs
Typst and the fonts named in the script).

Everything in this directory is under the same MIT license as the code.
