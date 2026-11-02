#ifndef PROVEN_INTERNAL_QUIC_H
#define PROVEN_INTERNAL_QUIC_H

/* QUIC version 1 below the transport (RFC 9000, RFC 9001): variable-length integers, packet
 * numbers, packet protection, the Retry integrity tag, transport parameters, and TLS 1.3 in
 * QUIC mode. Internal: nothing here is public API, and tests reach it by path. Left out of a
 * build compiled with PROVEN_NO_HTTP3. */

#include "proven_internal_tls.h"

#ifndef PROVEN_NO_HTTP3

#define PROVEN_QUIC_VERSION_1 ((proven_u32)0x00000001)
#define PROVEN_QUIC_VERSION_2 ((proven_u32)0x6b3343cf)         /* RFC 9369 (RFC-0036) */
#define PROVEN_QUIC_MAX_CID 20                     /* bytes in a version 1 connection ID */
#define PROVEN_QUIC_VARINT_MAX ((((proven_u64)1) << 62) - 1)

/* Transport error codes (RFC 9000 section 20.1). A TLS alert travels as 0x0100 + alert. */
#define PROVEN_QUIC_NO_ERROR                  0x00
#define PROVEN_QUIC_INTERNAL_ERROR            0x01
#define PROVEN_QUIC_PROTOCOL_VIOLATION        0x0a
#define PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR 0x08
#define PROVEN_QUIC_CRYPTO_BUFFER_EXCEEDED    0x0d
#define PROVEN_QUIC_CRYPTO_ERROR              0x0100

/* The three encryption levels this library uses. 0-RTT (RFC-0021) has a key of its own in the
 * Application level's packet number space, held by quic_conn.c. */
typedef enum { PROVEN_QUIC_LEVEL_INITIAL = 0, PROVEN_QUIC_LEVEL_HANDSHAKE = 1, PROVEN_QUIC_LEVEL_APPLICATION = 2 } proven_quic_level_t;
#define PROVEN_QUIC_LEVELS 3

/* ---- Variable-length integers (RFC 9000 section 16) ---- */

/* 1, 2, 4 or 8: the bytes `v` takes; 0 above PROVEN_QUIC_VARINT_MAX. */
proven_size_t proven_quic_varint_len(proven_u64 v);
/* Write `v` in its shortest form; returns the bytes written, 0 above the maximum. */
proven_size_t proven_quic_varint_put(proven_u64 v, proven_byte_t out[8]);

/* A reader over received bytes: every get checks what is left, and the first short read
 * clears `ok` for good. */
typedef struct { const proven_byte_t *p; proven_size_t n; bool ok; } proven_quic_rd_t;
proven_u64 proven_quic_rd_varint(proven_quic_rd_t *r);
proven_u64 proven_quic_rd_uint(proven_quic_rd_t *r, proven_size_t bytes);
/* The next `n` bytes as a reader of their own. */
proven_quic_rd_t proven_quic_rd_take(proven_quic_rd_t *r, proven_size_t n);

/* ---- Packet numbers (RFC 9000 section 17.1 and appendix A.3) ---- */

/* The full packet number from its `bits` low bits, given the largest one received so far in
 * the same number space (`largest_plus_one` 0: none yet). */
proven_u64 proven_quic_pn_decode(proven_u64 largest_plus_one, proven_u64 truncated, unsigned bits);

/* ---- Packet protection (RFC 9001 section 5) ---- */

typedef struct {
    bool chacha;
    proven_size_t key_len;                        /* 16 or 32 */
    proven_crypto_aes_gcm_t aead;                 /* AES: the context. ChaCha20: its key, in the first 32 bytes of aead.rk */
    proven_crypto_aes_t hp;                       /* the same for the header protection key: AES with no GHASH key */
    proven_byte_t iv[12];
} proven_quic_keys_t;

/* The Initial secrets from the client's first Destination Connection ID (RFC 9001 section
 * 5.2): 32 bytes each, for TLS_AES_128_GCM_SHA256. */
void proven_quic_initial_secrets(proven_mem_view_t dcid, proven_byte_t client[32], proven_byte_t server[32]);
/* The same for `version` (1 or 2: RFC 9369's salt). */
void proven_quic_initial_secrets_v(proven_mem_view_t dcid, proven_u32 version, proven_byte_t client[32], proven_byte_t server[32]);
/* The suite of the Initial packets. */
const proven_tls_suite_t *proven_quic_initial_suite(void);
/* Key, IV and header protection key from a secret of `suite` ("quic key", "quic iv", "quic hp"). */
void proven_quic_keys_set(proven_quic_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *secret);
/* The secret of the next key phase (RFC 9001 section 6.1, "quic ku"), in place. */
void proven_quic_secret_update(const proven_tls_suite_t *suite, proven_byte_t *secret);
/* The same for `version`: version 2 labels them "quicv2 key", "quicv2 iv", "quicv2 hp", "quicv2 ku". */
void proven_quic_keys_set_v(proven_quic_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *secret, proven_u32 version);
void proven_quic_secret_update_v(const proven_tls_suite_t *suite, proven_byte_t *secret, proven_u32 version);
/* Five bytes of header protection mask from a 16-byte sample. */
void proven_quic_hp_mask(const proven_quic_keys_t *keys, const proven_byte_t sample[16], proven_byte_t mask[5]);

/* Protect a packet in place. `packet` holds the header - its packet number field of `pn_len`
 * bytes last, at `header_len - pn_len`, and a Length field already counting the tag - then
 * `payload_len` bytes of plaintext, then room for 16 bytes of tag. The payload is sealed with
 * the header as associated data and the header protected. Returns the packet's length, or 0
 * when pn_len + payload_len < 4 (too short to sample: the caller pads). */
proven_size_t proven_quic_protect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t header_len, proven_size_t pn_len,
                                  proven_u64 pn, proven_size_t payload_len);

