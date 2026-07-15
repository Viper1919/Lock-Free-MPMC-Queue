#!/usr/bin/env python3
"""CSV -> charts. Reads bench_main CSV files, plots median throughput vs
thread count (one line per queue) with IQR error bars.

Usage: python3 bench/plot.py results/*.csv -o results/throughput.svg
"""

import argparse
import csv
import statistics
from collections import defaultdict


def load(paths):
    # {(queue, threads): [throughput per rep]}
    data = defaultdict(list)
    for path in paths:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                key = (row["queue"], int(row["threads"]))
                data[key].append(float(row["throughput_ops_s"]))
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("-o", "--output", default="results/throughput.svg")
    args = ap.parse_args()

    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    data = load(args.csv)
    queues = sorted({q for q, _ in data})

    fig, ax = plt.subplots(figsize=(7, 4.5))
    for queue in queues:
        threads = sorted(t for q, t in data if q == queue)
        med = [statistics.median(data[(queue, t)]) for t in threads]
        q1 = [statistics.quantiles(data[(queue, t)], n=4)[0] for t in threads]
        q3 = [statistics.quantiles(data[(queue, t)], n=4)[2] for t in threads]
        err = [[m - lo for m, lo in zip(med, q1)], [hi - m for m, hi in zip(med, q3)]]
        ax.errorbar(threads, med, yerr=err, marker="o", capsize=3, label=queue)

    ax.set_xlabel("threads")
    ax.set_ylabel("throughput (ops/s)")
    ax.set_title("Throughput vs. thread count (median, IQR bars)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted({t for _, t in data}))
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.output)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
