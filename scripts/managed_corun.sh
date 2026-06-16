#!/usr/bin/env bash
#
# managed_corun.sh - run membench `corun` under MEMTIS management.
#
# Unlike the static -A/-B placements, here BOTH regions are unbound (-A -1 -B -1):
# plain anon pages the kernel is free to migrate. We put membench in the `htmm`
# cgroup and CAP the DRAM (fast-tier) node below the 2-region working set, so
# MEMTIS must *choose* which pages to promote. Stock MEMTIS ranks scatter hotter
# (more LLC-miss samples) and promotes it -> chase stuck slow -> big makespan.
# AOL-weighted hotness promotes chase instead. Compare makespan_sec against the
# static runs (-A 0 -B 2 vs -A 2 -B 0) to see which policy the kernel matched.
#
# Must run on the htmm / AOL kernel, as root (writes htmm sysfs + cgroup v2).
#
#   sudo scripts/managed_corun.sh [opts]
#     -d <MB>   DRAM cap for the fast node      (default 2048; ~half the 4GB WS)
#     -L <MB>   region size per workload        (default 2048)
#     -N <n>    chase passes                    (default 1)
#     -M <n>    scatter passes                  (default 5)
#     -D <n>    scatter think-time              (default 8000)
#     -f <node> fast / DRAM node to cap         (default 0)
#     -c <core> first core (chase=core, scatter=core+1)  (default 0)
#     -O <chase|scatter> which region to alloc first     (default chase)
#
# Lock -N/-M/-D to the values you calibrated in the both-fast run first.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$HERE/src/membench}"
CG_DIR=/sys/fs/cgroup
CG_NAME=htmm
CG="$CG_DIR/$CG_NAME"

DRAM_MB=2048
REGION_MB=2048
CHASE_PASSES=1
SCATTER_PASSES=5
DELAY=8000
FAST_NODE=0
CORE0=0
ALLOC_FIRST=chase

while getopts "d:L:N:M:D:f:c:O:h" o; do
    case "$o" in
        d) DRAM_MB="$OPTARG" ;;
        L) REGION_MB="$OPTARG" ;;
        N) CHASE_PASSES="$OPTARG" ;;
        M) SCATTER_PASSES="$OPTARG" ;;
        D) DELAY="$OPTARG" ;;
        f) FAST_NODE="$OPTARG" ;;
        c) CORE0="$OPTARG" ;;
        O) ALLOC_FIRST="$OPTARG" ;;
        h) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "must run as root (htmm sysfs + cgroup writes)" >&2; exit 1
fi
if [[ ! -x "$BIN" ]]; then
    echo "membench not built at $BIN (run 'make -C $HERE/src')" >&2; exit 1
fi
if [[ ! -d /sys/kernel/mm/htmm ]]; then
    echo "/sys/kernel/mm/htmm missing - not on the htmm/AOL kernel?" >&2; exit 1
fi

# ---- htmm sysfs settings (mirrors memtis-userspace run_bench.sh) ------------
htmm_setting() {
    echo 199     > /sys/kernel/mm/htmm/htmm_sample_period
    echo 100007  > /sys/kernel/mm/htmm/htmm_inst_sample_period
    echo 1       > /sys/kernel/mm/htmm/htmm_thres_hot
    echo 2       > /sys/kernel/mm/htmm/htmm_split_period
    echo 100000  > /sys/kernel/mm/htmm/htmm_adaptation_period
    echo 2000000 > /sys/kernel/mm/htmm/htmm_cooling_period
    echo 2       > /sys/kernel/mm/htmm/htmm_mode
    echo 500     > /sys/kernel/mm/htmm/htmm_demotion_period_in_ms
    echo 500     > /sys/kernel/mm/htmm/htmm_promotion_period_in_ms
    echo 4       > /sys/kernel/mm/htmm/htmm_gamma
    echo 30      > /sys/kernel/mm/htmm/ksampled_soft_cpu_quota
    echo 1       > /sys/kernel/mm/htmm/htmm_thres_split
    echo 0       > /sys/kernel/mm/htmm/htmm_nowarm
    echo disabled > /sys/kernel/mm/htmm/htmm_cxl_mode
    echo always  > /sys/kernel/mm/transparent_hugepage/enabled
    echo always  > /sys/kernel/mm/transparent_hugepage/defrag
    echo 0       > /proc/sys/kernel/numa_balancing
}

# ---- cgroup lifecycle ------------------------------------------------------
cg_setup() {
    rmdir "$CG" 2>/dev/null || true            # clear any stale instance
    mkdir -p "$CG"
    echo "+memory" > "$CG_DIR/cgroup.subtree_control"
    echo "+cpuset" > "$CG_DIR/cgroup.subtree_control" 2>/dev/null || true
    # cap the fast (DRAM) node; capacity tier left at max (default)
    local bytes=$(( DRAM_MB * 1024 * 1024 ))
    if [[ ! -e "$CG/memory.max_at_node${FAST_NODE}" ]]; then
        echo "no memory.max_at_node${FAST_NODE} - htmm per-node cap unsupported?" >&2
        exit 1
    fi
    echo "$bytes" > "$CG/memory.max_at_node${FAST_NODE}"
    echo enabled  > "$CG/memory.htmm_enabled"
}

cg_teardown() {
    echo disabled > "$CG/memory.htmm_enabled" 2>/dev/null || true
}
trap cg_teardown EXIT

# ---------------------------------------------------------------------------
echo "== MEMTIS-managed corun =="
echo "DRAM cap node$FAST_NODE: ${DRAM_MB}MB   working set: $((2 * REGION_MB))MB (2x ${REGION_MB})"
echo "chase passes=$CHASE_PASSES   scatter passes=$SCATTER_PASSES   D=$DELAY   core0=$CORE0   alloc=$ALLOC_FIRST-first"
echo

htmm_setting
echo 3 > /proc/sys/vm/drop_caches
cg_setup
sleep 2

dmesg -c > /dev/null 2>&1 || true
grep -e htmm -e pgmig /proc/vmstat > /tmp/managed_corun.vmstat.before 2>/dev/null || true

# Put THIS shell in the cgroup; the membench child inherits it. Regions unbound
# so MEMTIS owns placement.
echo $$ > "$CG/cgroup.procs"

"$BIN" -m corun -L "$REGION_MB" -A -1 -B -1 \
       -N "$CHASE_PASSES" -M "$SCATTER_PASSES" -D "$DELAY" -c "$CORE0" \
       -O "$ALLOC_FIRST"
rc=$?

# move our shell back to root so the cgroup can be reused/removed later
echo $$ > "$CG_DIR/cgroup.procs" 2>/dev/null || true

echo
printf -- "---- htmm / migration counters ----\n%-28s %14s %14s %14s\n" \
       counter before after delta
grep -e htmm -e pgmig /proc/vmstat > /tmp/managed_corun.vmstat.after 2>/dev/null || true
if [[ -s /tmp/managed_corun.vmstat.before ]]; then
    join /tmp/managed_corun.vmstat.before /tmp/managed_corun.vmstat.after 2>/dev/null \
        | awk '{printf "%-28s %14s %14s %+14d\n", $1, $2, $3, $3-$2}'
fi
echo
echo "(weight printks: sudo dmesg | grep htmm_aol)"
exit $rc
