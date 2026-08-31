/*
 * client.c - TCP Echo Client with getaddrinfo()-based hostname resolution
 *
 * Baseline (from course slides): socket() -> connect() -> send() -> recv() -> close()
 *
 * Q1 addition: instead of the caller typing an IP address and the program
 * calling inet_pton() on it directly, the caller supplies a HOSTNAME.
 * getaddrinfo() resolves it (via the system resolver: /etc/hosts, DNS,
 * NSS, etc.) into a list of candidate addresses, which we try in turn.
 *
 * Usage:
 *   ./client <server-hostname> <port>
 *
 * Example:
 *   ./client example.com 8080
 *   ./client localhost 12345
 *
 * ============================================================================
 * Q2 ADDITION - Adaptive Application-Layer LZ77 Compression (--compress mode)
 * ============================================================================
 * Everything below this notice and above main() is NEW for Q2. Q1's original
 * code inside main() (the getaddrinfo()/connect()/send()/recv() logic you
 * see further down) is completely untouched - not one line was edited. main()
 * only gained a small dispatch block, inserted BEFORE the original Q1 logic,
 * that checks for a "--compress" flag and, if present, hands off entirely to
 * q2_client_main() below instead of running the Q1 path. Without
 * "--compress", execution falls straight through to the original Q1 code,
 * byte-for-byte as before.
 *
 * Q2 usage:
 *   ./client <hostname> <port> --compress [--file <path>] [--message <text>]
 *                                          [--log <csv-path>] [--type <label>]
 *
 *   --file <path>     read the message to send from this file (binary-safe;
 *                      lets the benchmark script test arbitrary sizes/content
 *                      without hitting command-line length limits)
 *   --message <text>  send this literal string instead (mutually exclusive
 *                      with --file)
 *   --log <csv-path>  append one CSV line of compression/transmission stats
 *                      for this run (used by the automated benchmark)
 *   --type <label>    a label for the CSV row (e.g. "random", "repetitive",
 *                      "text") - purely descriptive, defaults to "unlabeled"
 *
 *   If neither --file nor --message is given, the same default message Q1
 *   uses ("Hello from echo client!") is sent, so a bare
 *   "./client <host> <port> --compress" still works as a quick smoke test.
 *
 * ============================================================================
 * Q2 FEATURE 2 ADDITION - Multi-message workload mode (--count mode)
 * ============================================================================
 * Feature 2 (server-side concurrency, see server.c) needs a client that can
 * drive a realistic "workload" - many messages over one persistent
 * connection, with per-run timing/throughput - rather than the single
 * message Q1 and Feature 1's client send today. This is purely additive: a
 * NEW "--count" flag triggers a NEW dispatch path (q2f2_bench_client_main),
 * completely separate from both the Q1 baseline and Feature 1's existing
 * q2_client_main(), neither of which is touched. "--count" can optionally
 * be combined with "--compress" to benchmark the concurrent+compression
 * combination too.
 *
 * Feature 2 usage:
 *   ./client <hostname> <port> --count <N> [--size <bytes>] [--compress]
 *                                           [--log <csv-path>] [--label <text>]
 *
 *   --count <N>       send N messages, sequentially, over ONE connection
 *   --size <bytes>    size of each generated message (default: same length
 *                      as Q1's default message)
 *   --compress        use the LZ77 protocol for every message (optional;
 *                      without it, the plain Q1-style protocol is used)
 *   --log <csv-path>  write ONE summary row for this client run (always
 *                      overwrites/creates fresh - each concurrently-launched
 *                      client instance is expected to be given its own
 *                      unique path by the caller, so there is never a
 *                      multi-process write race on a shared file)
 *   --label <text>    a label included in the CSV row (e.g. which
 *                      benchmark trial this run belongs to)
 *
 * Each of the N messages has a distinct, deterministic payload (see
 * q2f2_build_message()) specifically so that if the server ever mixed up
 * bytes between two different clients' connections - the exact kind of bug
 * concurrent code is prone to - the echo verification below would catch it
 * as a MISMATCH, not just silently pass.
 */
 
#define _POSIX_C_SOURCE 200809L /* expose getaddrinfo()/struct addrinfo under -std=c11 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>      /* getaddrinfo(), gai_strerror(), struct addrinfo */
 
