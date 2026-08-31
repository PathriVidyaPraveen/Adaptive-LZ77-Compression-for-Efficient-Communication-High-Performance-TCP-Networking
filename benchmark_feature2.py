#!/usr/bin/env python3
"""
benchmark_feature2.py
=========================
Single-file automated benchmark AND plotting tool comparing the SEQUENTIAL
baseline server (./server <port>) against the CONCURRENT Feature 2 server
(./server <port> --multi).

This script lives in the project ROOT (alongside server.c/client.c/the
compiled `server`/`client` binaries), but writes all of its output into a
`q2_feature2/` subfolder it creates next to itself:

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

This script does NOT implement any networking itself - it only launches and
times the existing C `server` and `client` binaries as subprocesses. All
socket programming and concurrency (pthreads) live entirely in the C code;
this script's job is orchestration, measurement, and visualization.

For every (repetition, server_mode, client_count) combination:
  1. Start the C server in the requested mode.
  2. Launch `client_count` instances of the C client CONCURRENTLY, each
     sending `--messages-per-client` messages of `--message-size` bytes over
     its own persistent connection, verifying every echo itself.
  3. Wait for all clients to finish; record wall-clock time for the whole
     batch.
  4. Each client writes its own one-row CSV summary (to a unique temp path,
     so concurrent clients never write to the same file - avoiding any
     multi-process file-write race). This script reads those rows back.
  5. Stop the server.

Then it plots the results directly - no separate plotting script needed.

Results and plots are written into ./q2_feature2/ (created automatically),
next to wherever this script itself is run from - see the layout above.

Usage:
    python3 benchmark_feature2.py
    python3 benchmark_feature2.py --client-counts 1,2,4,8,16,32 --repetitions 5
    python3 benchmark_feature2.py --compress    # also test --multi + --compress together
    python3 benchmark_feature2.py --plot-only   # skip re-running trials, just re-plot
                                                 # the existing q2_feature2/results/ CSVs
"""

import argparse
import csv
import os
import statistics
import subprocess
import sys
import tempfile
import time

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required: pip install matplotlib --break-system-packages")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


# =============================================================================
# Argument parsing
# =============================================================================

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=15500)
    p.add_argument("--client-counts", default="1,2,4,8,16",
                    help="Comma-separated list of concurrent client counts to test")
    p.add_argument("--messages-per-client", type=int, default=100)
    p.add_argument("--message-size", type=int, default=256, help="Bytes per message")
    p.add_argument("--repetitions", type=int, default=3,
                    help="How many times to repeat each trial, to reduce noise")
    p.add_argument("--server-path", default=os.path.join(SCRIPT_DIR, "server"))
    p.add_argument("--client-path", default=os.path.join(SCRIPT_DIR, "client"))
    p.add_argument("--results-dir", default=os.path.join(SCRIPT_DIR, "q2_feature2", "results"))
    p.add_argument("--plots-dir", default=os.path.join(SCRIPT_DIR, "q2_feature2", "plots"))
    p.add_argument("--compress", action="store_true",
                    help="Also pass --compress to server/client, to benchmark "
                         "concurrency+compression together instead of concurrency alone")
    p.add_argument("--client-timeout", type=float, default=30.0,
                    help="Seconds to wait for a single client before giving up on it")
    p.add_argument("--plot-only", action="store_true",
                    help="Skip running trials; just (re)generate plots from the "
                         "existing results/summary.csv")
    return p.parse_args()


# =============================================================================
# Running trials (driving the C server/client binaries as subprocesses)
# =============================================================================

def start_server(server_path, port, concurrent, compress, log_path):
    cmd = [server_path, str(port)]
    if concurrent:
        cmd.append("--multi")
    if compress:
        cmd.append("--compress")
    log_f = open(log_path, "w")
    proc = subprocess.Popen(cmd, stdout=log_f, stderr=subprocess.STDOUT)
    return proc, log_f


def stop_server(proc, log_f):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    log_f.close()


