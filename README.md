# CS3530 Computer Networks — Assignment 1

TCP Echo Client/Server in C (POSIX sockets), with hostname resolution (Q1),
adaptive LZ77 compression (Q2 Feature 1), and a concurrent multi-client
server (Q2 Feature 2). Every command below is plain Linux/WSL2 bash — no
Windows-specific tools are used anywhere except opening a `.pcap` file in
the Wireshark GUI, which is called out explicitly where it happens.

## Contents

| File | Purpose |
|---|---|
| `server.c` | Echo server: Q1 baseline, `--compress` (Feature 1), `--multi` (Feature 2) |
| `client.c` | Echo client: Q1 baseline, `--compress` mode, `--count` benchmark-workload mode |
| `lz77_compress.h` | LZ77 codec + message-header protocol used by `--compress` mode |
| `Makefile` | Builds `server` and `client` |
| `benchmark_compression.py` | Automated benchmark + plots for Q2 Feature 1 (compression) |
| `benchmark_feature2.py` | Automated benchmark + plots for Q2 Feature 2 (concurrency) |

## Requirements

- Linux or WSL2 (Ubuntu)
- `gcc` with pthread support (standard on any Linux/WSL2 install)
- `tcpdump`, `valgrind` (for Q1 verification steps below)
- Python 3 with `matplotlib`

Install anything missing:
```bash
sudo apt-get update
sudo apt-get install -y tcpdump valgrind
python3 -c "import matplotlib" 2>/dev/null && echo "matplotlib OK" || pip install matplotlib --break-system-packages
```

## 0. Build

```bash
make clean
make
```
This produces two binaries, `server` and `client`, with no warnings.

---

## 1. Q1 — Hostname Resolution + Plain Echo

### 1.1 Basic round trip

```bash
./server 12399 &
SPID=$!
sleep 1
./client localhost 12399
kill -9 $SPID
```
**Expected client output:**
```
Trying address: 127.0.0.1
Connected to localhost:12399
Sent: Hello from echo client!
Received echo: Hello from echo client!
```

### 1.2 Prove `getaddrinfo()` is actually resolving (not hardcoded)

Add a fake hostname to `/etc/hosts` and connect to it by name:
```bash
echo "127.0.0.1  myfakehost.local" | sudo tee -a /etc/hosts

./server 12399 &
SPID=$!
sleep 1
./client myfakehost.local 12399
kill -9 $SPID

sudo sed -i '/myfakehost.local/d' /etc/hosts
```
If the echo still works by name, resolution is genuinely happening through
`getaddrinfo()`.

### 1.3 Multi-address / dual-stack fallback

Give one hostname both an IPv6 and an IPv4 record, and confirm the client
tries the IPv6 candidate, rejects it (nothing listens there), and correctly
falls back to IPv4:
```bash
echo "::1        dualstack.local" | sudo tee -a /etc/hosts
echo "127.0.0.1  dualstack.local" | sudo tee -a /etc/hosts

./server 12399 &
SPID=$!
sleep 1
./client dualstack.local 12399
kill -9 $SPID

sudo sed -i '/dualstack.local/d' /etc/hosts
```
**Expected:**
```
Trying address: ::1
connect() failed: Connection refused
Trying address: 127.0.0.1
Connected to dualstack.local:12399
...
Received echo: Hello from echo client!
```

### 1.4 Error-path testing

```bash
# nonexistent hostname -> clean gai_strerror() message, no crash
./client this-domain-does-not-exist-xyz123.invalid 12399

# valid hostname, nothing listening on that port -> clean Connection refused
./client localhost 9999

# missing arguments -> usage message
./client
./client localhost
```

### 1.5 Memory-safety check (Valgrind)