#include "lz77_compress.h" /* Q2 ONLY: LZ77 codec + message-header protocol */
 
#define BUF_SIZE 1024
 
/* ============================================================================
 * Q2 ONLY: everything from here down to (but not including) main() is new.
 * ============================================================================
 */
 
static const char *Q2_DEFAULT_MESSAGE = "Hello from echo client!";
 
/* Resolves hostname:port and connects, trying every candidate address
 * getaddrinfo() returns - the same resolution/fallback strategy Q1 uses,
 * duplicated here (rather than shared) so the original Q1 main() below
 * needs no modification at all. Returns a connected socket fd, or -1. */
static int q2_connect_to_host(const char *hostname, const char *port_str) {
    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
 
    int gai_err = getaddrinfo(hostname, port_str, &hints, &result);
    if (gai_err != 0) {
        fprintf(stderr, "getaddrinfo() failed for '%s': %s\n",
                hostname, gai_strerror(gai_err));
        return -1;
    }
 
    int sock_fd = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) continue;
        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(result);
 
    if (sock_fd < 0) {
        fprintf(stderr, "Could not connect to %s:%s (all addresses failed)\n",
                hostname, port_str);
    }
    return sock_fd;
}
 
/* Reads an entire file into a malloc'd buffer (binary-safe). Returns NULL on
 * failure. Used so the benchmark script can test arbitrary message sizes
 * without hitting OS command-line argument length limits. */
static uint8_t *q2_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Could not open --file '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "Could not determine size of --file '%s'\n", path);
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
 
    uint8_t *buf = (uint8_t *)malloc((size_t)size > 0 ? (size_t)size : 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        fprintf(stderr, "Short read on --file '%s'\n", path);
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}
 
/* Appends one CSV row to `path`, creating it with a header row first if it
 * doesn't already exist. Columns:
 *   type, message_size, q1_bytes_sent, q2_bytes_sent, compressed_size,
 *   compression_used, compression_ratio, percent_reduction, rtt_ms
 *
 * q1_bytes_sent is what Q1's *actual* protocol would put on the wire for a
 * message of this size: just the raw bytes, no header at all (Q1 never adds
 * one). q2_bytes_sent is what THIS run's Q2 protocol actually sent, header
 * included - so the comparison is honest even when compression isn't used
 * (Q2's header is still 13 bytes of overhead Q1 never pays). */
static void q2_log_csv(const char *path, const char *type, size_t message_size,
                        size_t compressed_size, int compression_used,
                        size_t q2_bytes_sent, double rtt_ms) {
    FILE *f = fopen(path, "r");
    int need_header = (f == NULL);
    if (f) fclose(f);
 
    f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "Could not open --log '%s': %s\n", path, strerror(errno));
        return;
    }
    if (need_header) {
        fprintf(f, "type,message_size,q1_bytes_sent,q2_bytes_sent,compressed_size,"
                    "compression_used,compression_ratio,percent_reduction,rtt_ms\n");
    }
 
    size_t q1_bytes_sent = message_size;
    double compression_ratio = (message_size > 0)
        ? (double)compressed_size / (double)message_size : 0.0;
    double percent_reduction = (message_size > 0)
        ? (1.0 - (double)q2_bytes_sent / (double)message_size) * 100.0 : 0.0;
 
    fprintf(f, "%s,%zu,%zu,%zu,%zu,%d,%.4f,%.2f,%.3f\n",
            type, message_size, q1_bytes_sent, q2_bytes_sent, compressed_size,
            compression_used, compression_ratio, percent_reduction, rtt_ms);
    fclose(f);
}
 
/* The full Q2 client run: parses its own argv (separately from Q1's), sends
 * one message through the adaptive-compression protocol, verifies the
 * echoed reply matches, prints a human-readable summary, and optionally
 * logs a CSV row for the benchmark script. */
