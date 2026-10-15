// src/ca/rp_script.h - the runs of a details box's text that need a face of their own (RFC-0027).
// Included by the helper DLL; plain C over UTF-16 code units, so a test can build it anywhere.
//
// A run is a stretch of one script group: Chinese, Japanese and Korean letters (with the CJK
// punctuation and full-width forms among them), the Indic scripts (Devanagari to Malayalam), or
// Thai. Its face: Kana anywhere in a CJK run makes it Japanese, else Hangul makes it Korean, else
// it is Han only (the caller picks that face by the system's UI language). Positions count as a
// RichEdit control counts them: "\r\n" is one position, a surrogate pair two.

#ifndef RUBRAPACK_SCRIPT_H
#define RUBRAPACK_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RP_FACE_NONE, RP_FACE_HANGUL, RP_FACE_KANA, RP_FACE_HAN, RP_FACE_INDIC, RP_FACE_THAI, RP_FACE_COUNT };

typedef struct {
    size_t from, to;        // RichEdit positions, `to` past the last
    int    face;            // RP_FACE_*
} rp_script_run_t;

// The class of the code unit at s[i] ('K' Hangul, 'J' Kana, 'H' Han, 'N' CJK punctuation and
// full-width forms, 'I' Indic, 'T' Thai, 0 none) and how many units it takes.
static inline int rp_script_class(const uint16_t *s, size_t n, size_t i, size_t *units) {
    uint16_t c = s[i];
    *units = 1;
    if (c >= 0xD840 && c <= 0xD8BF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        *units = 2;                                 // U+20000-U+3FFFF: Han extensions B to H
        return 'H';
    }
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) ||
        (c >= 0xA960 && c <= 0xA97F) || (c >= 0xD7B0 && c <= 0xD7FF)) return 'K';
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F)) return 'J';
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3100 && c <= 0x312F) || (c >= 0x3190 && c <= 0x31BF)) return 'H';
    if ((c >= 0x3000 && c <= 0x303F) || (c >= 0x3200 && c <= 0x33FF) || (c >= 0xFF00 && c <= 0xFFEF)) return 'N';
    if (c >= 0x0900 && c <= 0x0DFF) return 'I';
    if (c >= 0x0E00 && c <= 0x0E7F) return 'T';
    return 0;
}

// The next run at or after unit *i (position *pos); false when there is none. Call with *i = 0
// and *pos = 0 first; both move on.
static inline bool rp_script_next(const uint16_t *s, size_t n, size_t *i, size_t *pos, rp_script_run_t *run) {
    int group = 0;                                  // 'C' CJK, 'I', 'T'
    bool k = false, j = false, h = false;
    size_t from = 0;
    while (*i < n) {
        if (s[*i] == '\n' && *i > 0 && s[*i - 1] == '\r') {
            ++*i;                                   // "\r\n" is one position
            continue;
        }
        size_t units;
        int cls = rp_script_class(s, n, *i, &units);
        int g = cls == 'K' || cls == 'J' || cls == 'H' || cls == 'N' ? 'C' : cls;
        if (g != group && group != 0 && (group != 'C' || k || j || h)) {
            run->from = from;
            run->to = *pos;
            run->face = group == 'I' ? RP_FACE_INDIC : group == 'T' ? RP_FACE_THAI : j ? RP_FACE_KANA : k ? RP_FACE_HANGUL : RP_FACE_HAN;
            return true;
        }
        if (g != group) {
            group = g;
            from = *pos;
            k = j = h = false;
        }
        k |= cls == 'K';
        j |= cls == 'J';
        h |= cls == 'H';
        *i += units;
        *pos += units;
    }
    if (group != 0 && (group != 'C' || k || j || h)) {
        run->from = from;
        run->to = *pos;
        run->face = group == 'I' ? RP_FACE_INDIC : group == 'T' ? RP_FACE_THAI : j ? RP_FACE_KANA : k ? RP_FACE_HANGUL : RP_FACE_HAN;
        return true;
    }
    return false;
}

#endif
