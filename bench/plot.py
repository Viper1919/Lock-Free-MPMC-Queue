#!/usr/bin/env python3
"""CSV -> SVG charts for the phase-3b benchmark results.

Reads the CSVs bench_main emits (results/phase3b_*.csv), aggregates the
repetitions to medians, and renders the report charts. Medians only —
the harness's own rule: one scheduler hiccup destroys a mean.

Usage:  python3 bench/plot.py [--results results] [--out results]
Needs matplotlib + numpy; everything else is stdlib.
"""

import argparse
import csv
import os
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Palette: validated categorical slots (light mode), fixed queue->hue
# assignment so a queue keeps its color across every chart. Text wears
# ink tokens, never series colors.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

COLOR = {
    "mutex": "#2a78d6",       # blue
    "ms": "#008300",          # green
    "ms-hp": "#e87ba4",       # magenta
    "moodycamel": "#eda100",  # yellow
    # unpadded variants share their padded twin's hue; dash carries it
    "ms-unpadded": "#008300",
    "ms-hp-unpadded": "#e87ba4",
}
LABEL = {
    "mutex": "mutex",
    "ms": "ms (leaky)",
    "ms-hp": "ms-hp",
    "moodycamel": "moodycamel",
    "ms-unpadded": "ms (adjacent)",
    "ms-hp-unpadded": "ms-hp (adjacent)",
}

HW_THREADS = 8  # annotate the hardware-concurrency boundary


def load_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def med_by_threads(rows, field, scale=1.0):
    """{threads: median(field) * scale} across repetitions."""
    acc = {}
    for r in rows:
        acc.setdefault(int(r["threads"]), []).append(float(r[field]))
    return {t: median(v) * scale for t, v in sorted(acc.items())}