static int q2_client_main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <server-hostname> <port> --compress "
            "[--file <path> | --message <text>] [--log <csv-path>] [--type <label>]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
 
    const char *hostname = argv[1];
    const char *port_str = argv[2];
    const char *file_path = NULL;
    const char *message_arg = NULL;
    const char *log_path = NULL;
    const char *type_label = "unlabeled";
 
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) {
            continue; /* already known to be present; nothing to do */
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_path = argv[++i];
        } else if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) {
            message_arg = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            type_label = argv[++i];
        } else {
            fprintf(stderr, "Unrecognized or incomplete option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
 
    uint8_t *message = NULL;
    size_t message_len = 0;
    int message_owned = 0; /* whether we must free(message) */
 
    if (file_path != NULL) {
        message = q2_read_file(file_path, &message_len);
        if (!message) return EXIT_FAILURE;
        message_owned = 1;
    } else if (message_arg != NULL) {
        message_len = strlen(message_arg);
        message = (uint8_t *)message_arg; /* not owned - points into argv */
    } else {
        message_len = strlen(Q2_DEFAULT_MESSAGE);
        message = (uint8_t *)Q2_DEFAULT_MESSAGE;
    }
 
    int sock_fd = q2_connect_to_host(hostname, port_str);
    if (sock_fd < 0) {
        if (message_owned) free(message);
        return EXIT_FAILURE;
    }
    printf("Connected to %s:%s (compression mode)\n", hostname, port_str);
 
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
 
    q2_send_stats_t stats;
    if (q2_send_message(sock_fd, message, message_len, &stats) < 0) {
        close(sock_fd);
        if (message_owned) free(message);
        return EXIT_FAILURE;
    }
 
    size_t reply_len = 0;
    uint8_t *reply = q2_recv_message(sock_fd, &reply_len);
 
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double rtt_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                  + (t_end.tv_nsec - t_start.tv_nsec) / 1.0e6;
 
    if (!reply) {
        fprintf(stderr, "Failed to receive echoed reply\n");
        close(sock_fd);
        if (message_owned) free(message);
        return EXIT_FAILURE;
    }
 
    int matches = (reply_len == message_len && memcmp(reply, message, message_len) == 0);
 
    printf("Original size      : %zu bytes\n", stats.original_len);
    printf("Compressed size     : %zu bytes (%s)\n", stats.compressed_len,
           stats.compression_used ? "used" : "NOT used - raw was smaller/equal");
    printf("Bytes actually sent : %zu (header %d + payload)\n",
           stats.bytes_sent, Q2_HDR_SIZE);
    printf("Round-trip time     : %.3f ms\n", rtt_ms);
    printf("Echo verification   : %s\n", matches ? "MATCH" : "MISMATCH!!");
 
    if (log_path) {
        q2_log_csv(log_path, type_label, stats.original_len, stats.compressed_len,
                   stats.compression_used, stats.bytes_sent, rtt_ms);
    }
 
    free(reply);
    if (message_owned) free(message);
    close(sock_fd);
    return matches ? EXIT_SUCCESS : EXIT_FAILURE;
}
 
/* ============================================================================
 * Q2 FEATURE 2 ONLY: everything from here down to (but not including) main()
 * is new.
 * ============================================================================
 */
 
/* Builds message #index of the requested size: a deterministic, repeating
 * shifted-alphabet pattern that differs by `index`, so every message in a
 * --count run is distinguishable from every other one (see file header
 * comment for why this matters for concurrency testing). */
static uint8_t *q2f2_build_message(size_t size, long index, size_t *out_len) {
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? size : 1);
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)('A' + (((long)i + index) % 26));
    }
    *out_len = size;
    return buf;
}
 
/* Writes exactly one summary row (with header) to a fresh file at `path`.
 * Unlike q2_log_csv() (which appends to a possibly-shared file across many
 * runs), this always creates/overwrites `path` - safe by construction as
 * long as each concurrently-launched client instance is given a distinct
 * path, which is how the benchmark script uses it. */
static void q2f2_log_csv(const char *path, const char *label, int compress_mode,
                          long count_requested, long count_completed, size_t msg_size,
                          double total_ms, double msgs_per_sec, double bytes_per_sec,
                          int success) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Could not open --log '%s': %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "label,mode,count_requested,count_completed,message_size,"
                "total_time_ms,msgs_per_sec,bytes_per_sec,success\n");
    fprintf(f, "%s,%s,%ld,%ld,%zu,%.3f,%.2f,%.2f,%d\n",
            label, compress_mode ? "compress" : "plain",
            count_requested, count_completed, msg_size,
            total_ms, msgs_per_sec, bytes_per_sec, success);
    fclose(f);
}
 
