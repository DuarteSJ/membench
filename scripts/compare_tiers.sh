#!/usr/bin/env bash
#
# compare_tiers.sh — run characterize.sh on the fast and the slow node, then
# report each pattern's slow-tier slowdown = fast macc/s ÷ slow macc/s.
#
# This is the cross-tier view characterize can't give on its own (it probes one
# node per run). chase, being latency-bound, should slow more than mlp on a true
# slow tier — though note the isolated-loop caveat: both scale ~1/L, so the gap
# is small until a criticality-sensitive workload is used.
#
# Needs root (perf). Pass node overrides before the command so they survive sudo:
#   sudo FAST_NODE=0 SLOW_NODE=2 scripts/compare_tiers.sh
# Tunables (DUR, REGION_MB, THREADS, CORE0, PATTERNS) are read by characterize.
#
#   FAST_NODE  DRAM node for the fast-tier run            (default 0)
#   SLOW_NODE  PMEM / slow node (numactl -H: cpuless mem) (default 2)

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHAR="$HERE/characterize.sh"
FAST_NODE="${FAST_NODE:-0}"
SLOW_NODE="${SLOW_NODE:-2}"
PATTERNS="${PATTERNS:-chase mlp}"

if [[ ! -x "$CHAR" ]]; then
    echo "characterize.sh not found/executable at $CHAR" >&2
    exit 1
fi
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "warn: not root — perf will likely report no counts. Re-run under sudo." >&2
fi

# macc/s is column 8 of a characterize data row ("<pat> A1 A2 A3 AOL MLP llc macc P wt").
macc_of() { # $1=characterize-output  $2=pattern
    awk -v p="$2" '$1==p {print $8; exit}' <<<"$1"
}

echo "### FAST tier — NUMA node $FAST_NODE"
fast_out="$(NODE="$FAST_NODE" "$CHAR")"
echo "$fast_out"

echo
echo "### SLOW tier — NUMA node $SLOW_NODE"
slow_out="$(NODE="$SLOW_NODE" "$CHAR")"
echo "$slow_out"

echo
printf '%.0s─' {1..70}; echo
for pat in $PATTERNS; do
    f="$(macc_of "$fast_out" "$pat")"
    s="$(macc_of "$slow_out" "$pat")"
    if [[ -z "$f" || -z "$s" ]]; then
        printf '%s: missing macc/s (fast=%s slow=%s) — check the runs above\n' \
               "$pat" "${f:-?}" "${s:-?}"
        continue
    fi
    awk -v p="$pat" -v f="$f" -v s="$s" 'BEGIN{
        if (s > 0) printf "%s gets %.2fx slower when placed in the slow tier\n", p, f/s
        else       printf "%s: slow-tier throughput is zero — check the run\n", p }'
done
