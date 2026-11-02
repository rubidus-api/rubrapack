#ifndef PROVEN_H
#define PROVEN_H

/**
 * @file proven.h
 * @brief Unified entry point for the proven library.
 */

#include "proven/types.h"
#include "proven/config.h"
#include "proven/version.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/align.h"
#include "proven/allocator.h"
#include "proven/heap.h"
#include "proven/alloc_check.h"
#include "proven/arena.h"
#include "proven/pool.h"
#include "proven/buffer.h"
#include "proven/u8str.h"
#include "proven/u16str.h"
#include "proven/array.h"
#include "proven/list.h"
#include "proven/ring.h"
#include "proven/map.h"
#include "proven/algorithm.h"
#include "proven/hash.h"
#include "proven/hash_legacy.h"
#include "proven/hmac.h"
#include "proven/deflate.h"
#include "proven/zstd.h"
#ifndef PROVEN_NO_BROTLI
#include "proven/brotli.h"
#endif
#include "proven/cert.h"
#include "proven/tls.h"
#include "proven/url.h"
#include "proven/http.h"
#include "proven/hpack.h"
#include "proven/h2.h"
#include "proven/http_auth.h"
#include "proven/http_cookie.h"
#include "proven/sse.h"
#include "proven/ws.h"
#include "proven/encode.h"
#include "proven/utf.h"
#include "proven/random.h"
#include "proven/fs.h"
#include "proven/time.h"
#include "proven/fmt.h"
#include "proven/float_parse.h"
#include "proven/float_format.h"
#include "proven/mmap.h"
#ifndef PROVEN_FREESTANDING
#include "proven/stream.h"
#ifndef PROVEN_NO_NET
#include "proven/net.h"
#endif
#endif
#include "proven/sysio.h"
#include "proven/job.h"
#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
#include "proven/http_client.h"
#include "proven/http_server.h"
#include "proven/ws_conn.h"
#include "proven/loop.h"
#include "proven/http_event.h"
#include "proven/ws_event.h"
#include "proven/http_event_client.h"
#endif
#include "proven/scan.h"
#include "proven/coro.h"

#endif /* PROVEN_H */