/* Remove the protection of one packet in place. `pn_offset` is where its packet number field
 * starts (after the Length field of a long header, after the connection ID of a short one),
 * `packet_len` the packet's whole length, `largest_plus_one` as for proven_quic_pn_decode. On
 * success the first byte and the packet number are unprotected, `*pn` is the full number,
 * `*header_len` where the payload starts, and `*payload` views the plaintext. False when the
 * packet is too short or does not decrypt; its bytes are then not to be used. */
[[nodiscard]] bool proven_quic_unprotect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t pn_offset, proven_size_t packet_len,
                                         proven_u64 largest_plus_one, proven_u64 *pn, proven_size_t *header_len, proven_mem_mut_t *payload);
/* The same with the header protection mask already made from this packet's sample under these
 * keys' header protection key - which a key update does not change. */
[[nodiscard]] bool proven_quic_unprotect_masked(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t pn_offset, proven_size_t packet_len,
                                                proven_u64 largest_plus_one, const proven_byte_t mask[5], proven_u64 *pn, proven_size_t *header_len, proven_mem_mut_t *payload);

/* The Retry integrity tag (RFC 9001 section 5.8): over the Original Destination Connection ID
 * and the Retry packet without its tag; the key and nonce of the version the packet's own
 * Version field names (RFC 9369 section 3.3.3 for version 2). */
void proven_quic_retry_tag(proven_mem_view_t odcid, proven_mem_view_t retry, proven_byte_t tag[16]);
/* True when a whole Retry packet's last 16 bytes are its tag. */
[[nodiscard]] bool proven_quic_retry_check(proven_mem_view_t odcid, proven_mem_view_t retry_packet);

/* ---- Transport parameters (RFC 9000 section 18) ---- */

typedef struct {
    proven_byte_t len;
    proven_byte_t id[PROVEN_QUIC_MAX_CID];
} proven_quic_cid_t;

typedef struct {
    proven_byte_t ipv4[4];
    proven_u16 ipv4_port;
    proven_byte_t ipv6[16];
    proven_u16 ipv6_port;
    proven_quic_cid_t cid;
    proven_byte_t reset_token[16];
} proven_quic_preferred_address_t;

typedef struct {
    proven_u64 max_idle_timeout;                  /* milliseconds; 0: none */
    proven_u64 max_udp_payload_size;              /* 65527 by default */
    proven_u64 initial_max_data;
    proven_u64 initial_max_stream_data_bidi_local;
    proven_u64 initial_max_stream_data_bidi_remote;
    proven_u64 initial_max_stream_data_uni;
    proven_u64 initial_max_streams_bidi;
    proven_u64 initial_max_streams_uni;
    proven_u64 ack_delay_exponent;                /* 3 by default */
    proven_u64 max_ack_delay;                     /* milliseconds, 25 by default */
    proven_u64 active_connection_id_limit;        /* 2 by default */
    bool disable_active_migration;
    bool has_original_dcid, has_initial_scid, has_retry_scid, has_reset_token, has_preferred_address;
    proven_quic_cid_t original_dcid;              /* server only */
    proven_quic_cid_t initial_scid;
    proven_quic_cid_t retry_scid;                 /* server only */
    proven_byte_t reset_token[16];                /* server only */
    proven_quic_preferred_address_t preferred_address;  /* server only; never sent by this library */
    /* version_information (RFC 9368 section 3, id 0x11): the chosen version and the available
     * ones (a client's in descending preference; a server's, its fully deployed versions). */
    bool has_version_info;
    proven_u32 chosen_version;
    proven_u8 available_n;
#define PROVEN_QUIC_TP_VERSIONS 8
    proven_u32 available[PROVEN_QUIC_TP_VERSIONS];
} proven_quic_tp_t;

/* Every field at the default RFC 9000 gives it when absent; nothing present. */
void proven_quic_tp_defaults(proven_quic_tp_t *tp);
/* Encode the parameters a `server` (or client) sends: each one that differs from its default,
 * and the connection IDs and token that are present. Returns the bytes written, or 0 when
 * `out` is too small or a value is out of its range. Never sends preferred_address. */
proven_size_t proven_quic_tp_encode(const proven_quic_tp_t *tp, bool server, proven_mem_mut_t out);
/* Decode what the peer sent; `from_server` says which side that is. Returns 0, or
 * PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR for a malformed list, a value out of range, a
 * parameter repeated, one a client may not send, or one that must be there and is not
 * (initial_source_connection_id from either side, original_destination_connection_id from a
 * server), and a version_information that RFC 9368 section 4 calls a parsing failure (not a
 * whole number of versions, a zero version, more than PROVEN_QUIC_TP_VERSIONS, or from a client
 * a chosen version missing from its list). Parameters this library does not know are skipped. */
proven_u64 proven_quic_tp_decode(proven_mem_view_t in, bool from_server, proven_quic_tp_t *tp);

/* ---- TLS 1.3 in QUIC mode (RFC 9001 sections 4 and 8): tls13.c ----
 *
 * The connection is an ordinary proven_tls_conn_t; proven_tls_conn_destroy frees it, and
 * proven_tls_is_established, _alpn, _resumed, _peer_key_sha256 and the like answer as over
 * TCP. What differs: handshake bytes come in and go out per encryption level with no records
 * around them; each level's traffic secrets are handed out instead of being used here; the
 * transport parameters travel in their extension; TLS 1.3 only, no session id, no
 * ChangeCipherSpec, no KeyUpdate, a protocol must be agreed. proven_tls_feed, _pending_output,
 * _read, _write and _close refuse a QUIC-mode connection with PROVEN_ERR_INVALID_STATE, and
 * proven_tls_key_update with PROVEN_ERR_UNSUPPORTED. */

/* A client or server in QUIC mode. `transport_params` is what this side sends, encoded
 * (proven_quic_tp_encode), at most 1,024 bytes. PROVEN_ERR_INVALID_ARG when the configuration
 * cannot speak TLS 1.3 or names no protocol (ALPN), besides what proven_tls_client_create and
 * proven_tls_server_create refuse. The client's first flight is pending at once. */
