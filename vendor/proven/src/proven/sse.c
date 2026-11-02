#include "proven/sse.h"

/* The event-stream parsing rules of the HTML standard ("9.2.6 Interpreting an event stream"),
 * one byte at a time, so that input may be cut anywhere. */

proven_err_t proven_sse_init(proven_sse_t *sse, proven_mem_mut_t work) {
    if (!sse) return PROVEN_ERR_INVALID_ARG;
    *sse = (proven_sse_t){0};
    if (!work.ptr || work.size < 64) return PROVEN_ERR_INVALID_ARG;
    sse->line = work.ptr;
    sse->line_cap = work.size / 2;
    sse->data = work.ptr + sse->line_cap;
    sse->data_cap = work.size - sse->line_cap;
    return PROVEN_OK;
}

static bool sse_is(proven_u8str_view_t s, const char *lit) {
    proven_size_t n = 0;
    while (lit[n] != '\0') n++;
    if (s.size != n) return false;
    for (proven_size_t i = 0; i < n; ++i) if (s.ptr[i] != (proven_byte_t)lit[i]) return false;
    return true;
}

/* One complete line (without its terminator). Returns true when it ends an event that is to
 * be delivered. */
static proven_err_t sse_line(proven_sse_t *s, bool *dispatch) {
    *dispatch = false;
    proven_u8str_view_t line = { .ptr = s->line, .size = s->line_len };

    if (line.size == 0) {
        /* An empty line ends the event. With no data line there is nothing to deliver, and
         * the type is forgotten; the id is not. */
        if (!s->has_data) {
            s->type_len = 0;
            s->has_retry = false;
            return PROVEN_OK;
        }
        *dispatch = true;
        return PROVEN_OK;
    }
    if (line.ptr[0] == ':') return PROVEN_OK;                    /* a comment */

    proven_size_t colon = 0;
    while (colon < line.size && line.ptr[colon] != ':') colon++;
    proven_u8str_view_t field = { .ptr = line.ptr, .size = colon };
    proven_u8str_view_t value = { .ptr = line.ptr + line.size, .size = 0 };
    if (colon < line.size) {
        proven_size_t v = colon + 1;
        if (v < line.size && line.ptr[v] == ' ') v++;           /* exactly one leading space is dropped */
        value = (proven_u8str_view_t){ .ptr = line.ptr + v, .size = line.size - v };
    }

    if (sse_is(field, "data")) {
        /* Each data line is followed by LF; the last one is removed when the event is sent. */
        if (value.size + 1 > s->data_cap - s->data_len) return PROVEN_ERR_OUT_OF_BOUNDS;
        for (proven_size_t i = 0; i < value.size; ++i) s->data[s->data_len + i] = value.ptr[i];
        s->data_len += value.size;
        s->data[s->data_len++] = '\n';
        s->has_data = true;
    } else if (sse_is(field, "event")) {
        if (value.size > sizeof s->type) return PROVEN_ERR_OUT_OF_BOUNDS;
        for (proven_size_t i = 0; i < value.size; ++i) s->type[i] = value.ptr[i];
        s->type_len = value.size;
    } else if (sse_is(field, "id")) {
        bool has_nul = false;
        for (proven_size_t i = 0; i < value.size; ++i) if (value.ptr[i] == 0) has_nul = true;
        if (!has_nul) {
            if (value.size > sizeof s->id) return PROVEN_ERR_OUT_OF_BOUNDS;
            for (proven_size_t i = 0; i < value.size; ++i) s->id[i] = value.ptr[i];
            s->id_len = value.size;
        }
    } else if (sse_is(field, "retry")) {
        proven_u64 ms = 0;
        bool digits = value.size > 0;
        for (proven_size_t i = 0; i < value.size && digits; ++i) {
            if (value.ptr[i] < '0' || value.ptr[i] > '9') digits = false;
            else if (ms <= 0xffffffffull) ms = ms * 10u + (proven_u64)(value.ptr[i] - '0');
        }
        if (digits) {
            s->retry_ms = ms > 0xffffffffull ? 0xffffffffu : (proven_u32)ms;
            s->has_retry = true;
        }
    }
    /* Any other field name is ignored. */
    return PROVEN_OK;
}

proven_err_t proven_sse_feed(proven_sse_t *sse, proven_mem_view_t in, proven_size_t *consumed,
                             proven_sse_event_t *event, bool *have_event) {
    if (consumed) *consumed = 0;
    if (have_event) *have_event = false;
    if (!sse || !consumed || !event || !have_event || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (sse->failed || !sse->line) return PROVEN_ERR_INVALID_STATE;

    for (proven_size_t i = 0; i < in.size; ++i) {
        proven_byte_t c = in.ptr[i];

        /* A UTF-8 byte order mark at the very start of the stream is skipped. */
        if (!sse->bom_checked) {
            static const proven_byte_t bom[3] = { 0xef, 0xbb, 0xbf };
            if (c == bom[sse->bom_seen]) {
                sse->bom_seen++;
                if (sse->bom_seen == 3) { sse->bom_checked = true; sse->line_len = 0; continue; }
                /* Kept in the line as well, in case this turns out not to be a mark. */
            } else {
                sse->bom_checked = true;
            }
        }

        if (sse->skip_lf) {
            sse->skip_lf = false;
            if (c == '\n') continue;                             /* the LF of a CRLF */
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r') sse->skip_lf = true;
            sse->bom_checked = true;
            bool dispatch = false;
            proven_err_t e = sse_line(sse, &dispatch);
            sse->line_len = 0;
            if (e != PROVEN_OK) { sse->failed = true; return e; }
            if (dispatch) {
                event->event = (proven_u8str_view_t){ .ptr = sse->type, .size = sse->type_len };
                event->data = (proven_u8str_view_t){ .ptr = sse->data, .size = sse->data_len - 1 };
                event->id = (proven_u8str_view_t){ .ptr = sse->id, .size = sse->id_len };
                event->retry_ms = sse->retry_ms;
                event->has_retry = sse->has_retry;
                /* The buffers stay as they are until the next feed, which is when the views
                 * expire; only the bookkeeping is reset now. */
                sse->data_len = 0;
                sse->type_len = 0;
                sse->has_data = false;
                sse->has_retry = false;
                *have_event = true;
                *consumed = i + 1;
                return PROVEN_OK;
            }
            continue;
        }
        if (sse->line_len >= sse->line_cap) { sse->failed = true; return PROVEN_ERR_OUT_OF_BOUNDS; }
        sse->line[sse->line_len++] = c;
    }
    *consumed = in.size;
    return PROVEN_OK;
}

proven_u8str_view_t proven_sse_last_id(const proven_sse_t *sse) {
    if (!sse) return (proven_u8str_view_t){0};
    return (proven_u8str_view_t){ .ptr = sse->id, .size = sse->id_len };
}
