/*
 * lz77_compress.h - Q2 FEATURE ONLY
 * =================================
 * Adaptive Application-Layer LZ77 Compression with Communication-Efficiency
 * Analysis.
 *
 * This header is new for Q2. It is #include-d by both client.c and server.c
 * but does NOT touch, wrap, or alter a single line of the existing Q1 code
 * in those files. Q1's plain-echo code paths (no "--compress" flag) run
 * completely unmodified and never call anything in this header.
 *
 * WHY A SHARED HEADER (instead of duplicating this code separately into
 * client.c and server.c): the compression format and the message header
 * format must be byte-for-byte identical on both ends, or decoding breaks.
 * Keeping one authoritative copy removes the risk of the two sides quietly
 * drifting apart. Everything here is `static` and header-only, so this adds
 * zero new build rules - the Makefile is completely unchanged.
 *
 * ---------------------------------------------------------------------------
 * ALGORITHM PROVENANCE
 * ---------------------------------------------------------------------------
 * The LZ77 encoder/decoder below is a C port of the author's own Information
 * Theory course project (Python, LZ77 with Elias-delta-style integer prefix
 * codes for match offset/length - see integer_prefixencode/decode in the
 * original notebook). Two deliberate adaptations were made when moving it
 * from an offline information-theory exercise to a live networking feature:
 *
 *   1. FIXED 256-SYMBOL ALPHABET (b = 8 bits/literal), instead of a
 *      per-message reduced alphabet. The original notebook computes a
 *      custom alphabet per input string and passes it to lz77_decode() as
 *      an explicit parameter - which works offline, but a live TCP receiver
 *      has no way to learn a sender's session-specific alphabet without an
 *      extra out-of-band exchange. Since every byte value (0-255) is already
 *      implicitly known to both sides, fixing the alphabet to "all bytes"
 *      removes the need to transmit it at all, at the cost of a fixed
 *      (rather than minimal) per-literal width. This is exactly the same
 *      engineering tradeoff real byte-oriented compressors (e.g. DEFLATE)
 *      make.
 *   2. BOUNDED SLIDING WINDOW + MAX MATCH LENGTH, instead of searching the
 *      entire prefix for the longest match. The notebook's search is O(n^2)
 *      (unbounded backward search over the whole processed string so far),
 *      which is fine for short test strings but becomes impractically slow
 *      for the tens-of-KB messages used in the Q2 benchmark. Real LZ77 (and
 *      DEFLATE) bounds both the back-reference distance and the match
 *      length for exactly this reason. LZ77_WINDOW_SIZE and
 *      LZ77_MAX_MATCH_LEN below play that role here.
 *
 * The bit-level Elias-delta integer prefix code (encode_int_prefix /
 * decode_int_prefix) and the overall greedy longest-match parsing loop are
 * otherwise a direct, faithful translation of the notebook's algorithm.
 * ---------------------------------------------------------------------------
 */

#ifndef LZ77_COMPRESS_H
#define LZ77_COMPRESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>  /* htonl/ntohl */
#include <sys/socket.h> /* send/recv */

/* ---- Tunable constants (performance/format, not correctness) ---- */
#define LZ77_WINDOW_SIZE   4096u  /* how far back a match may point */
#define LZ77_MAX_MATCH_LEN 4096u  /* longest single match reference  */

/* Application-layer message header, sent (in --compress mode only)
 * immediately before every message payload:
 *
 *   byte 0        : compressed flag (0 = raw payload follows, 1 = LZ77
 *                   compressed bitstream follows)
 *   bytes 1..4    : original_len  (uint32_t, network byte order)
 *   bytes 5..8    : payload_len   (uint32_t, network byte order) -
 *                   number of BYTES following the header on the wire
 *   bytes 9..12   : compressed_bits (uint32_t, network byte order) -
 *                   exact bit-length of the compressed bitstream; only
 *                   meaningful when compressed flag == 1 (payload_len is
 *                   ceil(compressed_bits/8) in that case, and the final
 *                   byte's unused low bits are padding to be ignored)
 *
 * Fields are serialized manually into a fixed 13-byte buffer (rather than
 * sent as a raw struct) to avoid any risk of compiler-dependent struct
 * padding/alignment differences between the two ends.
 */