proven_err_t proven_tls_quic_client_create_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                            proven_mem_view_t transport_params, proven_tls_conn_t **out);
proven_err_t proven_tls_quic_server_create_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_tls_conn_t **out);
/* The same, held to one protocol of the configuration's list (`alpn`, empty: any): a client
 * offers only it and takes only it; a server agrees only it, and ends the handshake with
 * no_application_protocol when the client does not offer it. PROVEN_ERR_INVALID_ARG when the
 * configuration does not list it. HTTP/3 holds its connections to "h3" (RFC 9114 section 3.1). */
proven_err_t proven_tls_quic_client_create_for_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                                proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_tls_conn_t **out);
proven_err_t proven_tls_quic_server_create_for_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_tls_conn_t **out);
/* The same in QUIC `version` (RFC-0036): a session ticket made in the other version is not
 * offered by a client nor taken by a server (RFC 9369 section 5). A server given `upgrade_to`
 * (0: none) moves to that version when the client's version_information lists it, and says so
 * in its own transport parameters' version_information (compatible negotiation, RFC 9368 section
 * 2.3); proven_tls_quic_version_ then answers it. */
proven_err_t proven_tls_quic_client_create_v_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                              proven_mem_view_t transport_params, proven_u8str_view_t alpn, proven_u32 version, proven_tls_conn_t **out);
proven_err_t proven_tls_quic_server_create_v_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_u8str_view_t alpn,
                                              proven_u32 version, proven_u32 upgrade_to, proven_tls_conn_t **out);
proven_u32 proven_tls_quic_version_(const proven_tls_conn_t *c);
/* A client that has learned the negotiated version: the tickets that come are of it. */
void proven_tls_quic_set_version_(proven_tls_conn_t *c, proven_u32 version);

/* Handshake bytes the peer sent at `level`, in order and each once (the QUIC layer puts CRYPTO
 * frames in order). Bytes at a level other than the one being read end the connection with
 * 0x010a. On a failure the connection is dead and proven_tls_quic_error_ says why. */
proven_err_t proven_tls_quic_feed_(proven_tls_conn_t *c, proven_quic_level_t level, proven_mem_view_t in);
/* Handshake bytes waiting to go out at `level`, and how many of them the QUIC layer has taken. */
proven_mem_view_t proven_tls_quic_pending_(const proven_tls_conn_t *c, proven_quic_level_t level);
void proven_tls_quic_sent_(proven_tls_conn_t *c, proven_quic_level_t level, proven_size_t n);
/* The level being read and the level being written: each moves from Initial to Handshake to
 * Application as the handshake does, and the secret of a level is ready when it is reached. */
proven_quic_level_t proven_tls_quic_read_level_(const proven_tls_conn_t *c);
proven_quic_level_t proven_tls_quic_write_level_(const proven_tls_conn_t *c);
/* The traffic secret of `level` (Handshake or Application) for reading or for writing: its
 * length, the suite's hash length, or 0 while there is none. SECRET: out. */
proven_size_t proven_tls_quic_secret_(const proven_tls_conn_t *c, proven_quic_level_t level, bool write, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]);
/* Wipe a level's secrets once the QUIC layer has its keys (and has discarded the level's). */
void proven_tls_quic_secret_drop_(proven_tls_conn_t *c, proven_quic_level_t level);
/* The suite agreed (NULL before the ServerHello), and the peer's transport parameters (NULL
 * until they have arrived and decoded). */
const proven_tls_suite_t *proven_tls_quic_suite_(const proven_tls_conn_t *c);
const proven_quic_tp_t *proven_tls_quic_peer_params_(const proven_tls_conn_t *c);
/* Why the connection failed, as a QUIC error code for CONNECTION_CLOSE: 0x0100 + the TLS alert
 * (RFC 9001 section 4.8), or a transport error such as TRANSPORT_PARAMETER_ERROR. 0: none. */
proven_u64 proven_tls_quic_error_(const proven_tls_conn_t *c);

/* 0-RTT (RFC-0021). Early data is offered by a client whose configuration has `early_data` and
 * whose session allows it, and accepted by a server only as RFC-0021 section 10.1 P-5 lists. */
int proven_tls_quic_early_(const proven_tls_conn_t *c);
/* The early traffic secret: a client's for writing from its ClientHello on, a server's for
 * reading once it accepted. Its length and the suite it belongs to (the ticket's), or 0 when
 * there is none. SECRET: out. */
proven_size_t proven_tls_quic_early_secret_(const proven_tls_conn_t *c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE], const proven_tls_suite_t **suite);
void proven_tls_quic_early_drop_(proven_tls_conn_t *c);
/* A client offering early data: the server's limits its session remembers (RFC 9000 section
 * 7.4.1), in proven_quic_tp_t's order: initial_max_data, initial_max_stream_data_bidi_local,
 * _bidi_remote, _uni, initial_max_streams_bidi, _uni, active_connection_id_limit. */
bool proven_tls_quic_early_limits_(const proven_tls_conn_t *c, proven_u64 out[7]);
/* A session's NEW_TOKEN token (RFC 9000 section 8.1.3): taken out once (and cleared), and kept
 * when the server sends one. A token longer than 255 bytes is not kept. */
proven_size_t proven_tls_session_token_take_(proven_tls_session_t *s, proven_byte_t *out, proven_size_t cap);
void proven_tls_session_token_keep_(proven_tls_session_t *s, proven_mem_view_t token);
/* The same for QUIC `version` (RFC-0036): a token is taken only by a connection of the version it
 * was kept for; and whether the session's ticket came over version 2 (a client starts there). */
