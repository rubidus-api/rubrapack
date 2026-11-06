#ifndef PROVEN_SSE_H
#define PROVEN_SSE_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"

/**
 * @file sse.h
 * @brief A parser for Server-Sent Events: the `text/event-stream` format.
 *
 * An event stream is a response body that never has to end: the server writes small text
 * records as things happen, and the client reads them as they arrive. This header reads that
 * format. It does not make the request - feed it the body bytes of a response whose
 * `Content-Type` is `text/event-stream`, in whatever pieces they come.
 *
 * It follows the parsing rules of the HTML standard exactly, including the ones that look odd:
 * a line may end in CRLF, LF or CR alone; a line starting with `:` is a comment (servers send
 * them as keep-alives); several `data:` lines are one event whose data has newlines between
 * them; an `id` containing a NUL is ignored; a `retry` that is not all digits is ignored.
 *
 * Pure text handling over memory you supply; available in freestanding builds.
 */

/** @brief One event. The views are good until the next call to proven_sse_feed. */
typedef struct {
    proven_u8str_view_t event;      /**< the event type; empty means the default, "message" */
    proven_u8str_view_t data;       /**< the data lines joined with LF; no trailing LF */
    proven_u8str_view_t id;         /**< the last event id seen so far, in this or an earlier event */
    proven_u32 retry_ms;            /**< valid when has_retry: how long to wait before reconnecting */
    bool has_retry;
} proven_sse_event_t;

/**
 * @brief The state of an event-stream parser. Caller-owned; start it with proven_sse_init.
 *
 * Its fields are exposed only so it can live where you put it. Do not read or write them.
 */
typedef struct {
    proven_byte_t *line;            /* the line being gathered */
    proven_size_t line_cap;
    proven_size_t line_len;
    proven_byte_t *data;            /* the data of the event being gathered */
    proven_size_t data_cap;
    proven_size_t data_len;
    proven_byte_t type[64];
    proven_size_t type_len;
    proven_byte_t id[128];
    proven_size_t id_len;
    proven_u32 retry_ms;
    bool has_retry;
    bool has_data;                  /* at least one data line was seen */
    bool skip_lf;                   /* the last byte was CR: a following LF belongs to it */
    bool bom_checked;
    proven_u8 bom_seen;
    bool failed;
} proven_sse_t;

/**
 * @brief Begin parsing a stream.
 * @param work memory for the parser: half holds the line being read, half the data of the
 *        event being assembled. An event whose data, or a line that, does not fit is an error,
 *        so size it for the largest event you are prepared to accept. At least 64 bytes.
 * @return PROVEN_ERR_INVALID_ARG when `work` is smaller than that.
 */
[[nodiscard]]
proven_err_t proven_sse_init(proven_sse_t *sse, proven_mem_mut_t work);

/**
 * @brief Feed the parser more of the stream.
 *
 * Consumes bytes of `in` until an event is complete or `in` is used up; `*consumed` says how
 * many. When `*have_event` is true, `*event` holds an event: use it, then call again with the
 * rest of `in`. When it is false, everything was consumed and no event is ready yet.
 *
 * An event with no `data` line is not delivered, as the standard says - though its `id` and
 * `retry` are remembered.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when a line or an event's data outgrows `work`; the stream
 *         cannot be followed any further, and later calls return PROVEN_ERR_INVALID_STATE.
 */
[[nodiscard]]
proven_err_t proven_sse_feed(proven_sse_t *sse, proven_mem_view_t in, proven_size_t *consumed,
                             proven_sse_event_t *event, bool *have_event);

/** @brief The last event id received, to send as `Last-Event-ID` when reconnecting. Empty when
 *         the stream has given none. Good until the next proven_sse_feed. */
[[nodiscard]]
proven_u8str_view_t proven_sse_last_id(const proven_sse_t *sse);

#endif /* PROVEN_SSE_H */