#define Q2_HDR_SIZE 13

/* ===================== Bit writer / bit reader ===================== */

typedef struct {
    uint8_t *buf;
    size_t   byte_cap;
    size_t   bit_len; /* number of valid bits written so far */
} bitwriter_t;

static void bitwriter_init(bitwriter_t *bw, size_t initial_bytes) {
    if (initial_bytes < 16) initial_bytes = 16;
    bw->buf = (uint8_t *)calloc(initial_bytes, 1);
    bw->byte_cap = initial_bytes;
    bw->bit_len = 0;
}

static void bitwriter_ensure(bitwriter_t *bw, size_t extra_bits) {
    size_t needed_bytes = (bw->bit_len + extra_bits + 7) / 8;
    if (needed_bytes <= bw->byte_cap) return;
    size_t new_cap = bw->byte_cap * 2;
    if (new_cap < needed_bytes) new_cap = needed_bytes;
    bw->buf = (uint8_t *)realloc(bw->buf, new_cap);
    memset(bw->buf + bw->byte_cap, 0, new_cap - bw->byte_cap);
    bw->byte_cap = new_cap;
}

/* Appends a single bit (0 or 1), MSB-first within each byte. */
static void bitwriter_put_bit(bitwriter_t *bw, int bit) {
    bitwriter_ensure(bw, 1);
    size_t byte_idx = bw->bit_len / 8;
    int    bit_idx  = 7 - (int)(bw->bit_len % 8); /* MSB-first */
    if (bit) bw->buf[byte_idx] |= (uint8_t)(1u << bit_idx);
    bw->bit_len++;
}

/* Appends the low `width` bits of value, MSB of that field first. */
static void bitwriter_put_bits(bitwriter_t *bw, uint32_t value, int width) {
    for (int i = width - 1; i >= 0; i--) {
        bitwriter_put_bit(bw, (int)((value >> i) & 1u));
    }
}

typedef struct {
    const uint8_t *buf;
    size_t bit_len; /* total valid bits available */
    size_t bit_pos; /* current read position, in bits */
} bitreader_t;

static int bitreader_get_bit(bitreader_t *br) {
    size_t byte_idx = br->bit_pos / 8;
    int    bit_idx  = 7 - (int)(br->bit_pos % 8);
    int bit = (br->buf[byte_idx] >> bit_idx) & 1;
    br->bit_pos++;
    return bit;
}

static uint32_t bitreader_get_bits(bitreader_t *br, int width) {
    uint32_t value = 0;
    for (int i = 0; i < width; i++) {
        value = (value << 1) | (uint32_t)bitreader_get_bit(br);
    }
    return value;
}

/* ===================== Elias-delta style integer prefix code ===================
 * Direct port of integer_prefixencode()/integer_prefixdecode() from the
 * reference notebook. Requires a >= 1 (always true here: LZ77 offsets and
 * match lengths are never zero). */

static int bit_length_u32(uint32_t x) {
    int n = 0;
    while (x) { n++; x >>= 1; }
    return n;
}

static void encode_int_prefix(bitwriter_t *bw, uint32_t a) {
    int L  = bit_length_u32(a);            /* bits needed to represent a       */
    int Lb = bit_length_u32((uint32_t)L);  /* bits needed to represent L itself */
    for (int i = 0; i < Lb - 1; i++) bitwriter_put_bit(bw, 0);
    bitwriter_put_bit(bw, 1);
    bitwriter_put_bits(bw, (uint32_t)L, Lb);
    bitwriter_put_bits(bw, a, L);
}

static uint32_t decode_int_prefix(bitreader_t *br) {
    int zeros = 0;
    while (bitreader_get_bit(br) == 0) zeros++;
    int Lb = zeros + 1;
    uint32_t L = bitreader_get_bits(br, Lb);
    uint32_t a = bitreader_get_bits(br, (int)L);
    return a;
}

