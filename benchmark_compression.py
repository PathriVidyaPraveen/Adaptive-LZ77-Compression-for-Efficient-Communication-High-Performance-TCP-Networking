#!/usr/bin/env python3
"""
benchmark_compression.py
=========================
Automated Communication-Efficiency Analysis for Q2 (Adaptive Application-
Layer LZ77 Compression).

What this does:
  1. Generates test message files of several TYPES and SIZES:
       - "repetitive"  : a short phrase repeated many times (highly compressible)
       - "text"        : original English prose repeated/tiled (moderately compressible)
       - "random"      : os.urandom() bytes (incompressible - worst case)
  2. Starts the Q2 compression-mode server (./server <port> --compress).
  3. Runs the Q2 client (./client <host> <port> --compress --file ... --log ...)
     once per (type, size) combination, which itself performs the adaptive
     compress-or-raw decision and logs one CSV row per run.
  4. Loads the resulting CSV and produces plots quantifying the improvement
     over the Q1 baseline (which never compresses and never adds a header,
     so its "bytes on the wire" is simply the message size).

Usage:
    python3 benchmark_compression.py
    python3 benchmark_compression.py --port 15000 --outdir results/

Requires: matplotlib (pip install matplotlib --break-system-packages, if needed)
Requires: the `server` and `client` binaries already built (`make`) in the
          same directory as this script, or pass --client-path/--server-path.
"""

import argparse
import csv
import os
import shutil
import socket
import subprocess
import sys
import time

try:
    import matplotlib
    matplotlib.use("Agg")  # no display needed - just save PNG files
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required: pip install matplotlib --break-system-packages")


MESSAGE_TYPES = ["repetitive", "text", "random"]
MESSAGE_SIZES = [50, 500, 5000, 50000]  # bytes

REPETITIVE_PHRASE = "the quick brown fox jumps over the lazy dog. "
TEXT_PARAGRAPH = (
    "Computer networks rely on layered protocols to move data reliably "
    "between hosts, and each layer solves a distinct problem: physical "
    "transmission, addressing and routing, reliable delivery, and finally "
    "the application's own message format. Application-layer compression "
    "is one small example of that last layer at work. "
)


def find_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("localhost", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_test_file(path, msg_type, size):
    if msg_type == "repetitive":
        reps = size // len(REPETITIVE_PHRASE) + 1
        data = (REPETITIVE_PHRASE * reps)[:size].encode("utf-8")
    elif msg_type == "text":
        reps = size // len(TEXT_PARAGRAPH) + 1
        data = (TEXT_PARAGRAPH * reps)[:size].encode("utf-8")
    elif msg_type == "random":
        data = os.urandom(size)
    else:
        raise ValueError(f"unknown message type: {msg_type}")
    with open(path, "wb") as f:
        f.write(data)


def run_benchmark(args):
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)
    files_dir = os.path.join(outdir, "test_messages")
    os.makedirs(files_dir, exist_ok=True)
    csv_path = os.path.join(outdir, "results.csv")

    # Start from a clean CSV each run so results.csv always reflects this run only.
    if os.path.exists(csv_path):
        os.remove(csv_path)

    port = args.port or find_free_port()

    print(f"Starting compression-mode server on port {port} ...")
    server_log = open(os.path.join(outdir, "server_benchmark.log"), "w")
    server_proc = subprocess.Popen(
        [args.server_path, str(port), "--compress"],
        stdout=server_log, stderr=subprocess.STDOUT,
        # line-buffer the server's own stdio so the log is useful if inspected
        env={**os.environ, "STDBUF_LINE": "1"},
    )
    time.sleep(0.5)

    try:
        total = len(MESSAGE_TYPES) * len(MESSAGE_SIZES)
        done = 0
        for msg_type in MESSAGE_TYPES:
            for size in MESSAGE_SIZES:
                fname = os.path.join(files_dir, f"{msg_type}_{size}.bin")
                make_test_file(fname, msg_type, size)

                done += 1
                print(f"[{done}/{total}] {msg_type:10s} size={size:>6d} bytes ...", end=" ", flush=True)

                result = subprocess.run(
                    [args.client_path, args.host, str(port), "--compress",
                     "--file", fname, "--type", msg_type, "--log", csv_path],
                    capture_output=True, text=True, timeout=30,
                )
                if result.returncode != 0:
                    print("FAILED")
                    print(result.stdout)
                    print(result.stderr)
                else:
                    print("ok")
    finally:
        server_proc.terminate()
        try:
            server_proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server_proc.kill()
        server_log.close()

    print(f"\nAll runs complete. Raw results: {csv_path}")
    return csv_path


def load_results(csv_path):
    rows = []
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["message_size"] = int(row["message_size"])
            row["q1_bytes_sent"] = int(row["q1_bytes_sent"])
            row["q2_bytes_sent"] = int(row["q2_bytes_sent"])
            row["compressed_size"] = int(row["compressed_size"])
            row["compression_used"] = int(row["compression_used"])
            row["compression_ratio"] = float(row["compression_ratio"])
            row["percent_reduction"] = float(row["percent_reduction"])
            row["rtt_ms"] = float(row["rtt_ms"])
            rows.append(row)
    return rows


def by_type(rows):
    types = {}
    for r in rows:
        types.setdefault(r["type"], []).append(r)
    for t in types:
        types[t].sort(key=lambda r: r["message_size"])
    return types