proven_size_t proven_tls_session_token_take_v_(proven_tls_session_t *s, proven_u32 version, proven_byte_t *out, proven_size_t cap);
void proven_tls_session_token_keep_v_(proven_tls_session_t *s, proven_u32 version, proven_mem_view_t token);
bool proven_tls_session_quic_v2_(const proven_tls_session_t *s);
/* A session whose next connection must not send early data: the ticket stays, its allowance goes. */

/* ---- Packets and frames (RFC 9000 sections 12, 17 and 19): quic_frame.c ----
 *
 * Headers are read from a stranger's datagram: every length is checked against what is left,
 * and a header that does not parse ends the reading of that datagram. Frames are read from a
 * packet that has opened, so from the peer: a frame that breaks the encoding ends the
 * connection with the code returned. */

#define PROVEN_QUIC_CONNECTION_REFUSED        0x02
#define PROVEN_QUIC_FLOW_CONTROL_ERROR        0x03
#define PROVEN_QUIC_STREAM_LIMIT_ERROR        0x04
#define PROVEN_QUIC_STREAM_STATE_ERROR        0x05
#define PROVEN_QUIC_FINAL_SIZE_ERROR          0x06
#define PROVEN_QUIC_FRAME_ENCODING_ERROR      0x07
#define PROVEN_QUIC_CONNECTION_ID_LIMIT_ERROR 0x09
#define PROVEN_QUIC_INVALID_TOKEN             0x0b
#define PROVEN_QUIC_APPLICATION_ERROR         0x0c
#define PROVEN_QUIC_KEY_UPDATE_ERROR          0x0e
#define PROVEN_QUIC_VERSION_NEGOTIATION_ERROR 0x11            /* RFC 9368 */
#define PROVEN_QUIC_AEAD_LIMIT_REACHED        0x0f
#define PROVEN_QUIC_NO_VIABLE_PATH            0x10

#define PROVEN_QUIC_MIN_INITIAL 1200               /* bytes in a datagram carrying a client's Initial */
#define PROVEN_QUIC_RESET_TOKEN 16

typedef enum {
    PROVEN_QUIC_PKT_INITIAL, PROVEN_QUIC_PKT_0RTT, PROVEN_QUIC_PKT_HANDSHAKE, PROVEN_QUIC_PKT_RETRY,
    PROVEN_QUIC_PKT_SHORT,
    PROVEN_QUIC_PKT_VN,                            /* a version negotiation packet (version 0) */
    PROVEN_QUIC_PKT_OTHER_VERSION                  /* a long header of a version this library does not speak */
} proven_quic_pkt_type_t;

typedef struct {
    proven_quic_pkt_type_t type;
    proven_u32 version;
    proven_quic_cid_t dcid, scid;                  /* scid: long headers only */
    proven_mem_view_t token;                       /* Initial: its token (maybe empty). Retry: the Retry token. VN: the versions */
    proven_size_t pn_offset;                       /* Initial, 0-RTT, Handshake, short: where the protected packet number starts */
    proven_size_t len;                             /* the packet's bytes in the datagram (a short header, VN, Retry and another version: the rest of it) */
} proven_quic_hdr_t;

/* The header of the packet at the start of `d`. A short header does not say how long its
 * connection ID is: `short_dcid_len` is this side's. False when the header is malformed or
 * runs past the datagram, the fixed bit is clear in a version 1 packet, or a connection ID is
 * longer than 20 bytes in version 1: the rest of the datagram is then dropped. */
[[nodiscard]] bool proven_quic_hdr_parse(proven_mem_view_t d, proven_size_t short_dcid_len, proven_quic_hdr_t *h);

/* Version negotiation (RFC 9000 section 17.2.1): an answer to a client's long header of
 * another version, listing versions 1 and 2. `dcid` and `scid` are the client's scid and dcid,
 * as the answer swaps them. Returns its length, 0 when `out` is too small. */
proven_size_t proven_quic_vn_build(const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid, proven_byte_t random_bits, proven_mem_mut_t out);
/* A Retry packet (RFC 9000 section 17.2.5) with its integrity tag over `odcid`. */
proven_size_t proven_quic_retry_build(const proven_quic_cid_t *odcid, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                      proven_mem_view_t token, proven_byte_t random_bits, proven_mem_mut_t out);
/* The same in `version` (1 or 2). */
proven_size_t proven_quic_retry_build_v(proven_u32 version, const proven_quic_cid_t *odcid, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                        proven_mem_view_t token, proven_byte_t random_bits, proven_mem_mut_t out);

/* A long header of `type` up to and including its packet number field of `pn_len` bytes,
 * with a two-byte Length field counting `pn_len + payload_len + 16` - what
 * proven_quic_protect takes. Returns the header's length, 0 when it does not fit or the
 * Length would need more than two bytes. */
proven_size_t proven_quic_long_header_put(proven_quic_pkt_type_t type, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                          proven_mem_view_t token, proven_u64 pn, proven_size_t pn_len, proven_size_t payload_len,
                                          proven_mem_mut_t out);
/* The same in `version` (1 or 2: RFC 9369's type bits). */
proven_size_t proven_quic_long_header_put_v(proven_u32 version, proven_quic_pkt_type_t type, const proven_quic_cid_t *dcid, const proven_quic_cid_t *scid,
                                            proven_mem_view_t token, proven_u64 pn, proven_size_t pn_len, proven_size_t payload_len,
                                            proven_mem_mut_t out);
/* A short header: the key phase bit, the connection ID, the packet number field. */
proven_size_t proven_quic_short_header_put(const proven_quic_cid_t *dcid, bool key_phase, proven_u64 pn, proven_size_t pn_len, proven_mem_mut_t out);
/* The packet number field's length for `pn` when the peer has acknowledged up to `largest_acked`
 * (`largest_acked_plus_one` 0: nothing yet), RFC 9000 section 17.1: 1 to 4. */
proven_size_t proven_quic_pn_len(proven_u64 pn, proven_u64 largest_acked_plus_one);

