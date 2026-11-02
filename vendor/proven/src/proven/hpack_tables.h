/* Generated from the published text of RFC 7541 (Appendix A and Appendix B) by a private
 * program; do not edit. The program stops unless there are 61 static entries and 257
 * Huffman codes that are prefix-free and fill the code space exactly. */
#ifndef PROVEN_HPACK_TABLES_H
#define PROVEN_HPACK_TABLES_H

#define HPACK_STATIC_COUNT 61u
static const struct { const char *name; proven_u8 name_len; const char *value; proven_u8 value_len; } hpack_static[HPACK_STATIC_COUNT] = {
    { ":authority", 10, "", 0 },
    { ":method", 7, "GET", 3 },
    { ":method", 7, "POST", 4 },
    { ":path", 5, "/", 1 },
    { ":path", 5, "/index.html", 11 },
    { ":scheme", 7, "http", 4 },
    { ":scheme", 7, "https", 5 },
    { ":status", 7, "200", 3 },
    { ":status", 7, "204", 3 },
    { ":status", 7, "206", 3 },
    { ":status", 7, "304", 3 },
    { ":status", 7, "400", 3 },
    { ":status", 7, "404", 3 },
    { ":status", 7, "500", 3 },
    { "accept-charset", 14, "", 0 },
    { "accept-encoding", 15, "gzip, deflate", 13 },
    { "accept-language", 15, "", 0 },
    { "accept-ranges", 13, "", 0 },
    { "accept", 6, "", 0 },
    { "access-control-allow-origin", 27, "", 0 },
    { "age", 3, "", 0 },
    { "allow", 5, "", 0 },
    { "authorization", 13, "", 0 },
    { "cache-control", 13, "", 0 },
    { "content-disposition", 19, "", 0 },
    { "content-encoding", 16, "", 0 },
    { "content-language", 16, "", 0 },
    { "content-length", 14, "", 0 },
    { "content-location", 16, "", 0 },
    { "content-range", 13, "", 0 },
    { "content-type", 12, "", 0 },
    { "cookie", 6, "", 0 },
    { "date", 4, "", 0 },
    { "etag", 4, "", 0 },
    { "expect", 6, "", 0 },
    { "expires", 7, "", 0 },
    { "from", 4, "", 0 },
    { "host", 4, "", 0 },
    { "if-match", 8, "", 0 },
    { "if-modified-since", 17, "", 0 },
    { "if-none-match", 13, "", 0 },
    { "if-range", 8, "", 0 },
    { "if-unmodified-since", 19, "", 0 },
    { "last-modified", 13, "", 0 },
    { "link", 4, "", 0 },
    { "location", 8, "", 0 },
    { "max-forwards", 12, "", 0 },
    { "proxy-authenticate", 18, "", 0 },
    { "proxy-authorization", 19, "", 0 },
    { "range", 5, "", 0 },
    { "referer", 7, "", 0 },
    { "refresh", 7, "", 0 },
    { "retry-after", 11, "", 0 },
    { "server", 6, "", 0 },
    { "set-cookie", 10, "", 0 },
    { "strict-transport-security", 25, "", 0 },
    { "transfer-encoding", 17, "", 0 },
    { "user-agent", 10, "", 0 },
    { "vary", 4, "", 0 },
    { "via", 3, "", 0 },
    { "www-authenticate", 16, "", 0 },
};

