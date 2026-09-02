# CS3530 Computer Networks — Assignment 1

TCP Echo Client/Server in C (POSIX sockets), with hostname resolution (Q1),
adaptive LZ77 compression (Q2 Feature 1), and a concurrent multi-client
server (Q2 Feature 2). All commands below are plain Linux/WSL2 bash — no
Windows-specific tools are used anywhere.

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
- Python 3 with `matplotlib`

Check/install Python's dependency if needed:
```bash
python3 -c "import matplotlib" 2>/dev/null && echo "matplotlib OK" || pip install matplotlib --break-system-packages
```

## 1. Build

```bash
make clean
make
```
This produces two binaries, `server` and `client`, with no warnings.

---

## 2. Run Q1 — hostname resolution + plain echo

**Terminal / commands:**
```bash
./server 12399 &
SPID=$!
sleep 1
./client localhost 12399
kill -9 $SPID
```

**Expected output (client):**
```
Trying address: 127.0.0.1
Connected to localhost:12399
Sent: Hello from echo client!
Received echo: Hello from echo client!
```

Q1 has no separate benchmark script — its correctness is demonstrated
directly by the round trip above (and, for the DNS-resolution requirement
specifically, by capturing `getaddrinfo()`'s query/response with
`tcpdump`/Wireshark against a real hostname, as covered separately in the
assignment report).

---

## 3. Run Q2 Feature 1 — adaptive LZ77 compression

**Terminal / commands:**
```bash
./server 12399 --compress &
SPID=$!
sleep 1
./client localhost 12399 --compress
kill -9 $SPID
```

**Expected output (client):**
```
Connected to localhost:12399 (compression mode)
Original size : 23 bytes
Compressed size : 29 bytes (not used - raw was smaller)
Bytes actually sent : 36 (header 13 + payload)
Round-trip time : 0.046 ms
Echo verification : MATCH
```
(The default demo message is short enough that compression correctly
declines to activate — this is the adaptive logic working as intended, not
an error. See the next section for compressible input.)

**Try it with a larger, compressible input:**
```bash
./server 12399 --compress &
SPID=$!
sleep 1
python3 -c "print('the quick brown fox jumps over the lazy dog. ' * 50, end='')" > /tmp/rep.txt
./client localhost 12399 --compress --file /tmp/rep.txt --type repetitive
kill -9 $SPID
```
This should show `Compressed size : ... (used)` with a large reduction in
bytes actually sent.

### 3a. Q2 Feature 1 benchmark

```bash
python3 benchmark_compression.py
```
This builds test messages (repetitive / text / random text at several
sizes), starts `./server ... --compress` itself, drives `./client` through
every combination, and writes:
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

To just re-plot an existing `results.csv` without re-running the client/server:
```bash
python3 benchmark_compression.py --skip-run --outdir q2_benchmark_output
```

---

## 4. Run Q2 Feature 2 — concurrent multi-client server

**Demonstration with multiple simultaneous clients:**
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

**What to look for:** each client reports `completed=10/10 ... success=YES`.
More importantly, if you drop the `> /dev/null` redirection (or check the
server's own stdout, e.g. by *not* backgrounding it and instead running it
in a separate terminal), you'll see log lines tagged with different thread
IDs and client addresses **interleaved** — e.g. client A's message 5 logged
before client B's message 3 finishes — which is only possible if both
connections are being serviced at the same time by different threads. A
sequential (`./server 12399`, no `--multi`) server would always finish one
client's entire conversation before any line from the next client appears.

**Combining with compression** (both features together):
```bash
./server 12399 --multi --compress &
SPID=$!
sleep 1
./client localhost 12399 --count 10 --size 2000 --compress --label combo &
wait
kill -9 $SPID
```

### 4a. Q2 Feature 2 benchmark

```bash
python3 benchmark_feature2.py
```
This runs the full sequential-vs-concurrent comparison (client counts
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

- **Every mode above is independent and can be re-run at any time** in any
  order; each `make clean && make` produces fresh binaries.
- If a server is killed with `kill -9`, its stdout may not show the final
  buffered lines (this is normal `stdio` buffering, not a bug) — for live
  observation while a server is running, run it in its own terminal instead
  of backgrounding it, or prefix the command with `stdbuf -oL -eL` to force
  line-buffering into a redirected log file.
- If a port is reported as already in use, either wait a few seconds (TCP
  `TIME_WAIT`) or simply pick a different port number in the commands above.
- To find and stop any leftover server process instead of relying on job
  numbers:
  ```bash
  pkill -9 -f './server'
  ```