/* A writer: every put checks the room left, and the first that does not fit clears `ok`. */
typedef struct { proven_byte_t *p; proven_size_t n, cap; bool ok; } proven_quic_wr_t;
void proven_quic_wr_varint(proven_quic_wr_t *w, proven_u64 v);
void proven_quic_wr_uint(proven_quic_wr_t *w, proven_u64 v, proven_size_t bytes);
void proven_quic_wr_bytes(proven_quic_wr_t *w, const proven_byte_t *p, proven_size_t n);

enum {
    PROVEN_QUIC_FT_PADDING = 0x00, PROVEN_QUIC_FT_PING = 0x01, PROVEN_QUIC_FT_ACK = 0x02, PROVEN_QUIC_FT_ACK_ECN = 0x03,
    PROVEN_QUIC_FT_RESET_STREAM = 0x04, PROVEN_QUIC_FT_STOP_SENDING = 0x05, PROVEN_QUIC_FT_CRYPTO = 0x06,
    PROVEN_QUIC_FT_NEW_TOKEN = 0x07, PROVEN_QUIC_FT_STREAM = 0x08, /* 0x08 to 0x0f */
    PROVEN_QUIC_FT_MAX_DATA = 0x10, PROVEN_QUIC_FT_MAX_STREAM_DATA = 0x11, PROVEN_QUIC_FT_MAX_STREAMS_BIDI = 0x12,
    PROVEN_QUIC_FT_MAX_STREAMS_UNI = 0x13, PROVEN_QUIC_FT_DATA_BLOCKED = 0x14, PROVEN_QUIC_FT_STREAM_DATA_BLOCKED = 0x15,
    PROVEN_QUIC_FT_STREAMS_BLOCKED_BIDI = 0x16, PROVEN_QUIC_FT_STREAMS_BLOCKED_UNI = 0x17,
    PROVEN_QUIC_FT_NEW_CONNECTION_ID = 0x18, PROVEN_QUIC_FT_RETIRE_CONNECTION_ID = 0x19,
    PROVEN_QUIC_FT_PATH_CHALLENGE = 0x1a, PROVEN_QUIC_FT_PATH_RESPONSE = 0x1b,
    PROVEN_QUIC_FT_CONNECTION_CLOSE = 0x1c, PROVEN_QUIC_FT_CONNECTION_CLOSE_APP = 0x1d, PROVEN_QUIC_FT_HANDSHAKE_DONE = 0x1e
};

/* One frame, read or to be written. Which fields mean something depends on `type`. */
typedef struct {
    proven_u64 type;                               /* STREAM: 0x08, its three bits in `fin` and in whether `offset` and the length are written */
    proven_u64 id;                                 /* a stream ID; NEW_CONNECTION_ID's and RETIRE_CONNECTION_ID's sequence number */
    proven_u64 offset;                             /* STREAM, CRYPTO */
    proven_u64 value;                              /* MAX_*, *_BLOCKED: the limit; RESET_STREAM, STOP_SENDING, CONNECTION_CLOSE: the error code; NEW_CONNECTION_ID: retire_prior_to */
    proven_u64 final_size;                         /* RESET_STREAM */
    proven_u64 frame_type;                         /* CONNECTION_CLOSE (0x1c): the frame type that caused it */
    bool fin;                                      /* STREAM */
    proven_mem_view_t data;                        /* STREAM, CRYPTO, NEW_TOKEN; CONNECTION_CLOSE's reason; PATH_CHALLENGE's and PATH_RESPONSE's 8 bytes */
    proven_quic_cid_t cid;                         /* NEW_CONNECTION_ID */
    proven_byte_t reset_token[PROVEN_QUIC_RESET_TOKEN];
    proven_u64 largest, ack_delay, first_range, range_count;   /* ACK */
    proven_mem_view_t ranges;                      /* ACK: `range_count` (gap, length) pairs, already checked */
    proven_u64 ecn[3];                             /* ACK_ECN: ECT(0), ECT(1), ECN-CE */
} proven_quic_frame_t;

/* Read one frame. Returns 0, or the code to close with: FRAME_ENCODING_ERROR for a frame
 * that is cut short or malformed - an unknown type, an ACK range below zero, a STREAM or
 * CRYPTO frame past 2^62 - 1, a connection ID of 0 or more than 20 bytes, retire_prior_to
 * above the sequence number, a stream count above 2^60, an empty NEW_TOKEN; PROTOCOL_VIOLATION
 * for a frame type not in its shortest encoding. */
proven_u64 proven_quic_frame_read(proven_quic_rd_t *r, proven_quic_frame_t *f);
/* Write a frame other than ACK. STREAM always carries its length; its offset when not 0.
 * False when it does not fit (nothing usable is written then). */
bool proven_quic_frame_put(proven_quic_wr_t *w, const proven_quic_frame_t *f);
/* The bytes a STREAM frame's header takes (type, id, offset when not 0, length). */
proven_size_t proven_quic_stream_overhead(proven_u64 id, proven_u64 offset, proven_size_t len);

/* Walk an ACK frame's ranges from the highest: each call gives the next range [*lo, *hi]. */
typedef struct { proven_quic_rd_t r; proven_u64 hi, lo, left; bool first; } proven_quic_ack_iter_t;
proven_quic_ack_iter_t proven_quic_ack_iter(const proven_quic_frame_t *f);
bool proven_quic_ack_next(proven_quic_ack_iter_t *it, proven_u64 *lo, proven_u64 *hi);
/* Write an ACK frame for `count` ranges [lo, hi], highest first and not touching, as many as
 * fit (at least the first); `ack_delay` already scaled down by the exponent. Returns the
 * ranges written, 0 when not even the first fits. */
proven_size_t proven_quic_ack_put(proven_quic_wr_t *w, const proven_u64 (*ranges)[2], proven_size_t count, proven_u64 ack_delay);
/* The same as ACK_ECN (0x03) with the counts ECT(0), ECT(1), ECN-CE after the ranges (RFC 9000
 * section 19.3.2); `ecn` NULL writes a plain ACK. */
