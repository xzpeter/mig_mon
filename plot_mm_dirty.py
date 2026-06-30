#!/usr/bin/env python3
"""Parse mig_mon mm_dirty output and plot metrics over time."""

import argparse
import re
import sys
import matplotlib.pyplot as plt


def parse(lines):
    data = {"time": [], "dirty_rate": [], "load": [],
            "p50": [], "p99": [], "max": []}
    t = 0
    pat = re.compile(
        r"dirty-rate:\s*([\d.]+)\s*\(MB/s\),\s*"
        r"time \(ms\):\s*(\d+),\s*"
        r"load:\s*([\d.]+)%"
        r"(?:,\s*lat \(us\):\s*p50=(\d+)\s+p99=(\d+)\s+max=(\d+))?"
    )
    for line in lines:
        m = pat.search(line)
        if not m:
            continue
        duration_ms = int(m.group(2))
        t += duration_ms / 1000.0
        data["time"].append(t)
        data["dirty_rate"].append(float(m.group(1)))
        data["load"].append(float(m.group(3)))
        if m.group(4) is not None:
            data["p50"].append(int(m.group(4)))
            data["p99"].append(int(m.group(5)))
            data["max"].append(int(m.group(6)))
    return data


def plot(data, output):
    has_lat = len(data["p50"]) > 0
    nplots = 3 if has_lat else 2
    fig, axes = plt.subplots(nplots, 1, figsize=(12, 3 * nplots), sharex=True)

    axes[0].plot(data["time"], data["dirty_rate"], "b-")
    axes[0].set_ylabel("Dirty rate (MB/s)")
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(data["time"], data["load"], "g-")
    axes[1].set_ylabel("CPU load (%)")
    axes[1].grid(True, alpha=0.3)

    if has_lat:
        t = data["time"]
        axes[2].plot(t, data["p50"], label="p50")
        axes[2].plot(t, data["p99"], label="p99")
        axes[2].plot(t, data["max"], label="max", alpha=0.7)
        axes[2].set_ylabel("Latency (us)")
        axes[2].set_yscale("log")
        axes[2].legend()
        axes[2].grid(True, alpha=0.3)

    axes[-1].set_xlabel("Time (s)")
    fig.suptitle("mm_dirty workload")
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150)
        print(f"Saved to {output}")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", nargs="?", default="-",
                    help="mm_dirty output file (default: stdin)")
    ap.add_argument("-o", "--output",
                    help="Save plot to file (png/pdf/svg) instead of showing")
    args = ap.parse_args()

    if args.input == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.input) as f:
            lines = f.readlines()

    data = parse(lines)
    if not data["time"]:
        print("No data lines found.", file=sys.stderr)
        sys.exit(1)

    plot(data, args.output)


if __name__ == "__main__":
    main()