def plot_bytes_on_wire(rows, outdir):
    """Q1 baseline (raw bytes, no header) vs Q2 actual bytes sent, per type,
    across message size. This is the headline 'does compression help on the
    wire' plot - and for random data it should visibly show Q2 losing
    slightly to Q1 due to header overhead, which is the honest result."""
    types = by_type(rows)
    fig, axes = plt.subplots(1, len(types), figsize=(6 * len(types), 5), sharey=False)
    if len(types) == 1:
        axes = [axes]

    for ax, (t, entries) in zip(axes, types.items()):
        sizes = [e["message_size"] for e in entries]
        q1 = [e["q1_bytes_sent"] for e in entries]
        q2 = [e["q2_bytes_sent"] for e in entries]
        ax.plot(sizes, q1, marker="o", label="Q1 baseline (no compression, no header)")
        ax.plot(sizes, q2, marker="s", label="Q2 (adaptive compression + header)")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("Message size (bytes)")
        ax.set_ylabel("Bytes actually sent on wire")
        ax.set_title(f"Bytes on wire: {t}")
        ax.legend(fontsize=8)
        ax.grid(True, which="both", alpha=0.3)

    fig.tight_layout()
    path = os.path.join(outdir, "bytes_on_wire.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_compression_ratio(rows, outdir):
    """Compressed size / original size, per type across message size.
    Ratio < 1 means compression shrank the data; > 1 means it grew (random)."""
    types = by_type(rows)
    fig, ax = plt.subplots(figsize=(7, 5))
    for t, entries in types.items():
        sizes = [e["message_size"] for e in entries]
        ratios = [e["compression_ratio"] for e in entries]
        ax.plot(sizes, ratios, marker="o", label=t)
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=1, label="break-even (ratio = 1)")
    ax.set_xscale("log")
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Compression ratio (compressed / original)")
    ax.set_title("LZ77 compression ratio by message type and size")
    ax.legend(fontsize=8)
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    path = os.path.join(outdir, "compression_ratio.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_percent_reduction_bar(rows, outdir):
    """Average percent reduction in bytes-on-wire per type (bar chart) -
    the single 'headline number' summary for the report."""
    types = by_type(rows)
    labels = list(types.keys())
    avg_reduction = [
        sum(e["percent_reduction"] for e in types[t]) / len(types[t])
        for t in labels
    ]

    fig, ax = plt.subplots(figsize=(6, 5))
    colors = ["#2a9d8f" if v >= 0 else "#e76f51" for v in avg_reduction]
    ax.bar(labels, avg_reduction, color=colors)
    ax.axhline(0, color="black", linewidth=0.8)
    ax.set_ylabel("Average % reduction in bytes on wire (Q2 vs Q1 baseline)")
    ax.set_title("Communication-efficiency improvement by message type")
    for i, v in enumerate(avg_reduction):
        ax.text(i, v + (1 if v >= 0 else -3), f"{v:.1f}%", ha="center", fontsize=9)
    fig.tight_layout()
    path = os.path.join(outdir, "percent_reduction_by_type.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_rtt(rows, outdir):
    """Round-trip time by message size and type - shows the computational
    cost side of compression, not just the bytes-saved side."""
    types = by_type(rows)
    fig, ax = plt.subplots(figsize=(7, 5))
    for t, entries in types.items():
        sizes = [e["message_size"] for e in entries]
        rtts = [e["rtt_ms"] for e in entries]
        ax.plot(sizes, rtts, marker="o", label=t)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Round-trip time (ms)")
    ax.set_title("Round-trip time by message type and size (loopback)")
    ax.legend(fontsize=8)
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    path = os.path.join(outdir, "rtt.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def print_summary_table(rows):
    types = by_type(rows)
    print("\n" + "=" * 78)
    print("SUMMARY")
    print("=" * 78)
    header = f"{'type':<12}{'size':>8}{'orig':>8}{'compr':>8}{'used':>6}{'q2_sent':>9}{'%reduc':>9}{'rtt_ms':>9}"
    print(header)
    print("-" * len(header))
    for t, entries in types.items():
        for e in entries:
            print(f"{t:<12}{e['message_size']:>8}{e['message_size']:>8}"
                  f"{e['compressed_size']:>8}{e['compression_used']:>6}"
                  f"{e['q2_bytes_sent']:>9}{e['percent_reduction']:>9.2f}"
                  f"{e['rtt_ms']:>9.3f}")
    print("=" * 78)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=None,
                         help="Port to use (default: pick a free one automatically)")
    parser.add_argument("--server-path", default="./server")
    parser.add_argument("--client-path", default="./client")
    parser.add_argument("--outdir", default="./q2_benchmark_output")
    parser.add_argument("--skip-run", action="store_true",
                         help="Skip running client/server; just re-plot an existing results.csv in --outdir")
    args = parser.parse_args()

    if not shutil.which(args.server_path) and not os.path.isfile(args.server_path):
        sys.exit(f"server binary not found at {args.server_path} - build it first (make)")
    if not shutil.which(args.client_path) and not os.path.isfile(args.client_path):
        sys.exit(f"client binary not found at {args.client_path} - build it first (make)")

    if args.skip_run:
        csv_path = os.path.join(args.outdir, "results.csv")
        if not os.path.exists(csv_path):
            sys.exit(f"--skip-run given but {csv_path} does not exist")
    else:
        csv_path = run_benchmark(args)

    rows = load_results(csv_path)
    if not rows:
        sys.exit("No results to plot (results.csv is empty)")

    print_summary_table(rows)

    plot_bytes_on_wire(rows, args.outdir)
    plot_compression_ratio(rows, args.outdir)
    plot_percent_reduction_bar(rows, args.outdir)
    plot_rtt(rows, args.outdir)

    print(f"\nAll plots and results.csv saved in: {os.path.abspath(args.outdir)}")


if __name__ == "__main__":
    main()
