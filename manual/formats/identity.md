# Deterministic identities and reproducible packages

Windows Installer identifies things by GUIDs and keys that must be **stable**: the same component
must keep the same `ComponentId` in every version, or repair and upgrade misbehave. Generating
them from the content, instead of storing them by hand, keeps them stable and makes builds
reproducible. This page describes rubrapack's rules; other rules work too as long as they are
stable.

## Name-based GUIDs (UUID version 8) [spec: RFC 9562]

```text
digest = SHA-256( u32le(len(tag)) || tag
                  || u32le(1)                                   (format version)
                  || for each field: u32le(len(field)) || field )  (fields in UTF-8)
uuid   = digest[0..16), then byte 6 = (byte 6 & 0x0F) | 0x80   (version 8)
                             byte 8 = (byte 8 & 0x3F) | 0x80   (variant 10)
text   = "{" + hex(bytes 0-3) "-" hex(4-5) "-" hex(6-7) "-" hex(8-9) "-" hex(10-15) + "}"
         upper case, bytes in order
```

Length prefixes keep `("ab","c")` and `("a","bc")` apart; the tag keeps different kinds of
identity apart.

| Identity | Tag | Fields |
|---|---|---|
| ComponentId | `rubrapack.component` | upgrade code, scope (`machine`), component architecture, logical target path (`<dir id>/<file name>`), key path kind (`file`), key path name (the File key) |
| ProductCode | `rubrapack.product` | upgrade code, architecture, scope, the version's first three parts |
| package code (reproducible builds) | `rubrapack.package` | hex SHA-256 of the package built with an all-zero package code |

The ProductCode changes with the three-part version - which is what a major upgrade needs - and
the ComponentIds do not.

## Keys

Table keys (`File`, `Component`, `Directory`) are identifiers: `[A-Za-z_][A-Za-z0-9_.]*`, at most
72 characters. rubrapack uses the author's IDs for files and folders as given and derives the rest:

```text
key = prefix + "_" + first 20 lower-case hex digits of SHA-256(logical name)
      prefix C for components (logical name: the File key), D for intermediate folders
      (logical name: "<standard folder>/<part>/<part>..."), F for files from wildcards
```

## Reproducible packages

A package is byte-for-byte reproducible when nothing depends on the clock, the machine or the
order of hash tables: fixed CFB directory times (zero), fixed cabinet dates, string ids assigned in
byte order of the strings, rows sorted by key, streams in a fixed order, no random GUIDs. The one
identity that must differ between package files is the package code; derive it from the content
(as above) when reproducibility matters, and use a random one otherwise.
