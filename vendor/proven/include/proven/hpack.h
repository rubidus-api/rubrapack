#ifndef PROVEN_HPACK_H
#define PROVEN_HPACK_H

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/http.h"

/**
 * @file hpack.h
 * @brief HPACK (RFC 7541): the header compression of HTTP/2.
 *
 * HTTP/2 does not send header fields as text. Each side keeps a small table of fields it has
 * sent before - the *dynamic table* - and a fixed table of common ones that both sides know -
 * the *static table* - and sends a field as a reference into a table where it can, as a
 * literal where it cannot. Literals may be Huffman-coded. The two ends' tables must stay
 * identical: every header block is decoded, in order, by the one decoder of its connection,
 * or the connection is over.
 *
 * This header is that codec and nothing else: an encoder and a decoder, each with its table.
 * It is pure computation and available in a freestanding build. Framing (which bytes are a
 * header block) is h2.h's.
 *
 * **What a peer can make this side hold or do is bounded, and by you:**
 *   - the dynamic table by `max_table_bytes` (the setting HTTP/2 calls
 *     SETTINGS_HEADER_TABLE_SIZE; 4096 by default; 0 keeps no table at all);
 *   - the decoded header list by `max_list_bytes`, counted as the RFC counts it: the length of
 *     each name and value plus 32 a field. A few bytes that name a large table entry a
 *     thousand times are stopped by it;
 *   - integers and strings by the block they are in.
 *
 * A decoder's table is one allocation made when the first entry is inserted, and an
 * encoder's likewise: a connection that has exchanged no header that could be indexed holds
 * neither.
 */

/** @brief The table size HTTP/2 assumes until a setting says otherwise. */
#define PROVEN_HPACK_DEFAULT_TABLE_BYTES ((proven_size_t)4096)

/** @brief What a table entry costs beyond its name and value, by RFC 7541 section 4.1. */
#define PROVEN_HPACK_ENTRY_OVERHEAD ((proven_size_t)32)

// -----------------------------------------------------------------------------
// Decoding
// -----------------------------------------------------------------------------

/** @brief A decoder: the receiving half of one connection's header compression. Opaque. */
typedef struct proven_hpack_decoder proven_hpack_decoder_t;

/**
 * @brief Make a decoder.
 * @param max_table_bytes the largest dynamic table the peer's encoder may use: what this side
 *        announces. PROVEN_HPACK_DEFAULT_TABLE_BYTES is the protocol's default.
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM. (The table itself is allocated later, and
 *         grows as entries are added: `max_table_bytes` is its ceiling, not its cost.)
 */
[[nodiscard]]
proven_err_t proven_hpack_decoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_decoder_t **out);

/** @brief Free a decoder. Null is accepted. */
void proven_hpack_decoder_destroy(proven_hpack_decoder_t *decoder);

/**
 * @brief Change the largest table the peer may use - in HTTP/2, when the peer has
 *        acknowledged a new SETTINGS_HEADER_TABLE_SIZE.
 *
 * If the new size is below what the table may hold now, the peer must begin its next header
 * block by saying so (a "dynamic table size update"), and a block that does not is refused:
 * the two tables would no longer be the same.
 */
void proven_hpack_decoder_set_max_table_bytes(proven_hpack_decoder_t *decoder, proven_size_t max_table_bytes);

/**
 * @brief Decode one whole header block.
 *
 * The fields are written to `headers` in the order they were sent. A name or value is a view
 * into `block` when it was sent as plain bytes, into the static table when it came from
 * there, and into `scratch` otherwise (Huffman-coded, or taken from the dynamic table, which
 * the rest of the block may change): all three must outlive your use of the fields.
 * `scratch` of `max_list_bytes` bytes is always enough.
 *
 * @param max_list_bytes the largest header list to accept, counted as the name and value of
 *        each field plus 32; 0 means no limit.
 * @return PROVEN_OK with `*count` fields.
 *         PROVEN_ERR_OUT_OF_BOUNDS when the list is larger than `max_list_bytes`, has more
 *         fields than `header_cap`, or does not fit `scratch`: **the fields are not to be
 *         used, but the decoder is still good** - the block was read to its end and the
 *         table kept in step, so the connection can go on (in HTTP/2: refuse the stream).
 *         (One field that alone is larger than the whole of `scratch` cannot be followed,
 *         and is the next error instead.)
 *         PROVEN_ERR_INVALID_FORMAT when the block is not valid HPACK - an index that names
 *         nothing, an integer or a string that runs past the block or is too large, Huffman
 *         coding that ends wrongly, a table size update above the limit or not at the start,
 *         a missing one after the limit was lowered, a block that ends inside a field: **the
 *         decoder is then unusable and repeats the error** (in HTTP/2: a connection error,
 *         COMPRESSION_ERROR).
 */
[[nodiscard]]
proven_err_t proven_hpack_decode(proven_hpack_decoder_t *decoder, proven_mem_view_t block,
                                 proven_http_header_t *headers, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count);

/**
 * @brief proven_hpack_decode, and which fields came in the form that must never be indexed
 *        (RFC 7541, section 6.2.3). Something that passes fields on - a proxy - must send
 *        those the same way: give `never_indexed` to proven_hpack_encode as its `sensitive`.
 * @param never_indexed `header_cap` flags, filled for the fields given out. Null: as
 *        proven_hpack_decode.
 */
