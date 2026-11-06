#ifndef PROVEN_NET_UMBRELLA_H
#define PROVEN_NET_UMBRELLA_H

/**
 * @file proven_net.h
 * @brief Entry point for the network library, proven_net (RFC-0043): TLS, HTTP/1.1, HTTP/2 and
 *        HTTP/3, WebSocket, server-sent events, sockets and the event loop. It stands on the base
 *        library's public headers alone and is built from the same tree, under one version.
 *
 * Its headers keep the names they always had (proven/http.h, proven/tls.h, ...) and live in
 * net/include: add that directory to the include path, compile net/src and net/platform with the
 * base library, and proven.h includes this file too. The codecs (http, hpack, h2, sse, ws) and the
 * TLS engine are pure computation and part of the freestanding profile; sockets, the loop and the
 * drivers are hosted only and left out by PROVEN_NO_NET.
 */

#include "proven.h"
#include "proven/tls.h"
#include "proven/http.h"
#include "proven/hpack.h"
#include "proven/h2.h"
#include "proven/http_auth.h"
#include "proven/http_cookie.h"
#include "proven/sse.h"
#include "proven/ws.h"
#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
#include "proven/net.h"
#include "proven/http_client.h"
#include "proven/http_server.h"
#include "proven/ws_conn.h"
#include "proven/loop.h"
#include "proven/http_event.h"
#include "proven/ws_event.h"
#include "proven/http_event_client.h"
#endif

#endif /* PROVEN_NET_UMBRELLA_H */