proven_size_t proven_quic_ack_put_ecn(proven_quic_wr_t *w, const proven_u64 (*ranges)[2], proven_size_t count, proven_u64 ack_delay, const proven_u64 ecn[3]);

/* What a frame type is: allowed in Initial and Handshake packets (PADDING, PING, ACK, CRYPTO,
 * CONNECTION_CLOSE 0x1c); ack-eliciting (all but ACK, PADDING, CONNECTION_CLOSE); probing
 * (PATH_CHALLENGE, PATH_RESPONSE, NEW_CONNECTION_ID, PADDING). */
bool proven_quic_frame_in_handshake(proven_u64 type);
bool proven_quic_frame_ack_eliciting(proven_u64 type);
bool proven_quic_frame_probing(proven_u64 type);

/* ---- The connection (RFC 9000, RFC 9002): quic_conn.c ----
 *
 * Like proven_h2_conn_t: no socket and no clock of its own. Datagrams and the time come in,
 * datagrams and a deadline go out; streams are read and written through it. A connection is
 * for one thread at a time. Time is microseconds on any monotonic clock, the same one for
 * every call. Addresses are opaque bytes the caller chooses (a proven_net_addr_t's, say),
 * compared for equality only: the engine never sends anywhere by itself. */

typedef struct {
    proven_byte_t len;
    proven_byte_t b[32];
    proven_byte_t port_at, port_len;               /* where in `b` the port is (len 0: not said): a NEW_TOKEN token is bound to the rest, the client's IP (RFC-0021 P-7) */
} proven_quic_addr_t;

typedef struct {
    proven_allocator_t alloc;                      /* required */
    const proven_tls_config_t *tls;                /* required: names a protocol (ALPN); its random source is this connection's */
    proven_u8str_view_t alpn;                      /* the one protocol of `tls`'s list the connection may agree (HTTP/3: "h3"); empty: any of them */
    proven_u32 max_streams_bidi;                   /* streams the peer may open, each kind. 0: 100 */
    proven_u32 streams_bidi_now;                   /* of those, bidirectional ones it may open at first: 1 to max_streams_bidi, which stays the ceiling for proven_quic_conn_set_max_streams_. 0: max_streams_bidi */
    proven_u32 max_streams_uni;                    /* 0: 100 */
    proven_u32 stream_window;                      /* bytes a stream may receive ahead of the reader. 0: 256 KiB */
    proven_u32 conn_window;                        /* the same for all streams together. 0: 1 MiB */
    proven_u32 stream_send_buffer;                 /* bytes written and not yet acknowledged, per stream. 0: 256 KiB */
    proven_u32 conn_send_buffer;                   /* the same for all streams together. 0: 1 MiB */
    proven_u32 stream_window_max;                  /* RFC-0019 section 9: the stream window doubles, up to this, while the reader */
    proven_u32 conn_window_max;                    /*   empties it within two round trips; and these cap the send buffers, which follow */
                                                   /*   what the peer has granted. 0: no growth (stream_window, conn_window fixed) */
#define PROVEN_QUIC_STREAM_WINDOW_MAX (16u * 1024 * 1024)   /* the event drivers' caps when no window is configured */
#define PROVEN_QUIC_CONN_WINDOW_MAX (24u * 1024 * 1024)
    proven_u32 idle_timeout_ms;                    /* 0: 30,000 */
    proven_u32 datagram_size;                      /* the datagrams this side sends: 1,200 to 1,452. 0: 1,200 */
    proven_u32 granularity_us;                     /* RFC 9002's timer granularity: the clock's tick. 0: 1,000 */
    proven_u32 max_ack_ranges;                     /* received packet ranges kept per space. 0: 64 */
    proven_u32 key_update_packets;                 /* 1-RTT packets sent under one key before this side updates it (RFC 9001 section 6.6: AES-GCM's limit is 2^23). 0: 2^22 */
    proven_u32 pmtu_raise_ms;                      /* after a search that stopped below the largest size, how long before a larger one is tried again (RFC 8899's PMTU_RAISE_TIMER). 0: 600,000 */
    proven_byte_t static_key[32];                  /* a server: one key for all its connections (Retry tokens, stateless reset tokens). All zero: drawn per connection */
    bool newreno;                                  /* RFC 9002's NewReno instead of CUBIC (RFC 9438; RFC-0033): for tests and measurement only */
    bool no_ecn;                                   /* nothing marked and no counts reported (RFC-0035): for tests and measurement only */
    /* QUIC versions (RFC-0036). A client starts in `version` (0: the version its session's ticket
     * came over, else 1), lists it then the other in its version_information, and follows a
     * server that moves it (compatible negotiation). `after_vn`: a client starting again after a
     * Version Negotiation packet, which then holds the server's version_information to RFC 9368
     * section 4. `prefer_v2`: a client lists 2 first (tests and the private checks). A server
     * answers in the client's version; `upgrade_v2` moves a client that lists 2 there (tests only). */
    proven_u32 version;
    bool after_vn, prefer_v2, upgrade_v2;
    /* A server that is one of `cid_loops` loops on one port (RFC-0037; 0 or 1: alone): every
     * connection ID a connection issues - its first, NEW_CONNECTION_ID - has its first byte
     * equal to `cid_loop` modulo `cid_loops`, which is what the port's program routes by. (Not a
     * Retry's: the Initial that follows may reach any loop, and the group's key opens its token.) */
    proven_u8 cid_loops, cid_loop;
} proven_quic_config_t;

/* The ECN field of a datagram (RFC 3168), as the IP header carries it. */
#define PROVEN_QUIC_ECN_NOT_ECT 0
#define PROVEN_QUIC_ECN_ECT1 1
#define PROVEN_QUIC_ECN_ECT0 2
#define PROVEN_QUIC_ECN_CE 3