/* The code of each byte value and of EOS (256): its bits, right-aligned, and its length. */
static const struct { proven_u32 code; proven_u8 bits; } hpack_huffman[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 }, { 0xfffffe4, 28 }, { 0xfffffe5, 28 },
    { 0xfffffe6, 28 }, { 0xfffffe7, 28 }, { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 }, { 0xfffffed, 28 }, { 0xfffffee, 28 },
    { 0xfffffef, 28 }, { 0xffffff0, 28 }, { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 }, { 0xffffff8, 28 }, { 0xffffff9, 28 },
    { 0xffffffa, 28 }, { 0xffffffb, 28 }, { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 },
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 }, { 0x3fa, 10 }, { 0x3fb, 10 },
    { 0xf9, 8 }, { 0x7fb, 11 }, { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 },
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 }, { 0x1a, 6 }, { 0x1b, 6 },
    { 0x1c, 6 }, { 0x1d, 6 }, { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 },
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 }, { 0x1ffa, 13 }, { 0x21, 6 },
    { 0x5d, 7 }, { 0x5e, 7 }, { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 },
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 }, { 0x67, 7 }, { 0x68, 7 },
    { 0x69, 7 }, { 0x6a, 7 }, { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 },
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 }, { 0xfc, 8 }, { 0x73, 7 },
    { 0xfd, 8 }, { 0x1ffb, 13 }, { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 },
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 }, { 0x24, 6 }, { 0x5, 5 },
    { 0x25, 6 }, { 0x26, 6 }, { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 },
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 }, { 0x2b, 6 }, { 0x76, 7 },
    { 0x2c, 6 }, { 0x8, 5 }, { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 },
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 }, { 0x7fc, 11 }, { 0x3ffd, 14 },
    { 0x1ffd, 13 }, { 0xffffffc, 28 }, { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 }, { 0x3fffd6, 22 }, { 0x7fffda, 23 },
    { 0x7fffdb, 23 }, { 0x7fffdc, 23 }, { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 }, { 0xffffee, 24 }, { 0x7fffe1, 23 },
    { 0x7fffe2, 23 }, { 0x7fffe3, 23 }, { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 }, { 0x3fffda, 22 }, { 0x1fffdd, 21 },
    { 0xfffe9, 20 }, { 0x3fffdb, 22 }, { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 }, { 0x1fffdf, 21 }, { 0x3fffdf, 22 },
    { 0x7fffeb, 23 }, { 0x7fffec, 23 }, { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 }, { 0xfffea, 20 }, { 0x3fffe2, 22 },
    { 0x3fffe3, 22 }, { 0x3fffe4, 22 }, { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 }, { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 }, { 0x3fffe7, 22 }, { 0x7ffff2, 23 },
    { 0x3fffe8, 22 }, { 0x1ffffec, 25 }, { 0x3ffffe2, 26 }, { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 },
    { 0x7ffffdf, 27 }, { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 }, { 0x7fff2, 19 }, { 0x1fffe3, 21 },
    { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 }, { 0x7ffffe1, 27 }, { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 }, { 0xffffffd, 28 }, { 0x7ffffe3, 27 },
    { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 }, { 0xfffec, 20 }, { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 },
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 }, { 0x3fffea, 22 }, { 0x3fffeb, 22 },
    { 0x1ffffee, 25 }, { 0x1ffffef, 25 }, { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 },
    { 0x3ffffeb, 26 }, { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 }, { 0x7ffffe7, 27 }, { 0x7ffffe8, 27 },
    { 0x7ffffe9, 27 }, { 0x7ffffea, 27 }, { 0x7ffffeb, 27 }, { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 },
    { 0x7ffffee, 27 }, { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 }, { 0x3fffffff, 30 },
};

/* For decoding. The code is canonical: of the codes of one length, the first, how many
 * there are, and where their symbols begin in hpack_huffman_sorted (by length, then value). */