/* The full --count workload run: connects once, sends `count` distinct
 * messages back-to-back over the SAME connection, verifying each echo
 * exactly, then reports/optionally logs aggregate timing and throughput. */
static int q2f2_bench_client_main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <server-hostname> <port> --count <N> [--size <bytes>] "
            "[--compress] [--log <csv-path>] [--label <text>]\n", argv[0]);
        return EXIT_FAILURE;
    }
 
    const char *hostname = argv[1];
    const char *port_str = argv[2];
    int compress_mode = 0;
    long count = 1;
    long msg_size = (long)strlen(Q2_DEFAULT_MESSAGE);
    const char *log_path = NULL;
    const char *label = "client";
 
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) {
            compress_mode = 1;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atol(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            msg_size = atol(argv[++i]);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else {
            fprintf(stderr, "Unrecognized or incomplete option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (count < 1) count = 1;
    if (msg_size < 1) msg_size = 1;
 
    /* See the matching comment in server.c's q2f2_multi_server_main(): if
     * the server side ever closes/errors mid-stream (e.g. under heavy
     * concurrent load), a subsequent send() here could raise SIGPIPE and
     * kill this process outright before it can report anything. Ignoring
     * it means send() returns -1/EPIPE instead, which the existing
     * send_all()/q2_send_message() error handling already handles. */
    signal(SIGPIPE, SIG_IGN);
 
    int sock_fd = q2_connect_to_host(hostname, port_str);
    if (sock_fd < 0) return EXIT_FAILURE;
 
    printf("[%s] Connected to %s:%s - sending %ld message(s) of %ld bytes each (%s mode)\n",
           label, hostname, port_str, count, msg_size, compress_mode ? "compress" : "plain");
 
    uint8_t *plain_reply_buf = NULL;
    if (!compress_mode) {
        plain_reply_buf = (uint8_t *)malloc((size_t)msg_size);
    }
 
    long completed = 0;
    long mismatches = 0;
    size_t total_bytes = 0;
 
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
 
    for (long i = 0; i < count; i++) {
        size_t mlen = 0;
        uint8_t *msg = q2f2_build_message((size_t)msg_size, i, &mlen);
        int ok = 0;
 
        if (!compress_mode) {
            if (send_all(sock_fd, msg, mlen) == 0 &&
                recv_all(sock_fd, plain_reply_buf, mlen) == 0) {
                ok = (memcmp(plain_reply_buf, msg, mlen) == 0);
            }
        } else {
            q2_send_stats_t stats;
            if (q2_send_message(sock_fd, msg, mlen, &stats) == 0) {
                size_t reply_len = 0;
                uint8_t *reply = q2_recv_message(sock_fd, &reply_len);
                if (reply) {
                    ok = (reply_len == mlen && memcmp(reply, msg, mlen) == 0);
                    free(reply);
                }
            }
        }
 
        free(msg);
 
        if (ok) {
            completed++;
            total_bytes += mlen;
        } else {
            mismatches++;
            fprintf(stderr, "[%s] message %ld failed verification or transport - stopping\n",
                    label, i);
            break; /* connection is likely desynced; no point continuing */
        }
    }
 
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double total_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                     + (t_end.tv_nsec - t_start.tv_nsec) / 1.0e6;
 
    free(plain_reply_buf);
    close(sock_fd);
 
    int success = (mismatches == 0 && completed == count);
    double total_sec = total_ms / 1000.0;
    double msgs_per_sec  = (total_sec > 0.0) ? (double)completed / total_sec : 0.0;
    double bytes_per_sec = (total_sec > 0.0) ? (double)total_bytes / total_sec : 0.0;
 
    printf("[%s] completed=%ld/%ld total_time=%.3f ms msgs/sec=%.1f bytes/sec=%.1f success=%s\n",
           label, completed, count, total_ms, msgs_per_sec, bytes_per_sec,
           success ? "YES" : "NO");
 
    if (log_path) {
        q2f2_log_csv(log_path, label, compress_mode, count, completed,
                     (size_t)msg_size, total_ms, msgs_per_sec, bytes_per_sec, success);
    }
 
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
 
int main(int argc, char *argv[]) {
    /* ========================================================================
     * Q2 DISPATCH (new). Checked in order:
     *   "--count"    -> Feature 2: multi-message workload mode
     *                    (q2f2_bench_client_main), which independently also
     *                    honours "--compress" if present.
     *   "--compress" (without "--count") -> Feature 1: existing single-
     *                    message compression mode (q2_client_main), UNCHANGED.
     *   neither flag -> falls through to the ORIGINAL, UNMODIFIED Q1 code
     *                    exactly as it always was.
     * ======================================================================== */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0) {
            return q2f2_bench_client_main(argc, argv);
        }
    }
 
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) {
            return q2_client_main(argc, argv);
        }
    }
 
    /* ========================================================================
     * Q1 BASELINE BELOW THIS LINE - UNCHANGED, UNTOUCHED, IDENTICAL TO THE
     * ORIGINALLY SUBMITTED Q1 client.c.
     * ======================================================================== */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server-hostname> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
 
    const char *hostname = argv[1];
    const char *port_str = argv[2];
 
    /*
     * Q1: Build hints and call getaddrinfo() to resolve the hostname.
     * AF_UNSPEC lets the resolver return either IPv4 or IPv6 addresses,
     * matching whatever DNS / /etc/hosts / NSS provides for this host.
     */
    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
 
    int gai_err = getaddrinfo(hostname, port_str, &hints, &result);
    if (gai_err != 0) {
        /* getaddrinfo() returns its own error codes -> use gai_strerror(),
         * NOT perror()/errno. */
        fprintf(stderr, "getaddrinfo() failed for '%s': %s\n",
                hostname, gai_strerror(gai_err));
        exit(EXIT_FAILURE);
    }
 
    /*
     * Q1: Iterate the returned address list and try each one until a
     * connect() succeeds. This handles hosts that resolve to multiple
     * addresses (e.g. both IPv4 and IPv6, or several IPv4 records).
     */
    int sock_fd = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        char addr_str[INET6_ADDRSTRLEN];
        void *addr_ptr = NULL;
 
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
            addr_ptr = &ipv4->sin_addr;
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
            addr_ptr = &ipv6->sin6_addr;
        }
        if (addr_ptr != NULL &&
            inet_ntop(rp->ai_family, addr_ptr, addr_str, sizeof(addr_str)) != NULL) {
            printf("Trying address: %s\n", addr_str);
        }
 
        /* Baseline: socket() using the family/type/protocol getaddrinfo() gave us */
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            perror("socket() failed");
            continue;
        }
 
        /* Baseline: connect() */
        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; /* success - stop trying further addresses */
        }
 
        perror("connect() failed");
        close(sock_fd);
        sock_fd = -1;
    }
 
    /* Q1: always free the address list once we're done with it */
    freeaddrinfo(result);
 
    if (sock_fd < 0) {
        fprintf(stderr, "Could not connect to %s:%s (all addresses failed)\n",
                hostname, port_str);
        exit(EXIT_FAILURE);
    }
 
    printf("Connected to %s:%s\n", hostname, port_str);
 
    /* Baseline: send() an ordinary text message */
    const char *message = "Hello from echo client!";
    size_t msg_len = strlen(message);
 
    ssize_t total_sent = 0;
    while ((size_t)total_sent < msg_len) {
        ssize_t sent = send(sock_fd, message + total_sent, msg_len - (size_t)total_sent, 0);
        if (sent < 0) {
            perror("send() failed");
            close(sock_fd);
            exit(EXIT_FAILURE);
        }
        total_sent += sent;
    }
    printf("Sent: %s\n", message);
 
    /* Baseline: recv() the echoed reply */
    char buffer[BUF_SIZE];
    ssize_t n = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
    if (n < 0) {
        perror("recv() failed");
        close(sock_fd);
        exit(EXIT_FAILURE);
    } else if (n == 0) {
        fprintf(stderr, "Server closed the connection before echoing\n");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }
    buffer[n] = '\0';
    printf("Received echo: %s\n", buffer);
 
    close(sock_fd);
    return 0;
}
 