/* ===================== LZ77 encode / decode (fixed 256-symbol alphabet) ===== */

typedef struct {
    uint8_t *bytes;   /* malloc'd packed bitstream, owned by caller */
    size_t   byte_len;
    size_t   bit_len; /* exact valid bit count (last byte may be padded) */
} lz77_result_t;

static lz77_result_t lz77_encode(const uint8_t *s, size_t n) {
    bitwriter_t bw;
    bitwriter_init(&bw, n); /* compressed form is rarely bigger than input */

    size_t t = 0;
    while (t < n) {
        size_t max_length = 0, max_pos = 0;
        size_t window_start = (t > LZ77_WINDOW_SIZE) ? (t - LZ77_WINDOW_SIZE) : 0;
        size_t max_possible = n - t;
        if (max_possible > LZ77_MAX_MATCH_LEN) max_possible = LZ77_MAX_MATCH_LEN;

        for (size_t j = window_start; j < t; j++) {
            size_t length = 0;
            while (length < max_possible && s[j + length] == s[t + length]) {
                length++;
            }
            if (length > max_length) {
                max_length = length;
                max_pos = j;
            }
        }

        if (max_length > 0) {
            bitwriter_put_bit(&bw, 1);
            encode_int_prefix(&bw, (uint32_t)(t - max_pos));
            encode_int_prefix(&bw, (uint32_t)max_length);
            t += max_length;
        } else {
            bitwriter_put_bit(&bw, 0);
            bitwriter_put_bits(&bw, s[t], 8);
            t += 1;
        }
    }

    lz77_result_t result;
    result.bit_len  = bw.bit_len;
    result.byte_len = (bw.bit_len + 7) / 8;
    result.bytes    = bw.buf;
    return result;
}

/* Decodes exactly `bit_len` bits from `bits_bytes`, reconstructing a message
 * that is expected to be `expected_len` bytes long (this length comes from
 * the trusted Q2_HDR_SIZE header, not guessed). Returns a malloc'd buffer. */
static uint8_t *lz77_decode(const uint8_t *bits_bytes, size_t bit_len, size_t expected_len) {
    bitreader_t br;
    br.buf = bits_bytes;
    br.bit_len = bit_len;
    br.bit_pos = 0;

    uint8_t *out = (uint8_t *)malloc(expected_len > 0 ? expected_len : 1);
    size_t out_len = 0;

    while (br.bit_pos < br.bit_len) {
        int flag = bitreader_get_bit(&br);
        if (flag == 0) {
            uint32_t byte_val = bitreader_get_bits(&br, 8);
            out[out_len++] = (uint8_t)byte_val;
        } else {
            uint32_t offset = decode_int_prefix(&br);
            uint32_t length = decode_int_prefix(&br);
            size_t start_pos = out_len - offset;
            for (uint32_t k = 0; k < length; k++) {
                out[out_len] = out[start_pos + k];
                out_len++;
            }
        }
    }

    if (out_len != expected_len) {
        fprintf(stderr,
            "warning: lz77_decode() produced %zu bytes, expected %zu "
            "(protocol/header mismatch)\n", out_len, expected_len);
    }
    return out;
}

