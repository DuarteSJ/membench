#!/usr/bin/env python3
"""Run the three corun placements and bar-chart their makespan.

  hot_on_slow    chase fast, scatter slow  (AOL-correct placement)  -> optimal
  MEMTIS_managed kernel chooses (DRAM capped, regions unbound)      -> what MEMTIS does
  hot_on_fast    chase slow, scatter fast  (MEMTIS's wrong pick)    -> worst

MEMTIS misplaces (ranks scatter hotter by raw LLC-miss count), so the managed
bar lands near the worst static bar instead of the optimal one. That gap is the
misplacement cost.

Run as root (membench pins memory / writes htmm sysfs):
    sudo ./graph_maker.py
Tune the shared knobs with flags; see -h.
"""

import argparse
import re
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
    return [
        ("hot_on_slow\n",
         corun + ["-A", str(a.fast), "-B", str(a.slow)]),
        ("MEMTIS_managed\n",
         ["sudo", str(MANAGED), "-d", str(a.cap), "-L", str(a.region),
          "-N", str(a.chase), "-M", str(a.scatter), "-D", str(a.delay),
          "-c", str(a.core), "-f", str(a.fast)]),
        ("hot_on_fast\n",
         corun + ["-A", str(a.slow), "-B", str(a.fast)]),
    ]


def makespan_of(label, argv):
    """Run one scenario, return its makespan_sec (float) or None on failure."""
    print(f"running {label.splitlines()[0]} ...", flush=True)
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
    # green optimal, red managed (the result), grey worst reference
    colors = ["#2ca02c", "#7f7f7f", "#d62728"]
    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, values, color=colors[: len(values)])
    ax.set_ylabel("makespan (seconds)")
    ax.set_title("membench corun placements: optimal vs MEMTIS vs worst")
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
    p.add_argument("-N", "--chase", type=int, default=0.9, help="chase passes")
    p.add_argument("-M", "--scatter", type=int, default=1.5146, help="scatter passes")
    p.add_argument("-D", "--delay", type=int, default=8000, help="scatter think-time")
    p.add_argument("-c", "--core", type=int, default=0, help="first core")
    p.add_argument("-d", "--cap", type=int, default=2048, help="managed DRAM cap MB")
    p.add_argument("--fast", type=int, default=0, help="fast node")
    p.add_argument("--slow", type=int, default=2, help="slow node")
    p.add_argument("-o", "--out", default=str(HERE / "makespan.png"), help="output PNG")
    a = p.parse_args()

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