```bash
./server 12399 &
SPID=$!
sleep 1
valgrind --leak-check=full --show-leak-kinds=all ./client localhost 12399
kill -9 $SPID
```
Look for `All heap blocks were freed -- no leaks are possible` and
`ERROR SUMMARY: 0 errors`. Also run it against the error paths, since a
forgotten `freeaddrinfo()` on a failure branch is exactly what this catches:
```bash
valgrind --leak-check=full ./client this-domain-does-not-exist-xyz123.invalid 12399
valgrind --leak-check=full ./client localhost 9999
```

### 1.6 DNS query/response evidence (required for Q1)

`localhost` and `/etc/hosts` entries **never touch the network** — NSS
answers from files, so nothing shows up in a packet capture for them. You
need a hostname resolved purely via DNS to actually see a query/response.

```bash
# 1. Start a capture BEFORE running the client
sudo tcpdump -i any port 53 -w dns_capture.pcap &
TCPDUMP_PID=$!
sleep 1

# 2. Run the client against a real external hostname
#    (port 80 just needs SOMETHING listening so connect() completes -
#     the goal here is only to trigger the DNS lookup, not to echo-test
#     against your own server)
./client example.com 80

# 3. Stop the capture
sleep 1
kill -9 $TCPDUMP_PID
```

**Expected client output** — note this is a real external web server
responding, not your echo server, so an HTTP error is completely normal
here (you sent an echo-client message, not a valid HTTP request):
```
Trying address: 104.20.23.154
Connected to example.com:80
Sent: Hello from echo client!
Received echo: HTTP/1.1 400 Bad Request
Server: cloudflare
...
```
This confirms the full chain worked: `getaddrinfo()` resolved
`example.com` over real DNS, `connect()` reached Cloudflare's edge server,
and `send()`/`recv()` completed a genuine round trip — Cloudflare's `400`
is just its own server correctly rejecting a non-HTTP payload, which is
expected and fine. **Your actual echo-functionality demo should still use
your own server** (Section 1.1) — this step's only job is producing DNS
evidence.

**4. Inspect the capture:**
```bash
tcpdump -r dns_capture.pcap -n
```
You should see lines like:
```
... IP <your-ip>.xxxxx > 8.8.8.8.53: ... A? example.com. ...
... IP 8.8.8.8.53 > <your-ip>.xxxxx: ... A 104.20.23.154 ...
```
(There may also be unrelated `motd.ubuntu.com` lines from WSL's own
shell-startup check — ignore those.)

**5. Open it in Wireshark** (this is the one Windows-GUI step):
```bash
cp dns_capture.pcap "/mnt/c/Users/<you>/Desktop/dns_capture.pcap"
```
Then in Windows, open Wireshark → **File → Open** → select the file. Filter
with `dns`, click the query packet (`Standard query ... A example.com`) and
expand **Domain Name System (query)**, then click the matching response
packet and expand **Domain Name System (response) → Answers** to show the
resolved address — screenshot both for your report.

---

## 2. Q2 Feature 1 — Adaptive LZ77 Compression

### 2.1 Basic round trip (compression correctly declines on a short message)

```bash
./server 12399 --compress &
SPID=$!
sleep 1
./client localhost 12399 --compress
kill -9 $SPID
```
**Expected:**
```
Connected to localhost:12399 (compression mode)
Original size : 23 bytes
Compressed size : 29 bytes (not used - raw was smaller)
Bytes actually sent : 36 (header 13 + payload)
Round-trip time : 0.046 ms
Echo verification : MATCH
```
The default message is too short for LZ77 to help — this is the adaptive
logic correctly choosing not to compress, not a bug.

### 2.2 Compression actually kicking in (larger, repetitive input)

```bash
./server 12399 --compress &
SPID=$!
sleep 1
python3 -c "print('the quick brown fox jumps over the lazy dog. ' * 50, end='')" > /tmp/rep.txt
./client localhost 12399 --compress --file /tmp/rep.txt --type repetitive
kill -9 $SPID
```
**Expected:** `Compressed size : ... (used)` with a large drop in bytes
actually sent.

