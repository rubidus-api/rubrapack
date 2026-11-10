#ifndef PROVEN_CODEC_UMBRELLA_H
#define PROVEN_CODEC_UMBRELLA_H

/**
 * @file proven_codec.h
 * @brief Entry point for the compression library, proven_codec (RFC-0045): DEFLATE with zlib and
 *        gzip framing both ways, Zstandard both ways, Brotli decompression. It stands on the base
 *        library's public headers alone (CRC-32 is the base's hash.h) and is built from the same
 *        tree, under one version.
 *
 * Its headers keep the names they always had (proven/deflate.h, proven/zstd.h, proven/brotli.h)
 * and live in codec/include: add that directory to the include path, compile codec/src and
 * codec/platform with the base library, and proven.h includes this file too. All of it is pure
 * computation and part of the freestanding profile; PROVEN_NO_BROTLI leaves Brotli out.
 */

#include "proven.h"
#include "proven/deflate.h"
#include "proven/zstd.h"
#ifndef PROVEN_NO_BROTLI
#include "proven/brotli.h"
#endif

#endif /* PROVEN_CODEC_UMBRELLA_H */
