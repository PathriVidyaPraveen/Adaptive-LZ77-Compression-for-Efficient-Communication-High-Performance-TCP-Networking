# TCP Echo Client/Server — Q1 (getaddrinfo hostname resolution)

## Requirements

- Linux
- GCC (C11)
- `make`
- Standard POSIX socket headers only (`<sys/socket.h>`, `<netinet/in.h>`,
  `<arpa/inet.h>`, `<netdb.h>`, `<unistd.h>`) — no external networking
  libraries or wrappers are used.

## Compilation

```sh
make
```

This builds two binaries: `server` and `client`. `make clean` removes them.

## Starting the server

```sh
./server <port>
```

Example:

```sh
./server 12345
```

The server binds to `INADDR_ANY` on the given port and serves clients
one connection at a time, echoing back whatever bytes it receives until
the client closes the connection.

## Running the client with a hostname

```sh
./client <server-hostname> <port>
```

Examples:

```sh
./client localhost 12345
./client myserver.example.com 12345
./client 127.0.0.1 12345     # a numeric address also works, since
                              # getaddrinfo() accepts both names and
                              # numeric addresses
```

The client resolves `<server-hostname>` with `getaddrinfo()`, connects
to the first address that succeeds, sends one fixed text message, prints
the server's echoed reply, and exits.

## Q1 implementation summary

The course slides' baseline echo client builds a `struct sockaddr_in`
manually and calls `inet_pton()` on a numeric IP string typed by the
user. Q1 replaces that with proper hostname resolution:

- `getaddrinfo(hostname, port, &hints, &result)` resolves the hostname
  into a linked list of `struct addrinfo` candidates, using
  `hints.ai_family = AF_UNSPEC` (IPv4 or IPv6), `ai_socktype =
  SOCK_STREAM`, `ai_protocol = IPPROTO_TCP`.
- The client walks the list (`rp = rp->ai_next`) and calls `socket()`
  + `connect()` on each candidate until one succeeds — this handles
  hosts with multiple DNS records (e.g. multiple A records, or both
  A and AAAA records).
- Resolver errors are reported with `gai_strerror()` (the correct way
  to interpret `getaddrinfo()`'s return code — it is *not* `errno`).
- `freeaddrinfo(result)` is called once the address list is no longer
  needed, whether or not a connection succeeded.
- `gethostbyname()` and other deprecated resolution APIs are not used.

Everything else (server side, `send()`/`recv()`, socket lifecycle) is
unchanged from the baseline pattern taught in the slides.

## Capturing DNS traffic for the assignment's evidence requirement

Calling `getaddrinfo()` does **not** by itself guarantee a DNS packet
will be visible on the wire — the actual traffic depends on your
system's resolver configuration:

- If the hostname is `localhost`, or is listed in `/etc/hosts`, or is
  already cached by the local resolver (e.g. `systemd-resolved`,
  `nscd`), `getaddrinfo()` resolves it **without** sending any DNS
  query — NSS (`/etc/nsswitch.conf`, typically `hosts: files dns`)
  answers from `/etc/hosts` first.
- To force a real DNS lookup, use a hostname that is **not** in
  `/etc/hosts` and not already cached (e.g. a real public domain such
  as `example.com`, or a hostname on your own network resolved via
  your local DNS server), and flush any local resolver cache first if
  needed (e.g. `sudo systemd-resolve --flush-caches` or `sudo
  resolvectl flush-caches`, depending on your distro).

Steps to capture the evidence:

1. Start a packet capture before running the client, filtered to DNS
   traffic:
   ```sh
   sudo tcpdump -i any port 53 -w dns_capture.pcap
   ```
   or open Wireshark with the same capture filter (`port 53`) or
   display filter (`dns`) on the appropriate interface.
2. In another terminal, start the server: `./server <port>`.
3. Run the client with a hostname that requires an actual DNS lookup:
   `./client <hostname-not-in-hosts-or-cache> <port>`.
4. Stop the capture and confirm you can see:
   - a **DNS query** packet (client → resolver, e.g. an `A` or `AAAA`
     query for the hostname), and
   - a **DNS response** packet (resolver → client) containing the
     resolved address.
5. Also take a screenshot of the client's terminal output showing
   `Trying address: ...`, `Connected to ...`, `Sent: ...`, and
   `Received echo: ...` — this is the "successful execution" screenshot
   the assignment asks for.

If your resolver uses a local stub (e.g. `systemd-resolved` listening
on `127.0.0.53`), the "real" outbound DNS query to your upstream DNS
server may only be visible by capturing on the interface used for the
upstream request rather than `lo`; capturing on `any` with `sudo
tcpdump -i any port 53` covers both cases.

## References

- `getaddrinfo(3)`, `gai_strerror(3)`, `socket(2)`, `bind(2)`,
  `listen(2)`, `accept(2)`, `connect(2)`, `send(2)`, `recv(2)`,
  `inet_ntop(3)`, `inet_pton(3)` — Linux man pages
  (`man 3 getaddrinfo`, etc.)
- POSIX.1-2017, `<netdb.h>` and `<sys/socket.h>` specifications
  (IEEE Std 1003.1)
- Beej's Guide to Network Programming, "Client-Server Background" and
  "getaddrinfo()" sections — https://beej.us/guide/bgnet/
- Kurose & Ross, *Computer Networking: A Top-Down Approach*, Chapter 2
  (Socket Programming with TCP)
- Course slides: "Network Programming Basics" (Kotaro Kataoka)
