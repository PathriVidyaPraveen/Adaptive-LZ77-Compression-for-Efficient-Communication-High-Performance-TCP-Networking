/*
 * server.c - Simple TCP Echo Server
 *
 * Follows the socket-programming pattern from the course slides:
 *   socket() -> bind() -> listen() -> accept() -> recv()/send() -> close()
 *
 * Usage:
 *   ./server <port>
 *
 * No new (Q1) APIs are used here - the server side is unchanged from the
 * baseline course material. getaddrinfo() only affects the client, since
 * only the client needs to resolve a hostname.
 *
 * ============================================================================
 * Q2 FEATURE 1 - Adaptive Application-Layer LZ77 Compression (--compress mode)
 * ============================================================================
 * Everything below this notice and above main() is NEW for Q2. Q1's original
 * code inside main() (the socket()/bind()/listen()/accept()/recv()/send()
 * loop you see further down) is completely untouched - not one line was
 * edited. main() only gained a small dispatch block, inserted BEFORE the
 * original Q1 logic, that checks for a "--compress" flag and, if present,
 * hands off entirely to q2_server_main() below instead of running the Q1
 * path. Without "--compress", execution falls straight through to the
 * original Q1 code, byte-for-byte as before.
 *
 * Feature 1 usage:
 *   ./server <port> --compress
 *
 * The server still serves one client connection at a time (exactly like Q1's
 * accept() loop) - Feature 1 is the compression protocol itself, not
 * concurrency (that is Feature 2, below, an independent addition). For each
 * connection, the server receives one Q2-framed message, decompresses it if
 * needed, prints the same adaptive-compression stats the client prints, and
 * echoes the message back through the same adaptive Q2 protocol (so the
 * client can verify the round trip and log its own transmit-direction
 * stats).
 *
 * ============================================================================
 * Q2 FEATURE 2 - Concurrent Multi-Client TCP Echo Server (--multi mode)
 * ============================================================================
 * Adds a "--multi" flag that makes the server handle every accepted client
 * connection on its own POSIX thread (pthread), instead of finishing one
 * client before accept()-ing the next. This is a genuinely independent
 * feature from Feature 1: "--multi" alone gives a concurrent server using
 * the PLAIN (Q1) echo protocol, "--compress" alone gives the existing
 * sequential compression server (unchanged), and "--multi --compress"
 * together give a concurrent, compression-aware server - the two flags
 * combine freely because each connection's handler function is chosen
 * independently of how the connection was accepted.
 *
 * Feature 2 usage:
 *   ./server <port> --multi              (concurrent, plain echo protocol)
 *   ./server <port> --multi --compress   (concurrent, LZ77 compression too)
 *
 * WHERE CONCURRENCY IS INSERTED: q2f2_multi_server_main() below runs the same
 * socket()/bind()/listen()/accept() setup as q2_server_main() (Feature 1's
 * sequential server), but for each accepted connection it spawns a detached
 * pthread (q2f2_client_thread) instead of handling the connection inline.
 * That thread then calls whichever per-connection handler matches the mode:
 *   - compress_mode: reuses the EXISTING q2_handle_client() from Feature 1,
 *     completely unmodified - the LZ77 protocol code is not duplicated.
 *   - plain mode: calls q1_style_handle_client_plain() (new below), which
 *     mirrors Q1's original inline accept-loop body byte-for-byte in
 *     behavior, just extracted into a reusable function so it can be called
 *     from a thread. Q1's own main() below is still never touched or called
 *     from here.
 *
 * THREAD LIFECYCLE: threads are created DETACHED (PTHREAD_CREATE_DETACHED),
 * not joined. This is the appropriate choice here because the server's main
 * thread has no result to collect from a finished client thread and must
 * keep accepting new connections indefinitely - joining would mean either
 * blocking the accept() loop (defeating the purpose of threading at all) or
 * tracking and periodically joining an ever-growing list of thread IDs for
 * no benefit. A detached thread's resources (stack, thread control block)
 * are automatically reclaimed by the OS the moment the thread function
 * returns, which is exactly the cleanup behaviour we want per client.
 *
 * SHARED STATE / RACE CONDITIONS: deliberately, there is none. Each thread
 * only ever touches its own connection fd and its own stack-local buffers;
 * no client's data is visible to any other thread, and no global mutable
 * counters or structures are shared between threads. The one thing that IS
 * shared is stdout (used for the demonstration logging below) - glibc's
 * stdio functions (printf, fprintf) are internally locked per FILE*, so
 * concurrent printf() calls from different threads cannot interleave
 * mid-line or corrupt each other; lines from different threads may appear
 * in any order relative to each other, but each individual line is always
 * printed atomically. No additional mutex is needed for this.
 */
 