[[nodiscard]]
proven_err_t proven_hpack_decode_marked(proven_hpack_decoder_t *decoder, proven_mem_view_t block,
                                        proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                        proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count);

/** @brief How many entries the decoder's dynamic table holds, and their size as the RFC
 *         counts it (names, values and 32 an entry). For tests, and for the curious. */
[[nodiscard]]
proven_size_t proven_hpack_decoder_table_count(const proven_hpack_decoder_t *decoder);
[[nodiscard]]
proven_size_t proven_hpack_decoder_table_bytes(const proven_hpack_decoder_t *decoder);

/** @brief Entry `index` of the decoder's dynamic table, 0 being the newest. The views are
 *         good until the next decode. False when there is no such entry. */
[[nodiscard]]
bool proven_hpack_decoder_table_entry(const proven_hpack_decoder_t *decoder, proven_size_t index,
                                      proven_u8str_view_t *name, proven_u8str_view_t *value);

// -----------------------------------------------------------------------------
// Encoding
// -----------------------------------------------------------------------------

/** @brief An encoder: the sending half of one connection's header compression. Opaque. */
typedef struct proven_hpack_encoder proven_hpack_encoder_t;

/**
 * @brief Make an encoder.
 * @param max_table_bytes the largest dynamic table this encoder will use, whatever the peer
 *        allows. 0 makes it stateless: every field is sent from the static table or as a
 *        literal, and nothing is remembered.
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_hpack_encoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_encoder_t **out);

/** @brief Free an encoder. Null is accepted. */
void proven_hpack_encoder_destroy(proven_hpack_encoder_t *encoder);

/**
 * @brief Tell the encoder how large a table the peer allows - in HTTP/2, the peer's
 *        SETTINGS_HEADER_TABLE_SIZE, once received. Until this is called the protocol's
 *        default applies. The change is announced at the start of the next block, as the
 *        protocol requires.
 */
void proven_hpack_encoder_set_peer_table_bytes(proven_hpack_encoder_t *encoder, proven_size_t peer_max_table_bytes);

/** @brief Room that is always enough for proven_hpack_encode to write these fields. */
[[nodiscard]]
proven_size_t proven_hpack_encode_bound(const proven_http_header_t *headers, proven_size_t count);

/**
 * @brief Encode a header list as one header block, appended at `out.ptr[*len]`.
 *
 * Names must already be lowercase, as HTTP/2 requires of them; this does not check or change
 * what it is given. A field is sent as a table reference when a table has it, and otherwise
 * as a literal that is Huffman-coded when that is shorter and remembered in the dynamic
 * table - **except** where remembering would be a risk: a table is a compression context, and
 * a secret placed in one can be guessed from the size of later blocks. So the values of
 * `authorization` and `proxy-authorization` are sent in the form that tells every hop never
 * to index them, `cookie` and `set-cookie` are not remembered, and any field you mark in
 * `sensitive` is treated like the first two. Not remembered either, because their values are
 * seldom the same twice and would push out of the table what is: `content-length`,
 * `content-range`, `etag`, `age`, `if-none-match`, `if-modified-since`.
 *
 * @param sensitive optional: `count` flags, true for a field whose value must not be
 *        remembered by anyone. Null: none beyond the ones named above.
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `out` has less room than proven_hpack_encode_bound
 *         says: nothing was written and the encoder is unchanged. PROVEN_ERR_INVALID_ARG;
 *         PROVEN_ERR_NOMEM when the table could not be allocated (nothing written).
 */
[[nodiscard]]
proven_err_t proven_hpack_encode(proven_hpack_encoder_t *encoder, const proven_http_header_t *headers, proven_size_t count,
                                 const bool *sensitive, proven_mem_mut_t out, proven_size_t *len);

// -----------------------------------------------------------------------------
// Huffman coding of one string (RFC 7541, section 5.2 and Appendix B)
// -----------------------------------------------------------------------------

/** @brief How many bytes `text` takes Huffman-coded. */
[[nodiscard]]
proven_size_t proven_hpack_huffman_size(proven_u8str_view_t text);

/** @brief Huffman-code `text`, appended at `out.ptr[*len]`. The last byte is padded with ones.
 *  @return PROVEN_ERR_OUT_OF_BOUNDS with nothing written when it does not fit. */
[[nodiscard]]
proven_err_t proven_hpack_huffman_encode(proven_u8str_view_t text, proven_mem_mut_t out, proven_size_t *len);

/**
 * @brief Decode a Huffman-coded string, appended at `out.ptr[*len]`.
 * @return PROVEN_ERR_INVALID_FORMAT for padding of more than seven bits, padding that is not
 *         all ones, or the end-of-string symbol inside the string; PROVEN_ERR_OUT_OF_BOUNDS
 *         when `out` is too small (the decoded string is at most 8/5 of the coded one).
 *         `*len` is unchanged on any error.
 */
[[nodiscard]]
proven_err_t proven_hpack_huffman_decode(proven_mem_view_t coded, proven_mem_mut_t out, proven_size_t *len);

#endif /* PROVEN_HPACK_H */