/* ===================== Reliable full send/recv over TCP ===================
 * TCP is a byte stream, not a message protocol: a single send()/recv() call
 * is not guaranteed to transfer the whole buffer. These helpers loop until
 * exactly `len` bytes have been transferred (or a real error/close occurs).
 * Needed here because Q2 messages can be tens of KB, well beyond what a
 * single send()/recv() call is guaranteed to move in one shot. */

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) { perror("send() failed"); return -1; }
        if (n == 0) { fprintf(stderr, "send() returned 0 unexpectedly\n"); return -1; }
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) { perror("recv() failed"); return -1; }
        if (n == 0) {
            /* A clean close exactly at the START of a new read (got == 0)
             * is the NORMAL, expected way a connection ends once the other
             * side has sent everything it intends to - e.g. Feature 1's
             * client always closes right after its one message, and a
             * Feature 2 --count client closes after its last message. That
             * is not an error and prints nothing. A close with got > 0
             * (partway through an expected header or payload) IS a genuine
             * problem - the peer vanished mid-message - and is worth
             * flagging loudly. */
            if (got > 0) {
                fprintf(stderr,
                    "connection closed prematurely (expected %zu more bytes)\n",
                    len - got);
            }
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

/* ===================== Q2 message layer (header + adaptive choice) ========= */

/* Stats the caller can use for logging/benchmarking. */
typedef struct {
    size_t original_len;
    size_t compressed_len;   /* what compression WOULD produce, always computed */
    int    compression_used; /* 1 if the compressed form was actually sent */
    size_t bytes_sent;       /* Q2_HDR_SIZE + payload actually placed on the wire */
} q2_send_stats_t;

/* Compresses `msg`, adaptively picks whichever representation (compressed or
 * raw) is smaller, and sends [header][payload] over `fd`. Always reports the
 * compressed size in *stats even when the raw form was the one transmitted,
 * so callers can log "what compression would have achieved" either way. */
static int q2_send_message(int fd, const uint8_t *msg, size_t msg_len, q2_send_stats_t *stats) {
    lz77_result_t enc = lz77_encode(msg, msg_len);

    int use_compressed = (enc.byte_len < msg_len) ? 1 : 0;
    size_t payload_len = use_compressed ? enc.byte_len : msg_len;
    const uint8_t *payload_ptr = use_compressed ? enc.bytes : msg;

    uint8_t header[Q2_HDR_SIZE];
    header[0] = (uint8_t)use_compressed;
    uint32_t orig_n = htonl((uint32_t)msg_len);
    uint32_t pay_n  = htonl((uint32_t)payload_len);
    uint32_t bits_n = htonl((uint32_t)(use_compressed ? enc.bit_len : 0));
    memcpy(header + 1, &orig_n, 4);
    memcpy(header + 5, &pay_n,  4);
    memcpy(header + 9, &bits_n, 4);

    int rc = 0;
    if (send_all(fd, header, Q2_HDR_SIZE) < 0) rc = -1;
    if (rc == 0 && send_all(fd, payload_ptr, payload_len) < 0) rc = -1;

    if (stats) {
        stats->original_len     = msg_len;
        stats->compressed_len   = enc.byte_len;
        stats->compression_used = use_compressed;
        stats->bytes_sent       = Q2_HDR_SIZE + payload_len;
    }

    free(enc.bytes);
    return rc;
}

/* Receives one Q2-framed message from `fd`, decompressing if needed.
 * Returns a malloc'd buffer holding the ORIGINAL message and sets *out_len,
 * or NULL on error/connection close. */
static uint8_t *q2_recv_message(int fd, size_t *out_len) {
    uint8_t header[Q2_HDR_SIZE];
    if (recv_all(fd, header, Q2_HDR_SIZE) < 0) return NULL;

    uint8_t compressed_flag = header[0];
    uint32_t orig_n, pay_n, bits_n;
    memcpy(&orig_n, header + 1, 4);
    memcpy(&pay_n,  header + 5, 4);
    memcpy(&bits_n, header + 9, 4);
    uint32_t original_len   = ntohl(orig_n);
    uint32_t payload_len    = ntohl(pay_n);
    uint32_t compressed_bits = ntohl(bits_n);

    uint8_t *payload_buf = (uint8_t *)malloc(payload_len > 0 ? payload_len : 1);
    if (recv_all(fd, payload_buf, payload_len) < 0) {
        free(payload_buf);
        return NULL;
    }

    uint8_t *original;
    if (compressed_flag) {
        original = lz77_decode(payload_buf, compressed_bits, original_len);
        free(payload_buf);
    } else {
        original = payload_buf; /* already the original bytes */
    }

    if (out_len) *out_len = original_len;
    return original;
}

#endif /* LZ77_COMPRESS_H */