typedef struct proven_quic_conn proven_quic_conn_t;

typedef enum {
    PROVEN_QUIC_EV_NONE = 0,
    PROVEN_QUIC_EV_ESTABLISHED,                    /* the handshake is complete: streams may be used */
    PROVEN_QUIC_EV_STREAM_OPENED,                  /* the peer opened `stream_id` */
    PROVEN_QUIC_EV_STREAM_READABLE,                /* `stream_id` has bytes to read, or its end */
    PROVEN_QUIC_EV_STREAM_WRITABLE,                /* `stream_id` can take bytes again */
    PROVEN_QUIC_EV_STREAM_RESET,                   /* the peer reset its side of `stream_id` with `code` */
    PROVEN_QUIC_EV_STOP_SENDING,                   /* the peer wants no more on `stream_id` (`code`); this side has reset it */
    PROVEN_QUIC_EV_CID_NEW,                        /* `cid` reaches this connection (for a demultiplexer) */
    PROVEN_QUIC_EV_CID_RETIRED,                    /* `cid` no longer does */
    PROVEN_QUIC_EV_CLOSED,                         /* over: `code` (`app`: an application's code), `remote` when the peer closed; nothing more is sent */
    PROVEN_QUIC_EV_EARLY                           /* before ESTABLISHED, 0-RTT (RFC-0021): a client may use streams now (early data); a server accepted the client's and may answer */
} proven_quic_event_kind_t;

typedef struct {
    proven_quic_event_kind_t kind;
    proven_u64 stream_id;
    proven_u64 code;
    bool app, remote;
    proven_quic_cid_t cid;
} proven_quic_event_t;

/* What a server makes of a datagram before any connection exists. */
typedef enum {
    PROVEN_QUIC_STATELESS_DROP = 0,                /* nothing to do */
    PROVEN_QUIC_STATELESS_REPLY,                   /* send `out` back (version negotiation or a Retry) */
    PROVEN_QUIC_STATELESS_ACCEPT                   /* a client's first Initial: proven_quic_conn_server_create */
} proven_quic_stateless_t;

typedef struct {
    proven_quic_cid_t odcid;                       /* the client's first Destination Connection ID */
    proven_quic_cid_t retry_scid;                  /* when `retried`: the Source Connection ID of the Retry */
    bool retried;                                  /* the token was a valid Retry token: the address is validated */
    bool validated;                                /* the token was a valid NEW_TOKEN token (RFC 9000 section 8.1.3): the address is validated, no Retry */
    proven_u32 version;                            /* the client's Initial's version (RFC-0036) */
} proven_quic_accept_t;

/* A server's look at a datagram that matched no connection. Version negotiation for a long
 * header of a version other than 1 and 2 (in a datagram of at least 1,200 bytes); a client's Initial in a
 * datagram of at least 1,200 bytes is to be accepted - validated when it carries a NEW_TOKEN
 * token of this server's for its IP address, at most a day old. With `require_retry`, a Retry
 * for an Initial without a valid token of either kind, and a Retry token checked (address, age
 * at most ten seconds): one that fails is dropped. Everything else is dropped. */
proven_quic_stateless_t proven_quic_server_stateless(const proven_quic_config_t *cfg, proven_mem_view_t datagram, const proven_quic_addr_t *from,
                                                      proven_u64 now, bool require_retry, proven_mem_mut_t out, proven_size_t *out_len,
                                                      proven_quic_accept_t *accept);
/* A stateless reset (RFC 9000 section 10.3) for a short-header datagram of `in_len` bytes to a
 * connection ID this server issued and no longer knows: shorter than what came, at least 21
 * bytes. 0 when `in_len` is too small to answer without the answer being as large. */
proven_size_t proven_quic_stateless_reset(const proven_quic_config_t *cfg, const proven_quic_cid_t *dcid, proven_size_t in_len, proven_mem_mut_t out);

proven_err_t proven_quic_conn_client_create(const proven_quic_config_t *cfg, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                           const proven_quic_addr_t *server, proven_u64 now, proven_quic_conn_t **out);
/* A server's connection for an ACCEPTed datagram, which it then reads. */
proven_err_t proven_quic_conn_server_create(const proven_quic_config_t *cfg, const proven_quic_accept_t *accept, proven_mem_view_t datagram,
                                           const proven_quic_addr_t *from, proven_u8 ecn, proven_u64 now, proven_quic_conn_t **out);
void proven_quic_conn_destroy(proven_quic_conn_t *c);

/* A datagram from `from`, with the ECN field it arrived with (PROVEN_QUIC_ECN_*; NOT_ECT when the
 * system does not say). A packet that does not open or breaks no rule worth closing over is
 * dropped; a rule broken closes the connection (an event). PROVEN_ERR_NOMEM only. */
proven_err_t proven_quic_conn_receive(proven_quic_conn_t *c, proven_mem_view_t datagram, const proven_quic_addr_t *from, proven_u8 ecn, proven_u64 now);
/* The next datagram to send now, into `out` (at least 1,452 bytes), where, and with which ECN
 * mark (`ecn` may be NULL: the caller cannot mark): `*len` 0 when nothing may go now (call again
 * after the deadline or after receiving). */
proven_err_t proven_quic_conn_send(proven_quic_conn_t *c, proven_u64 now, proven_mem_mut_t out, proven_size_t *len, proven_quic_addr_t *to, proven_u8 *ecn);
/* When to call proven_quic_conn_timeout next; UINT64 max when nothing is waiting. */
proven_u64 proven_quic_conn_deadline(const proven_quic_conn_t *c);
void proven_quic_conn_timeout(proven_quic_conn_t *c, proven_u64 now);
/* The next event; false when there is none. */
bool proven_quic_conn_poll(proven_quic_conn_t *c, proven_quic_event_t *ev);