#define _POSIX_C_SOURCE 200809L
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
 
#include "lz77_compress.h" /* Q2 ONLY: LZ77 codec + message-header protocol */
 
#define BUF_SIZE 1024
#define BACKLOG  10
 
/* ============================================================================
 * Q2 ONLY: everything from here down to (but not including) main() is new.
 * ============================================================================
 */
 
/* Handles one client connection under the Q2 adaptive-compression protocol.
 *
 * LOOPS over messages until the client closes the connection (q2_recv_message
 * returning NULL, via recv_all() seeing a clean close, is how end-of-stream
 * is detected - no protocol change, just repeated use of the existing
 * per-message functions). This matters for Feature 2: a --count-mode client
 * sends several messages back-to-back over ONE connection, so this handler
 * must keep serving that connection until the client is done, not stop
 * after the first message. This is also a correctness fix that benefits
 * Feature 1's existing sequential server: previously, if any compress-mode
 * client had ever tried to send a second message on the same connection,
 * the old single-shot version of this function would have returned after
 * the first message and the caller would immediately close the socket out
 * from under it. Feature 1's own client only ever sent one message, so this
 * was never observed there - but it is the correct, general behaviour for a
 * per-connection handler either way, and the on-the-wire message format is
 * completely unchanged. */
static void q2_handle_client(int conn_fd, const char *client_ip, int client_port) {
    for (;;) {
        size_t msg_len = 0;
        uint8_t *msg = q2_recv_message(conn_fd, &msg_len);
        if (!msg) {
            /* Either the client closed the connection normally (expected,
             * once it has sent all of its messages) or a transport error
             * occurred; either way, there is nothing more to serve. */
            break;
        }
 
        printf("[compress mode] Received message from %s:%d - %zu bytes (after decompression)\n",
               client_ip, client_port, msg_len);
 
        q2_send_stats_t stats;
        if (q2_send_message(conn_fd, msg, msg_len, &stats) < 0) {
            fprintf(stderr, "Failed to echo Q2 message back to %s:%d\n", client_ip, client_port);
            free(msg);
            break;
        }
 
        printf("[compress mode] Echoed back to %s:%d - original %zu bytes, "
               "compressed %zu bytes (%s), %zu bytes sent on wire\n",
               client_ip, client_port, stats.original_len, stats.compressed_len,
               stats.compression_used ? "used" : "not used", stats.bytes_sent);
 
        free(msg);
    }
}
 
/* The full Q2 server run: parses its own argv (separately from Q1's) and
 * runs the same accept()-one-at-a-time loop Q1 uses, but dispatching each
 * connection to q2_handle_client() instead of Q1's plain echo loop. */
static int q2_server_main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> --compress\n", argv[0]);
        return EXIT_FAILURE;
    }
 
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
 
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }
 
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons((uint16_t)port);
 
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    printf("Echo server (compression mode) listening on port %d...\n", port);
 
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
 
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept() failed");
            continue;
        }
 
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);
        printf("Accepted connection from %s:%d\n", client_ip, client_port);
 
        q2_handle_client(conn_fd, client_ip, client_port);
 
        close(conn_fd);
        printf("Client %s:%d closed connection\n", client_ip, client_port);
    }
 
    close(listen_fd); /* unreachable, kept for completeness */
    return 0;
}
 