static const proven_u16 hpack_huffman_sorted[257] = {
    48, 49, 50, 97, 99, 101, 105, 111, 115, 116, 32, 37, 45, 46, 47, 51,
    52, 53, 54, 55, 56, 57, 61, 65, 95, 98, 100, 102, 103, 104, 108, 109,
    110, 112, 114, 117, 58, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76,
    77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 89, 106, 107, 113, 118,
    119, 120, 121, 122, 38, 42, 44, 59, 88, 90, 33, 34, 40, 41, 63, 39,
    43, 124, 35, 62, 0, 36, 64, 91, 93, 126, 94, 125, 60, 96, 123, 92,
    195, 208, 128, 130, 131, 162, 184, 194, 224, 226, 153, 161, 167, 172, 176, 177,
    179, 209, 216, 217, 227, 229, 230, 129, 132, 133, 134, 136, 146, 154, 156, 160,
    163, 164, 169, 170, 173, 178, 181, 185, 186, 187, 189, 190, 196, 198, 228, 232,
    233, 1, 135, 137, 138, 139, 140, 141, 143, 147, 149, 150, 151, 152, 155, 157,
    158, 165, 166, 168, 174, 175, 180, 182, 183, 188, 191, 197, 231, 239, 9, 142,
    144, 145, 148, 159, 171, 206, 215, 225, 236, 237, 199, 207, 234, 235, 192, 193,
    200, 201, 202, 205, 210, 213, 218, 219, 238, 240, 242, 243, 255, 203, 204, 211,
    212, 214, 221, 222, 223, 241, 244, 245, 246, 247, 248, 250, 251, 252, 253, 254,
    2, 3, 4, 5, 6, 7, 8, 11, 12, 14, 15, 16, 17, 18, 19, 20,
    21, 23, 24, 25, 26, 27, 28, 29, 30, 31, 127, 220, 249, 10, 13, 22,
    256,
};
static const proven_u32 hpack_huffman_first[31] = { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x14, 0x5c, 0xf8, 0x0, 0x3f8, 0x7fa, 0xffa, 0x1ff8, 0x3ffc, 0x7ffc, 0x0, 0x0, 0x0, 0x7fff0, 0xfffe6, 0x1fffdc, 0x3fffd2, 0x7fffd8, 0xffffea, 0x1ffffec, 0x3ffffe0, 0x7ffffde, 0xfffffe2, 0x0, 0x3ffffffc };
static const proven_u16 hpack_huffman_count[31] = { 0, 0, 0, 0, 0, 10, 26, 32, 6, 0, 5, 3, 2, 6, 2, 3, 0, 0, 0, 3, 8, 13, 26, 29, 12, 4, 15, 19, 29, 0, 4 };
static const proven_u16 hpack_huffman_offset[31] = { 0, 0, 0, 0, 0, 0, 10, 36, 68, 0, 74, 79, 82, 84, 90, 92, 0, 0, 0, 95, 98, 106, 119, 145, 174, 186, 190, 205, 224, 0, 253 };

/* The next eight bits of input as an index: (symbol << 4) | length for a code of eight bits or
 * fewer that begins there, 0 when the code is longer. */