/* Streams. PROVEN_ERR_INVALID_STATE before the handshake completes (unless early data is open:
 * PROVEN_QUIC_EV_EARLY), after the close, or for a stream in the wrong state; PROVEN_ERR_NOT_FOUND for a stream that is not open. */
proven_err_t proven_quic_stream_open(proven_quic_conn_t *c, bool bidi, proven_u64 *id);   /* PROVEN_ERR_BUSY: the peer's limit */
proven_err_t proven_quic_stream_write(proven_quic_conn_t *c, proven_u64 id, proven_mem_view_t data, bool fin, proven_size_t *taken);   /* may take less: STREAM_WRITABLE follows when there is room */
proven_u64 proven_quic_stream_room(proven_quic_conn_t *c, proven_u64 id);   /* what a write would take now (0 for a stream that cannot be written); STREAM_WRITABLE follows when that grows */
proven_err_t proven_quic_stream_read(proven_quic_conn_t *c, proven_u64 id, proven_mem_mut_t out, proven_size_t *got, bool *fin);
proven_err_t proven_quic_stream_reset(proven_quic_conn_t *c, proven_u64 id, proven_u64 code);
proven_err_t proven_quic_stream_stop(proven_quic_conn_t *c, proven_u64 id, proven_u64 code);
/* Whether stream data or a stream's end is still unacknowledged: a graceful close waits for it. */
bool proven_quic_conn_sending(const proven_quic_conn_t *c);
/* Close: an application's code (`app`) or a transport one. Sending goes on for the close. */
void proven_quic_conn_close(proven_quic_conn_t *c, bool app, proven_u64 code);

/* For tests and the private checks: counters and state. */
typedef struct {
    proven_u64 packets_sent, packets_received, packets_lost, packets_dropped, bytes_sent, bytes_received;
    proven_u64 cwnd, bytes_in_flight, srtt_us, min_rtt_us, pto_count;
    proven_size_t streams, memory;                 /* streams held; bytes of buffers held */
    bool established, confirmed, closed, path_validated;
    proven_u64 path_validations;                   /* new peer addresses a server began to validate */
    proven_u64 key_updates, spurious_losses;
    proven_size_t datagram_size;                   /* what a datagram may take on this path now */
    proven_u64 pmtu_probes, pmtu_probes_lost;      /* PMTU probes sent, and not acknowledged in time */
    proven_u8 early;                               /* 0-RTT (RFC-0021): 0 none, 1 accepted, 2 refused */
    proven_u8 ecn_state;                           /* RFC-0035, this path: 0 testing, 1 unknown, 2 capable, 3 failed */
    proven_u64 ecn_marked;                         /* packets sent marked ECT(0) */
    proven_u64 ecn_received[3];                    /* packets received ECT(0), ECT(1), CE, all spaces */
    proven_u64 ecn_ce_events;                      /* increases of the peer's CE count acted on */
    proven_u32 version;                            /* the version in use (RFC-0036) */
    proven_u32 vn_version;                         /* a client closed by a Version Negotiation packet: the version to start again in; 0: none */
} proven_quic_stats_t;
void proven_quic_conn_stats(const proven_quic_conn_t *c, proven_quic_stats_t *s);
/* Tests only: `payload` read as the frames of a packet that opened at `lvl` (3: a 0-RTT packet;
 * its result: the close code, or 0), and a key update started by this side. */
/* For tests (RFC-0033): the congestion controller alone, on a model of the path - every round
 * trip of `rtt_us` acknowledges the whole window in packets of `mss` bytes, the sender never short
 * of data. Once the window reaches `w_loss` one loss is declared; `*after` is the window just after
 * it, `*recover_us` the time until the window is `w_loss` again (0: not within 600 s of model time). */
void proven_quic_cc_model_(bool newreno, proven_u32 mss, proven_u64 rtt_us, proven_u64 w_loss, proven_u64 *after, proven_u64 *recover_us);
/* For tests: CUBIC's remembered maximum after a loss at `w1` and then one at `w2` (RFC 9438
 * section 4.7, fast convergence: below the last maximum it is taken lower still). */
proven_u64 proven_quic_cc_fast_convergence_(proven_u32 mss, proven_u64 w1, proven_u64 w2);
proven_u64 proven_quic_conn_test_frames_(proven_quic_conn_t *c, int lvl, proven_mem_view_t payload, proven_u64 now);
/* Tests (RFC-0036): the chosen version a client or a server says in its version_information from
 * now on (0: its own) - to see the other side refuse it. */
void proven_quic_test_chosen_(bool server, proven_u32 version);
bool proven_quic_conn_test_key_update_(proven_quic_conn_t *c);
proven_u64 proven_quic_conn_test_tx_max_(const proven_quic_conn_t *c, proven_u64 id);
/* The peer's bidirectional streams open at once from now on: at most the configured number;
 * credit already granted stands (RFC 9000, section 4.6), and a raise is granted at once. For the
 * event server (RFC-0024). */
void proven_quic_conn_set_max_streams_(proven_quic_conn_t *c, proven_u64 n);
/* RFC-0026: a ceiling on credit from now on (MAX_STREAM_DATA, MAX_DATA, growth), 0 for none. */
void proven_quic_conn_cap_window_(proven_quic_conn_t *c, proven_u64 n);
/* Whether the handshake resumed a session the client offered (RFC-0020). */
bool proven_quic_conn_resumed(const proven_quic_conn_t *c);
/* The protocol agreed (ALPN), as proven_tls_alpn: empty before the handshake has agreed one. */
proven_u8str_view_t proven_quic_conn_alpn_(const proven_quic_conn_t *c);
/* Whether the handshake is complete: before it, streams carry early data (RFC-0021). */
bool proven_quic_conn_complete_(const proven_quic_conn_t *c);
/* Client: whether a ticket has come on this connection (for tests). */
bool proven_quic_conn_ticket_stored_(const proven_quic_conn_t *c);

#endif /* PROVEN_NO_HTTP3 */
#endif
