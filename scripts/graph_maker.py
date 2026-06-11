#!/usr/bin/env python3
"""Run the corun placements and bar-chart their makespan.

  all_on_fast    both regions on DRAM                              -> all-fast baseline
  all_on_slow    both regions on slow tier                         -> all-slow floor
  hot_on_slow    chase fast, scatter slow   (AOL-correct)          -> optimal split
  hot_on_fast    chase slow, scatter fast   (MEMTIS's wrong pick)  -> worst split
  MEMTIS_managed kernel chooses (DRAM capped, regions unbound)     -> what MEMTIS does

("hot" = scatter, the page MEMTIS ranks hottest by raw LLC-miss count.) MEMTIS
promotes scatter and demotes chase, so the managed bar lands near hot_on_fast
instead of hot_on_slow. That gap is the misplacement cost.

Run as your user (each scenario prefixes sudo internally):
    ./graph_maker.py
Tune the shared knobs with flags; see -h.
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")           # headless: no DISPLAY needed on the box
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
BIN = HERE.parent / "src" / "membench"
MANAGED = HERE / "managed_corun.sh"

MAKESPAN_RE = re.compile(r"makespan_sec=([0-9.]+)")


def build_scenarios(a):
    """(label, argv) per scenario, ordered optimal -> managed -> worst."""
    corun = ["sudo", str(BIN), "-m", "corun",
             "-L", str(a.region), "-N", str(a.chase), "-M", str(a.scatter),
             "-D", str(a.delay), "-c", str(a.core)]

    def managed(alloc):
        return ["sudo", str(MANAGED), "-d", str(a.cap), "-L", str(a.region),
                "-N", str(a.chase), "-M", str(a.scatter), "-D", str(a.delay),
                "-c", str(a.core), "-f", str(a.fast), "-O", alloc]

    return [
        ("all_on_fast\n",
         corun + ["-A", str(a.fast), "-B", str(a.fast)]),
        ("all_on_slow\n",
         corun + ["-A", str(a.slow), "-B", str(a.slow)]),
        ("hot_on_fast\n",
         corun + ["-A", str(a.slow), "-B", str(a.fast)]),
        ("hot_on_slow\n",
         corun + ["-A", str(a.fast), "-B", str(a.slow)]),
        ("MEMTIS_managed\nalloc=chase", managed("chase")),
        ("MEMTIS_managed\nalloc=scatter", managed("scatter")),
    ]


def makespan_of(label, argv):
    """Run one scenario, return its makespan_sec (float) or None on failure."""
    print(f"running {label.splitlines()[0]} ...", flush=True)
    print(f"  $ {shlex.join(argv)}", flush=True)
    r = subprocess.run(argv, capture_output=True, text=True)
    out = r.stdout + r.stderr     # managed prints to both; search all
    m = MAKESPAN_RE.search(out)
    if r.returncode != 0 and m is None:
        print(f"  FAILED (rc={r.returncode}):\n{r.stderr.strip()}", file=sys.stderr)
        return None
    if m is None:
        print(f"  no makespan_sec in output:\n{out.strip()}", file=sys.stderr)
        return None
    val = float(m.group(1))
    print(f"  makespan = {val:.2f}s")
    return val


def plot(labels, values, out_path):
    # one colour per scenario, in build_scenarios order:
    # all_fast(green) all_slow(grey) hot_on_fast(red) hot_on_slow(blue)
    # managed-chase(orange) managed-scatter(purple)
    palette = ["#2ca02c", "#7f7f7f", "#d62728", "#1f77b4", "#ff7f0e", "#9467bd"]
    colors = (palette * ((len(values) // len(palette)) + 1))[: len(values)]
    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.bar(labels, values, color=colors)
    ax.set_ylabel("makespan (seconds)")
    ax.set_title("membench corun placements: makespan by tier assignment")
    ax.bar_label(bars, fmt="%.1f", padding=3)
    ax.margins(y=0.15)
    ax.grid(axis="y", linestyle=":", alpha=0.5)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"\nsaved {out_path}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-L", "--region", type=int, default=2048, help="region MB per workload")
    p.add_argument("-N", "--chase", type=float, default=0.9, help="chase passes (fractional ok)")
    p.add_argument("-M", "--scatter", type=float, default=1.5146, help="scatter passes (fractional ok)")
    p.add_argument("-x", "--mult", type=float, default=1.0,
                   help="scale both -N and -M by this (keeps their ratio; longer runs)")
    p.add_argument("-D", "--delay", type=int, default=8000, help="scatter think-time")
    p.add_argument("-c", "--core", type=int, default=0, help="first core")
    p.add_argument("-d", "--cap", type=int, default=None,
                   help="managed DRAM cap MB (default: equal to -L)")
    p.add_argument("--fast", type=int, default=0, help="fast node")
    p.add_argument("--slow", type=int, default=2, help="slow node")
    p.add_argument("-o", "--out", default=str(HERE / "makespan.png"), help="output PNG")
    a = p.parse_args()
    if a.cap is None:           # cap follows region size unless set explicitly
        a.cap = a.region
    a.chase *= a.mult           # scale work up/down, keeping the N:M ratio
    a.scatter *= a.mult

    if not BIN.exists():
        sys.exit(f"membench not built at {BIN} (run 'make -C {BIN.parent}')")

    labels, values = [], []
    for label, argv in build_scenarios(a):
        v = makespan_of(label, argv)
        if v is None:
            sys.exit("aborting: a scenario failed (see above)")
        labels.append(label)
        values.append(v)

    plot(labels, values, a.out)


if __name__ == "__main__":
    main()