static const proven_u16 hpack_huffman_fast[256] = {
    0x305, 0x305, 0x305, 0x305, 0x305, 0x305, 0x305, 0x305, 0x315, 0x315, 0x315, 0x315, 0x315, 0x315, 0x315, 0x315,
    0x325, 0x325, 0x325, 0x325, 0x325, 0x325, 0x325, 0x325, 0x615, 0x615, 0x615, 0x615, 0x615, 0x615, 0x615, 0x615,
    0x635, 0x635, 0x635, 0x635, 0x635, 0x635, 0x635, 0x635, 0x655, 0x655, 0x655, 0x655, 0x655, 0x655, 0x655, 0x655,
    0x695, 0x695, 0x695, 0x695, 0x695, 0x695, 0x695, 0x695, 0x6f5, 0x6f5, 0x6f5, 0x6f5, 0x6f5, 0x6f5, 0x6f5, 0x6f5,
    0x735, 0x735, 0x735, 0x735, 0x735, 0x735, 0x735, 0x735, 0x745, 0x745, 0x745, 0x745, 0x745, 0x745, 0x745, 0x745,
    0x206, 0x206, 0x206, 0x206, 0x256, 0x256, 0x256, 0x256, 0x2d6, 0x2d6, 0x2d6, 0x2d6, 0x2e6, 0x2e6, 0x2e6, 0x2e6,
    0x2f6, 0x2f6, 0x2f6, 0x2f6, 0x336, 0x336, 0x336, 0x336, 0x346, 0x346, 0x346, 0x346, 0x356, 0x356, 0x356, 0x356,
    0x366, 0x366, 0x366, 0x366, 0x376, 0x376, 0x376, 0x376, 0x386, 0x386, 0x386, 0x386, 0x396, 0x396, 0x396, 0x396,
    0x3d6, 0x3d6, 0x3d6, 0x3d6, 0x416, 0x416, 0x416, 0x416, 0x5f6, 0x5f6, 0x5f6, 0x5f6, 0x626, 0x626, 0x626, 0x626,
    0x646, 0x646, 0x646, 0x646, 0x666, 0x666, 0x666, 0x666, 0x676, 0x676, 0x676, 0x676, 0x686, 0x686, 0x686, 0x686,
    0x6c6, 0x6c6, 0x6c6, 0x6c6, 0x6d6, 0x6d6, 0x6d6, 0x6d6, 0x6e6, 0x6e6, 0x6e6, 0x6e6, 0x706, 0x706, 0x706, 0x706,
    0x726, 0x726, 0x726, 0x726, 0x756, 0x756, 0x756, 0x756, 0x3a7, 0x3a7, 0x427, 0x427, 0x437, 0x437, 0x447, 0x447,
    0x457, 0x457, 0x467, 0x467, 0x477, 0x477, 0x487, 0x487, 0x497, 0x497, 0x4a7, 0x4a7, 0x4b7, 0x4b7, 0x4c7, 0x4c7,
    0x4d7, 0x4d7, 0x4e7, 0x4e7, 0x4f7, 0x4f7, 0x507, 0x507, 0x517, 0x517, 0x527, 0x527, 0x537, 0x537, 0x547, 0x547,
    0x557, 0x557, 0x567, 0x567, 0x577, 0x577, 0x597, 0x597, 0x6a7, 0x6a7, 0x6b7, 0x6b7, 0x717, 0x717, 0x767, 0x767,
    0x777, 0x777, 0x787, 0x787, 0x797, 0x797, 0x7a7, 0x7a7, 0x268, 0x2a8, 0x2c8, 0x3b8, 0x588, 0x5a8, 0x0, 0x0,
};

/* For encoding. The distinct names by length: those of length n are
 * hpack_static_names[hpack_static_by_len[n] .. hpack_static_by_len[n + 1]), each the index of
 * its first entry in hpack_static and how many entries (neighbours) have that name. */
#define HPACK_STATIC_LONGEST_NAME 27u
static const proven_u8 hpack_static_by_len[29] = { 0, 0, 0, 0, 2, 8, 11, 15, 21, 24, 24, 27, 28, 30, 36, 38, 40, 44, 46, 47, 50, 50, 50, 50, 50, 50, 51, 51, 52 };
static const struct { proven_u8 first, count; } hpack_static_names[52] = {
    { 20, 1 }, { 59, 1 }, { 32, 1 }, { 33, 1 }, { 36, 1 }, { 37, 1 }, { 44, 1 }, { 58, 1 },
    { 3, 2 }, { 21, 1 }, { 49, 1 }, { 18, 1 }, { 31, 1 }, { 34, 1 }, { 53, 1 }, { 1, 2 },
    { 5, 2 }, { 7, 7 }, { 35, 1 }, { 50, 1 }, { 51, 1 }, { 38, 1 }, { 41, 1 }, { 45, 1 },
    { 0, 1 }, { 54, 1 }, { 57, 1 }, { 52, 1 }, { 30, 1 }, { 46, 1 }, { 17, 1 }, { 22, 1 },
    { 23, 1 }, { 29, 1 }, { 40, 1 }, { 43, 1 }, { 14, 1 }, { 27, 1 }, { 15, 1 }, { 16, 1 },
    { 25, 1 }, { 26, 1 }, { 28, 1 }, { 60, 1 }, { 39, 1 }, { 56, 1 }, { 47, 1 }, { 24, 1 },
    { 42, 1 }, { 48, 1 }, { 55, 1 }, { 19, 1 },
};

#endif /* PROVEN_HPACK_TABLES_H */