/* ============================================================================
 * Q2 FEATURE 2 ONLY: everything from here down to (but not including) main()
 * is new.
 * ============================================================================
 */
 
/* Handles one client connection under the PLAIN (Q1) echo protocol: this is
 * Q1's original inline accept-loop body, extracted verbatim into a function
 * so a pthread can call it. Behaviour is identical to Q1's loop - same
 * recv()/send() pattern, same partial-send handling, same log messages
 * (with a client-address prefix added, since with multiple concurrent
 * clients the plain "Received N bytes: ..." line alone would be ambiguous
 * about which client it came from). Q1's own main() below still contains
 * its own separate, untouched copy of this same loop - this function does
 * not replace or call into that code. */
static void q1_style_handle_client_plain(int conn_fd, const char *client_ip, int client_port) {
    char buffer[BUF_SIZE];
    ssize_t n;
    while ((n = recv(conn_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = '\0';
        printf("[%s:%d] Received %zd bytes: %s\n", client_ip, client_port, n, buffer);
 
        ssize_t total_sent = 0;
        while (total_sent < n) {
            ssize_t sent = send(conn_fd, buffer + total_sent, (size_t)(n - total_sent), 0);
            if (sent < 0) {
                perror("send() failed");
                break;
            }
            total_sent += sent;
        }
    }
    if (n < 0) {
        perror("recv() failed");
    }
}
 
/* Heap-allocated argument block handed to each client thread. Allocated by
 * the accept() loop just before pthread_create(), freed by the thread
 * itself once it has copied out what it needs - this avoids any risk of
 * the accept loop reusing/overwriting a stack variable before a newly
 * created thread has read it. */
typedef struct {
    int  conn_fd;
    char client_ip[INET_ADDRSTRLEN];
    int  client_port;
    int  compress_mode; /* whether this connection should use the LZ77 protocol */
} q2f2_client_args_t;
 
static void *q2f2_client_thread(void *arg) {
    q2f2_client_args_t *a = (q2f2_client_args_t *)arg;
 
    printf("[thread %lu] Handling client %s:%d (%s mode)\n",
           (unsigned long)pthread_self(), a->client_ip, a->client_port,
           a->compress_mode ? "compression" : "plain");
 
    if (a->compress_mode) {
        q2_handle_client(a->conn_fd, a->client_ip, a->client_port); /* Feature 1's handler, reused as-is */
    } else {
        q1_style_handle_client_plain(a->conn_fd, a->client_ip, a->client_port);
    }
 
    printf("[thread %lu] Finished client %s:%d\n",
           (unsigned long)pthread_self(), a->client_ip, a->client_port);
 
    close(a->conn_fd);
    free(a);
    return NULL;
}
 
/* The full concurrent server run: same socket setup as q2_server_main(),
 * but accept()-ed connections are dispatched to a new detached pthread
 * each, instead of being handled inline before the next accept(). Whether
 * each connection uses the LZ77 protocol is controlled independently by
 * whether "--compress" was ALSO given alongside "--multi". */
static int q2f2_multi_server_main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> --multi [--compress]\n", argv[0]);
        return EXIT_FAILURE;
    }
 
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
 
    int compress_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) compress_mode = 1;
    }
 
    /* With multiple concurrent client threads, it's normal for a client to
     * disconnect while a thread still has data queued to send() to it (e.g.
     * the client finished its --count messages and closed early, or simply
     * crashed). Without this, the SECOND such send() would raise SIGPIPE,
     * whose default action terminates the ENTIRE process - taking down the
     * whole server, including every other client's thread, over one
     * client's ordinary disconnect. Ignoring SIGPIPE means send() instead
     * returns -1/EPIPE, which the existing send_all()/q2_send_message()
     * error handling already deals with correctly (that thread's connection
     * ends; every other thread and the server itself are unaffected). */
    signal(SIGPIPE, SIG_IGN);
 
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }
 
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons((uint16_t)port);
 
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }
 
    printf("Echo server (concurrent mode%s) listening on port %d...\n",
           compress_mode ? " + compression" : "", port);
 
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
 
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept() failed");
            continue;
        }
 
        q2f2_client_args_t *args = (q2f2_client_args_t *)malloc(sizeof(q2f2_client_args_t));
        if (!args) {
            fprintf(stderr, "malloc() failed for client args - dropping connection\n");
            close(conn_fd);
            continue;
        }
        args->conn_fd = conn_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, args->client_ip, sizeof(args->client_ip));
        args->client_port = ntohs(client_addr.sin_port);
        args->compress_mode = compress_mode;
 
        printf("Accepted connection from %s:%d - dispatching to a new thread\n",
               args->client_ip, args->client_port);
 
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
 
        int rc = pthread_create(&tid, &attr, q2f2_client_thread, args);
        pthread_attr_destroy(&attr);
 
        if (rc != 0) {
            fprintf(stderr, "pthread_create() failed: %s - dropping connection\n", strerror(rc));
            close(conn_fd);
            free(args);
            continue;
        }
        /* Detached: no pthread_join() call. The thread cleans up (closes
         * conn_fd, frees args) and its OS resources are reclaimed
         * automatically the moment it returns - see the file header
         * comment above for why detach (not join) is the right choice for
         * a server that must keep accepting new clients indefinitely. */
    }
 
    close(listen_fd); /* unreachable, kept for completeness */
    return 0;
}
 
