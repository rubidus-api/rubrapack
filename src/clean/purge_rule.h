// src/clean/purge_rule.h - which registry keys a removal may delete with everything below them
// ([registry] remove = true, on = "uninstall"), checked when the package is built (src/model) and
// again by the helper DLL before it deletes one (src/ca: a package changed by hand). Plain C, no
// Windows header: tests/test_keypath.c runs it on Linux.
//
// A key of the product's own below Software: "Software\<a>\..." with no empty part and no '\' at
// either end. Software\Wow6432Node\ counts as Software\ (it is the same keys' 32-bit view). Below
// Microsoft, Classes, Policies and Wow6432Node it takes two more parts (Software\Microsoft\<product>
// is not one's own); Software\Microsoft\Windows, ...\Windows NT and Software\Policies\Microsoft are
// Windows' own at any depth.

#ifndef RUBRAPACK_PURGE_RULE_H
#define RUBRAPACK_PURGE_RULE_H

#include <stdbool.h>
#include <stddef.h>

// Part `i` of `key` ('\'-separated, UTF-8) equals `name` (ASCII) as Windows compares key names:
// ignoring case - in which U+0131 (dotless i, C4 B1) is an 'I' and U+017F (long s, C5 BF) an 'S'
// (their capitals are ASCII); any other byte past ASCII is no ASCII letter.
static inline bool purge_part_is(const char *const *part, const size_t *len, size_t i, size_t count, const char *name) {
    if (i >= count) return false;
    const unsigned char *p = (const unsigned char *)part[i], *end = p + len[i];
    for (const char *q = name; *q; ++q) {
        if (p == end) return false;
        char a;
        if (p + 1 < end && p[0] == 0xC4 && p[1] == 0xB1) { a = 'i'; p += 2; }
        else if (p + 1 < end && p[0] == 0xC5 && p[1] == 0xBF) { a = 's'; p += 2; }
        else a = (char)*p++;
        char b = *q;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return p == end;
}

static inline bool purge_key_ok(const char *key) {
    enum { MAX_PARTS = 64 };
    const char *part[MAX_PARTS];
    size_t len[MAX_PARTS], count = 0;
    if (key == NULL || key[0] == 0) return false;
    for (const char *p = key;;) {
        const char *q = p;
        while (*q && *q != '\\') ++q;
        if (q == p || count == MAX_PARTS) return false;     // an empty part ("\\", or '\' at an end)
        part[count] = p;
        len[count++] = (size_t)(q - p);
        if (*q == 0) break;
        p = q + 1;
    }
    if (!purge_part_is(part, len, 0, count, "Software")) return false;
    size_t s = purge_part_is(part, len, 1, count, "Wow6432Node") ? 2 : 1;     // the first part below Software
    if (count <= s) return false;
    if (purge_part_is(part, len, s, count, "Microsoft") &&
        (purge_part_is(part, len, s + 1, count, "Windows") || purge_part_is(part, len, s + 1, count, "Windows NT"))) return false;
    if (purge_part_is(part, len, s, count, "Policies") && purge_part_is(part, len, s + 1, count, "Microsoft")) return false;
    static const char *const wide[] = { "Microsoft", "Classes", "Policies", "Wow6432Node" };
    for (size_t k = 0; k < sizeof wide / sizeof wide[0]; ++k) {
        if (purge_part_is(part, len, s, count, wide[k]) && count - s < 3) return false;
    }
    return true;
}

#endif