def run_trial(args, server_mode, n_clients, rep, tmpdir):
    """Runs one (server_mode, n_clients) trial and returns a dict of results,
    or None if the trial could not be completed (e.g. server failed to bind)."""
    concurrent = (server_mode == "concurrent")
    server_log_path = os.path.join(tmpdir, f"server_{server_mode}_{n_clients}_{rep}.log")
    server_proc, server_log_f = start_server(
        args.server_path, args.port, concurrent, args.compress, server_log_path)
    time.sleep(0.5)

    if server_proc.poll() is not None:
        stop_server(server_proc, server_log_f)
        with open(server_log_path) as f:
            print(f"  [WARN] server failed to start for {server_mode}/n={n_clients}/rep={rep}:")
            print("    " + f.read().replace("\n", "\n    "))
        return None

    client_log_paths = [
        os.path.join(tmpdir, f"client_{server_mode}_{n_clients}_{rep}_{i}.csv")
        for i in range(n_clients)
    ]

    client_cmd_base = [args.client_path, args.host, str(args.port),
                        "--count", str(args.messages_per_client),
                        "--size", str(args.message_size)]
    if args.compress:
        client_cmd_base.append("--compress")

    procs = []
    t_start = time.perf_counter()
    for i in range(n_clients):
        label = f"{server_mode}_n{n_clients}_r{rep}_c{i}"
        cmd = client_cmd_base + ["--label", label, "--log", client_log_paths[i]]
        procs.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    for proc in procs:
        try:
            proc.wait(timeout=args.client_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
    t_end = time.perf_counter()

    stop_server(server_proc, server_log_f)

    total_wall_time_s = t_end - t_start

    per_client_rows = []
    for path in client_log_paths:
        if not os.path.exists(path):
            per_client_rows.append(None)
            continue
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            row = next(reader, None)
            per_client_rows.append(row)

    successful = [r for r in per_client_rows if r is not None and r["success"] == "1"]
    failed_count = n_clients - len(successful)

    if not successful:
        print(f"  [WARN] all {n_clients} clients failed for {server_mode}/n={n_clients}/rep={rep}")
        return {
            "repetition": rep, "server_mode": server_mode, "n_clients": n_clients,
            "messages_per_client": args.messages_per_client, "message_size": args.message_size,
            "total_wall_time_s": total_wall_time_s,
            "avg_client_time_ms": float("nan"), "median_client_time_ms": float("nan"),
            "max_client_time_ms": float("nan"),
            "total_messages": 0, "msgs_per_sec": 0.0, "bytes_per_sec": 0.0,
            "successful_clients": 0, "failed_clients": failed_count,
        }

    client_times_ms = [float(r["total_time_ms"]) for r in successful]
    total_messages = sum(int(r["count_completed"]) for r in successful)
    total_bytes = sum(int(r["count_completed"]) * int(r["message_size"]) for r in successful)

    return {
        "repetition": rep,
        "server_mode": server_mode,
        "n_clients": n_clients,
        "messages_per_client": args.messages_per_client,
        "message_size": args.message_size,
        "total_wall_time_s": total_wall_time_s,
        "avg_client_time_ms": statistics.mean(client_times_ms),
        "median_client_time_ms": statistics.median(client_times_ms),
        "max_client_time_ms": max(client_times_ms),
        "total_messages": total_messages,
        "msgs_per_sec": total_messages / total_wall_time_s if total_wall_time_s > 0 else 0.0,
        "bytes_per_sec": total_bytes / total_wall_time_s if total_wall_time_s > 0 else 0.0,
        "successful_clients": len(successful),
        "failed_clients": failed_count,
    }


# =============================================================================
# CSV output
# =============================================================================

def write_raw_csv(rows, path):
    if not rows:
        return
    fieldnames = list(rows[0].keys())
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_summary_csv(rows, path):
    groups = {}
    for r in rows:
        key = (r["server_mode"], r["n_clients"])
        groups.setdefault(key, []).append(r)

    summary_rows = []
    for (server_mode, n_clients), group in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        def mean_of(field):
            vals = [g[field] for g in group if g[field] == g[field]]
            return statistics.mean(vals) if vals else float("nan")

        summary_rows.append({
            "server_mode": server_mode,
            "n_clients": n_clients,
            "messages_per_client": group[0]["messages_per_client"],
            "message_size": group[0]["message_size"],
            "repetitions": len(group),
            "avg_total_wall_time_s": mean_of("total_wall_time_s"),
            "avg_client_time_ms": mean_of("avg_client_time_ms"),
            "median_client_time_ms": mean_of("median_client_time_ms"),
            "max_client_time_ms": mean_of("max_client_time_ms"),
            "avg_msgs_per_sec": mean_of("msgs_per_sec"),
            "avg_bytes_per_sec": mean_of("bytes_per_sec"),
            "total_successful_clients": sum(g["successful_clients"] for g in group),
            "total_failed_clients": sum(g["failed_clients"] for g in group),
        })

    by_n = {}
    for r in summary_rows:
        by_n.setdefault(r["n_clients"], {})[r["server_mode"]] = r
    for r in summary_rows:
        pair = by_n.get(r["n_clients"], {})
        if "sequential" in pair and "concurrent" in pair and pair["concurrent"]["avg_total_wall_time_s"] > 0:
            r["speedup"] = pair["sequential"]["avg_total_wall_time_s"] / pair["concurrent"]["avg_total_wall_time_s"]
        else:
            r["speedup"] = float("nan")

    if not summary_rows:
        return
    fieldnames = list(summary_rows[0].keys())
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(summary_rows)


def load_summary(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["n_clients"] = int(row["n_clients"])
            for field in ["avg_total_wall_time_s", "avg_client_time_ms",
                          "median_client_time_ms", "max_client_time_ms",
                          "avg_msgs_per_sec", "avg_bytes_per_sec", "speedup"]:
                try:
                    row[field] = float(row[field])
                except ValueError:
                    row[field] = float("nan")
            rows.append(row)
    return rows


# =============================================================================
# Plotting
# =============================================================================

COLORS = {"sequential": "#e76f51", "concurrent": "#2a9d8f"}
MARKERS = {"sequential": "o", "concurrent": "s"}


def by_mode(rows):
    modes = {}
    for r in rows:
        modes.setdefault(r["server_mode"], []).append(r)
    for m in modes:
        modes[m].sort(key=lambda r: r["n_clients"])
    return modes


def plot_completion_time(rows, outdir):
    """A. PRIMARY plot: total workload completion time vs number of clients."""
    modes = by_mode(rows)
    fig, ax = plt.subplots(figsize=(8, 6))
    for mode, entries in modes.items():
        x = [e["n_clients"] for e in entries]
        y = [e["avg_total_wall_time_s"] * 1000 for e in entries]
        ax.plot(x, y, marker=MARKERS.get(mode, "o"), color=COLORS.get(mode),
                label=mode, linewidth=2, markersize=8)
    ax.set_xlabel("Number of concurrent clients")
    ax.set_ylabel("Total workload completion time (ms)")
    ax.set_title("Completion Time: Sequential vs Concurrent Server")
    ax.set_yscale("log")
    ax.legend(fontsize=10)
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    path = os.path.join(outdir, "A_completion_time.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_throughput_msgs(rows, outdir):
    """B. Messages/sec vs number of clients."""
    modes = by_mode(rows)
    fig, ax = plt.subplots(figsize=(8, 6))
    for mode, entries in modes.items():
        x = [e["n_clients"] for e in entries]
        y = [e["avg_msgs_per_sec"] for e in entries]
        ax.plot(x, y, marker=MARKERS.get(mode, "o"), color=COLORS.get(mode),
                label=mode, linewidth=2, markersize=8)
    ax.set_xlabel("Number of concurrent clients")
    ax.set_ylabel("Aggregate throughput (messages/sec)")
    ax.set_title("Message Throughput: Sequential vs Concurrent Server")
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    path = os.path.join(outdir, "B_throughput_msgs.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_throughput_bytes(rows, outdir):
    """C. Application-layer bytes/sec vs number of clients."""
    modes = by_mode(rows)
    fig, ax = plt.subplots(figsize=(8, 6))
    for mode, entries in modes.items():
        x = [e["n_clients"] for e in entries]
        y = [e["avg_bytes_per_sec"] / 1024.0 for e in entries]
        ax.plot(x, y, marker=MARKERS.get(mode, "o"), color=COLORS.get(mode),
                label=mode, linewidth=2, markersize=8)
    ax.set_xlabel("Number of concurrent clients")
    ax.set_ylabel("Application-layer throughput (KB/sec)")
    ax.set_title("Byte Throughput: Sequential vs Concurrent Server")
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    path = os.path.join(outdir, "C_throughput_bytes.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_speedup(rows, outdir):
    """D. Speedup = T_sequential / T_concurrent vs number of clients."""
    modes = by_mode(rows)
    entries = modes.get("concurrent") or modes.get("sequential") or []
    x = [e["n_clients"] for e in entries]
    y = [e["speedup"] for e in entries]

    fig, ax = plt.subplots(figsize=(8, 6))
    ax.plot(x, y, marker="D", color="#264653", linewidth=2, markersize=8,
            label="measured speedup")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=1.5,
               label="speedup = 1 (no benefit)")
    ax.set_xlabel("Number of concurrent clients")
    ax.set_ylabel(r"Speedup  ($T_{sequential} / T_{concurrent}$)")
    ax.set_title("Concurrency Speedup vs Number of Clients")
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    for xi, yi in zip(x, y):
        if yi == yi:
            ax.annotate(f"{yi:.2f}x", (xi, yi), textcoords="offset points",
                        xytext=(0, 8), ha="center", fontsize=9)
    fig.tight_layout()
    path = os.path.join(outdir, "D_speedup.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def plot_client_completion_time(rows, outdir):
    """E. Average/median individual client completion time vs number of clients."""
    modes = by_mode(rows)
    fig, axes = plt.subplots(1, 2, figsize=(13, 6), sharey=True)

    for ax, metric, title in zip(
        axes,
        ["avg_client_time_ms", "median_client_time_ms"],
        ["Average client completion time", "Median client completion time"],
    ):
        for mode, entries in modes.items():
            x = [e["n_clients"] for e in entries]
            y = [e[metric] for e in entries]
            ax.plot(x, y, marker=MARKERS.get(mode, "o"), color=COLORS.get(mode),
                    label=mode, linewidth=2, markersize=8)
        ax.set_xlabel("Number of concurrent clients")
        ax.set_title(title)
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)

    axes[0].set_ylabel("Per-client completion time (ms)")
    fig.tight_layout()
    path = os.path.join(outdir, "E_client_completion_time.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"Saved {path}")


def print_headline(rows):
    modes = by_mode(rows)
    seq = {e["n_clients"]: e for e in modes.get("sequential", [])}
    con = {e["n_clients"]: e for e in modes.get("concurrent", [])}
    common_n = sorted(set(seq) & set(con))
    if not common_n:
        return
    max_n = common_n[-1]
    s, c = seq[max_n]["avg_total_wall_time_s"], con[max_n]["avg_total_wall_time_s"]
    print(f"\nHeadline result at n_clients={max_n}:")
    print(f"  Sequential : {s*1000:.1f} ms")
    print(f"  Concurrent : {c*1000:.1f} ms")
    if c > 0:
        print(f"  Speedup    : {s/c:.2f}x")


def generate_plots(summary_path, plots_dir):
    if not os.path.exists(summary_path):
        sys.exit(f"{summary_path} not found - run without --plot-only first")

    os.makedirs(plots_dir, exist_ok=True)
    rows = load_summary(summary_path)
    if not rows:
        sys.exit("summary.csv is empty - nothing to plot")

    plot_completion_time(rows, plots_dir)
    plot_throughput_msgs(rows, plots_dir)
    plot_throughput_bytes(rows, plots_dir)
    plot_speedup(rows, plots_dir)
    plot_client_completion_time(rows, plots_dir)

    print_headline(rows)
    print(f"\nAll plots saved in: {os.path.abspath(plots_dir)}")


# =============================================================================
# Main
# =============================================================================

def main():
    args = parse_args()
    summary_path = os.path.join(args.results_dir, "summary.csv")

    if args.plot_only:
        generate_plots(summary_path, args.plots_dir)
        return

    if not os.path.isfile(args.server_path):
        sys.exit(f"server binary not found at {args.server_path} - build it first (make)")
    if not os.path.isfile(args.client_path):
        sys.exit(f"client binary not found at {args.client_path} - build it first (make)")

    client_counts = [int(x) for x in args.client_counts.split(",")]
    os.makedirs(args.results_dir, exist_ok=True)

    all_rows = []
    with tempfile.TemporaryDirectory(prefix="q2f2_bench_") as tmpdir:
        total_trials = args.repetitions * 2 * len(client_counts)
        done = 0
        for rep in range(args.repetitions):
            for server_mode in ["sequential", "concurrent"]:
                for n_clients in client_counts:
                    done += 1
                    print(f"[{done}/{total_trials}] rep={rep} mode={server_mode:10s} "
                          f"n_clients={n_clients:3d} ...", end=" ", flush=True)
                    result = run_trial(args, server_mode, n_clients, rep, tmpdir)
                    if result is None:
                        print("SKIPPED (server failed to start)")
                        continue
                    all_rows.append(result)
                    print(f"wall_time={result['total_wall_time_s']:.3f}s "
                          f"success={result['successful_clients']}/{n_clients}")

    raw_path = os.path.join(args.results_dir, "raw_results.csv")
    write_raw_csv(all_rows, raw_path)
    write_summary_csv(all_rows, summary_path)

    print(f"\nRaw results:     {raw_path}")
    print(f"Summary results: {summary_path}")

    generate_plots(summary_path, args.plots_dir)


if __name__ == "__main__":
    main()