def style_axes(ax, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    ax.grid(axis="y", color=GRID, linewidth=0.75)
    ax.set_axisbelow(True)
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.set_xlabel(xlabel, color=INK_2, fontsize=10)
    ax.set_ylabel(ylabel, color=INK_2, fontsize=10)


def new_fig(title):
    fig, ax = plt.subplots(figsize=(8, 4.5), facecolor=SURFACE)
    ax.set_title(title, color=INK, fontsize=12, loc="left", pad=12)
    return fig, ax


def hw_marker(ax):
    ax.axvline(HW_THREADS, color=GRID, linewidth=1, linestyle=(0, (4, 4)))
    ax.annotate("HW threads", xy=(HW_THREADS, 1.0),
                xycoords=("data", "axes fraction"), xytext=(4, -2),
                textcoords="offset points", color=MUTED, fontsize=8, va="top")


def line_chart(ax, series, dashed=(), logy=False):
    """series: {queue: {threads: value}}; direct label at each line end,
    dodged vertically so nearby line-ends don't collide."""
    ends = []
    for q, data in series.items():
        xs, ys = list(data.keys()), list(data.values())
        ax.plot(xs, ys, color=COLOR[q], linewidth=2, marker="o",
                markersize=6, linestyle="--" if q in dashed else "-",
                label=LABEL[q])
        ends.append((q, xs[-1], ys[-1]))
    ax.set_xscale("log", base=2)
    if logy:
        ax.set_yscale("log")
    ax.set_xticks(list(next(iter(series.values())).keys()))
    ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.margins(x=0.14)

    # Dodge pass needs final scales, so it runs after them.
    ax.figure.canvas.draw()
    min_gap = 13.0  # px between label baselines
    last = None
    for y_px, q, x, y in sorted(
            (ax.transData.transform((x, y))[1], q, x, y) for q, x, y in ends):
        target = y_px if last is None else max(y_px, last + min_gap)
        ax.annotate(LABEL[q], xy=(x, y), xytext=(6, target - y_px),
                    textcoords="offset pixels", color=INK_2, fontsize=8.5,
                    va="center")
        last = target
    leg = ax.legend(frameon=False, fontsize=8.5, labelcolor=INK_2)
    for line in leg.get_lines():
        line.set_linewidth(2)


def save(fig, out_dir, name):
    path = os.path.join(out_dir, name)
    fig.savefig(path, bbox_inches="tight", facecolor=SURFACE)
    plt.close(fig)
    print(f"wrote {path}")


def chart_pairs_throughput(results, out):
    series = {}
    for q in ("mutex", "ms", "ms-hp", "moodycamel"):
        rows = load_rows(os.path.join(results, f"phase3b_pairs_{q}.csv"))
        series[q] = med_by_threads(rows, "throughput_ops_s", 1e-6)
    fig, ax = new_fig("Throughput vs threads — pairs workload (median of 10 reps)")
    style_axes(ax, "threads", "M ops/s")
    line_chart(ax, series)
    hw_marker(ax)
    save(fig, out, "phase3b_pairs_throughput.svg")


def chart_pairs_scalability(results, out):
    series = {}
    for q in ("mutex", "ms", "ms-hp", "moodycamel"):
        rows = load_rows(os.path.join(results, f"phase3b_pairs_{q}.csv"))
        tp = med_by_threads(rows, "throughput_ops_s")
        base = tp[min(tp)]
        series[q] = {t: v / base for t, v in tp.items()}
    fig, ax = new_fig("Scalability — throughput normalized to 1 thread")
    style_axes(ax, "threads", "x single-thread throughput")
    line_chart(ax, series)
    ax.axhline(1.0, color=BASELINE, linewidth=1)
    hw_marker(ax)
    save(fig, out, "phase3b_pairs_scalability.svg")


def chart_pairs_p99(results, out):
    series = {}
    for q in ("mutex", "ms", "ms-hp", "moodycamel"):
        rows = load_rows(os.path.join(results, f"phase3b_pairs_{q}.csv"))
        series[q] = med_by_threads(rows, "deq_p99_ns", 1e-3)
    fig, ax = new_fig("Dequeue p99 latency vs threads — pairs workload")
    style_axes(ax, "threads", "p99 latency (µs, log)")
    line_chart(ax, series, logy=True)
    hw_marker(ax)
    save(fig, out, "phase3b_pairs_deq_p99.svg")


def chart_false_sharing(results, out):
    series = {}
    for q in ("ms", "ms-unpadded", "ms-hp", "ms-hp-unpadded"):
        rows = load_rows(os.path.join(results, f"phase3b_pairs_{q}.csv"))
        series[q] = med_by_threads(rows, "throughput_ops_s", 1e-6)
    fig, ax = new_fig(
        "False sharing — head/tail padded to separate cache lines vs adjacent")
    style_axes(ax, "threads", "M ops/s")
    line_chart(ax, series, dashed=("ms-unpadded", "ms-hp-unpadded"))
    hw_marker(ax)
    save(fig, out, "phase3b_false_sharing.svg")


def chart_ratio_bars(results, out, threads=8):
    queues = ("mutex", "ms", "ms-hp", "moodycamel")
    scenarios = [("pc_1to3", "1:3 (2P/6C)"), ("pc_3to1", "3:1 (6P/2C)")]
    values = {}  # (scenario, queue) -> M ops/s
    for key, _ in scenarios:
        for q in queues:
            rows = load_rows(os.path.join(results, f"phase3b_{key}_{q}.csv"))
            values[(key, q)] = med_by_threads(
                rows, "throughput_ops_s", 1e-6)[threads]

    fig, ax = new_fig(
        f"Producer:consumer ratios at {threads} threads (median throughput)")
    style_axes(ax, "", "M ops/s")
    width, gap = 0.8, 1.4
    for si, (key, label) in enumerate(scenarios):
        for qi, q in enumerate(queues):
            x = si * (len(queues) * width + gap) + qi * width
            v = values[(key, q)]
            ax.bar(x, v, width=width * 0.94, color=COLOR[q],
                   edgecolor=SURFACE, linewidth=1)
            ax.annotate(f"{v:.1f}", xy=(x, v), xytext=(0, 3),
                        textcoords="offset points", ha="center",
                        color=INK_2, fontsize=8.5)
    centers = [si * (len(queues) * width + gap) + (len(queues) - 1) * width / 2
               for si in range(len(scenarios))]
    ax.set_xticks(centers)
    ax.set_xticklabels([label for _, label in scenarios], color=INK_2)
    handles = [plt.Rectangle((0, 0), 1, 1, color=COLOR[q]) for q in queues]
    ax.legend(handles, [LABEL[q] for q in queues], frameon=False,
              fontsize=8.5, labelcolor=INK_2)
    save(fig, out, "phase3b_ratio_bars.svg")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--results", default="results")
    p.add_argument("--out", default="results")
    args = p.parse_args()
    chart_pairs_throughput(args.results, args.out)
    chart_pairs_scalability(args.results, args.out)
    chart_pairs_p99(args.results, args.out)
    chart_false_sharing(args.results, args.out)
    chart_ratio_bars(args.results, args.out)


if __name__ == "__main__":
    main()
