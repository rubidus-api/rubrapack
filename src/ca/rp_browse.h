// src/ca/rp_browse.h - the install folder made from a folder chosen in Windows' folder picker
// (RpBrowseFolder, RFC-0033). No Windows calls: tests/test_keypath.c runs it on Linux.

#ifndef RP_BROWSE_H
#define RP_BROWSE_H

#include <stdbool.h>
#include <stddef.h>

#include "rp_wstr.h"

// The install folder for `picked`: the product's folder name `leaf` added unless `picked` already
// ends in it (ASCII case ignored; Windows compares names so), and one '\' at the end. "D:\" and
// "D:\Apps" give "D:\<leaf>\" and "D:\Apps\<leaf>\"; "D:\Apps\<leaf>" gives itself with its '\'.
// An empty `leaf` adds nothing. False (out empty) when `picked` is empty or the result does not fit.
static inline bool rp_browse_compose(const wchar_t *picked, const wchar_t *leaf, wchar_t *out, size_t cap) {
    if (cap == 0) return false;
    out[0] = 0;
    size_t n = picked ? rp_wlen(picked) : 0;
    while (n > 3 && picked[n - 1] == L'\\') --n;       // "D:\" keeps its '\'
    if (n == 0 || n + 1 > cap) return false;
    for (size_t i = 0; i < n; ++i) out[i] = picked[i];
    out[n] = 0;
    // The last name of the folder: after the last '\' (none for "D:\").
    const wchar_t *last = rp_wrchr(out, L'\\');
    last = last ? last + 1 : out;
    bool has = leaf && leaf[0] && last[0] && rp_wieq(last, leaf);
    if (out[n - 1] != L'\\' && !rp_wcat(out, cap, L"\\")) return out[0] = 0, false;
    if (leaf && leaf[0] && !has && !(rp_wcat(out, cap, leaf) && rp_wcat(out, cap, L"\\"))) return out[0] = 0, false;
    return true;
}

#endif