### 2.3 Automated benchmark

```bash
python3 benchmark_compression.py
```
Generates repetitive/text/random test messages at several sizes, drives the
client/server through all of them, and writes:
```
q2_benchmark_output/
├── results.csv
├── bytes_on_wire.png
├── compression_ratio.png
├── percent_reduction_by_type.png
└── rtt.png
```
Useful flags: `--port <n>` (default: auto-picks a free port),
`--outdir <path>` (default: `./q2_benchmark_output`).

To just re-plot an existing `results.csv` without re-running anything:
```bash
python3 benchmark_compression.py --skip-run --outdir q2_benchmark_output
```

---

## 3. Q2 Feature 2 — Concurrent Multi-Client Server

### 3.1 Demonstration with multiple simultaneous clients

```bash
./server 12399 --multi &
SPID=$!
sleep 1

./client localhost 12399 --count 10 --size 200 --label clientA &
P1=$!
./client localhost 12399 --count 10 --size 200 --label clientB &
P2=$!
wait $P1 $P2

kill -9 $SPID
```
Each client should report `completed=10/10 ... success=YES`.

**To see the interleaving that proves genuine concurrency**, run the server
in the foreground in its own terminal instead of backgrounding it (or with
`stdbuf -oL -eL ./server 12399 --multi > server.log 2>&1 &` so lines are
flushed immediately), then run the two clients above from a second
terminal. Watch for log lines from **different client addresses/thread
IDs interleaved** — e.g. client A's message 5 logged before client B's
message 3 finishes — which is only possible if both connections are being
serviced at the same time by different threads. A sequential server
(`./server 12399`, no `--multi`) would always finish one client's entire
conversation before any line from the next client appears.

### 3.2 Combining concurrency with compression

```bash
./server 12399 --multi --compress &
SPID=$!
sleep 1
./client localhost 12399 --count 10 --size 2000 --compress --label combo &
wait
kill -9 $SPID
```

### 3.3 Automated benchmark

```bash
python3 benchmark_feature2.py
```
Runs the full sequential-vs-concurrent comparison (client counts
1/2/4/8/16 by default, 100 messages of 256 bytes each, 3 repetitions),
prints a headline speedup number, and writes:
```
q2_feature2/
├── results/
│   ├── raw_results.csv
│   └── summary.csv
└── plots/
    ├── A_completion_time.png
    ├── B_throughput_msgs.png
    ├── C_throughput_bytes.png
    ├── D_speedup.png
    └── E_client_completion_time.png
```
Useful flags: `--client-counts 1,2,4,8,16` (comma-separated, configurable),
`--messages-per-client <n>`, `--message-size <bytes>`, `--repetitions <n>`,
`--port <n>`, `--host <name>`, `--compress` (also benchmark
concurrency+compression together instead of concurrency alone).

To just re-plot existing results without re-running trials:
```bash
python3 benchmark_feature2.py --plot-only
```

---

## Notes

- **Every section above is independent and can be re-run at any time** in
  any order; each `make clean && make` produces fresh binaries.
- If a server is killed with `kill -9`, its stdout may not show the final
  buffered lines (normal `stdio` buffering, not a bug) — for live
  observation while a server is running, either run it in its own terminal
  instead of backgrounding it, or prefix the command with `stdbuf -oL -eL`
  to force line-buffering into a redirected log file.
- If a port is reported as already in use, either wait a few seconds (TCP
  `TIME_WAIT`) or pick a different port number in the commands above.
- To find and stop any leftover server process instead of relying on shell
  job numbers (which can silently desync if a job exits before you kill
  it):
  ```bash
  pkill -9 -f './server'
  ```
- All `$SPID`/`$P1`/`$P2`-style PID capture (`$!`) is used deliberately
  instead of `kill %1` throughout this README, since job numbers can
  desync from reality if a job has already exited — capturing the PID
  directly is reliable regardless.