int main(int argc, char *argv[]) {
    /* ========================================================================
     * Q2 DISPATCH (new). Scans argv once for both feature flags:
     *   "--multi"    -> Feature 2: concurrent server (q2f2_multi_server_main),
     *                    which independently also honours "--compress" if
     *                    present, to run compression on every thread's
     *                    connection too.
     *   "--compress" (without "--multi") -> Feature 1: existing sequential
     *                    compression server (q2_server_main), UNCHANGED.
     *   neither flag -> falls through to the ORIGINAL, UNMODIFIED Q1 code
     *                    exactly as it always was.
     * ======================================================================== */
    int multi_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi") == 0) multi_mode = 1;
    }
    if (multi_mode) {
        return q2f2_multi_server_main(argc, argv);
    }
 
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compress") == 0) {
            return q2_server_main(argc, argv);
        }
    }
 
    /* ========================================================================
     * Q1 BASELINE BELOW THIS LINE - UNCHANGED, UNTOUCHED, IDENTICAL TO THE
     * ORIGINALLY SUBMITTED Q1 server.c.
     * ======================================================================== */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
 
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }
 
    /* 1. Create a TCP socket (slide: socket() System Call) */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
 
    /* Allow quick restart of the server on the same port during testing */
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
 
    /* 2. Fill sockaddr_in and bind() (slide: bind() System Call) */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons((uint16_t)port);
 
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
 
    /* 3. Wait for connections (slide: listen() System Call) */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
 
    printf("Echo server listening on port %d...\n", port);
 
    /* 4. Accept and serve clients one at a time (slide: accept() System Call) */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
 
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept() failed");
            continue; /* keep serving other clients */
        }
 
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("Accepted connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
 
        /* 5. Echo loop: recv() then send() back the same bytes */
        char buffer[BUF_SIZE];
        ssize_t n;
        while ((n = recv(conn_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[n] = '\0';
            printf("Received %zd bytes: %s\n", n, buffer);
 
            ssize_t total_sent = 0;
            while (total_sent < n) {
                ssize_t sent = send(conn_fd, buffer + total_sent, (size_t)(n - total_sent), 0);
                if (sent < 0) {
                    perror("send() failed");
                    break;
                }
                total_sent += sent;
            }
        }
 
        if (n < 0) {
            perror("recv() failed");
        } else {
            printf("Client %s:%d closed connection\n", client_ip, ntohs(client_addr.sin_port));
        }
 
        close(conn_fd);
    }
 
    close(listen_fd); /* unreachable in this simple loop, kept for completeness */
    return 0;
}
 