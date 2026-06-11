#!/usr/bin/env bash
#
# characterize.sh — the FAST test tier.
#
# Runs each membench access pattern in isolation under `perf stat` and reports
# the PMU signature that decides whether the kernel's hotness logic can tell the
# patterns apart. Needs NO custom kernel and NO reboot — run it on stock to
# prove the workloads behave before spending a kernel build/boot on a managed
# run.
#
# Counters (raw encodings mirror linux/include/linux/htmm.h):
#   A1   cpu/0xb1,0x01,cmask=1/  OFFCORE_REQUESTS_OUTSTANDING cycles w/ >=1 pending demand read
#   A3   cpu/0xb0,0x01/          OFFCORE_REQUESTS demand data reads (count)
#   llc  cpu/0xd1,0x20/          MEM_LOAD_RETIRED.L3_MISS (proxy for what MEMTIS samples)
#   sllc cpu/0xa3,0x06,cmask=6/  CYCLE_ACTIVITY.STALLS_L3_MISS (cycles stalled on L3 miss)
#   cyc  cycles                  CPU_CLK_UNHALTED
#
# (4 GP events + cycles on the fixed counter -> no multiplexing, counts exact.)
#
# Derived:
#   AOL = A1 / A3       (kernel's definition; ~avg exposed latency per request)
#   P   = sllc / cyc    (stall fraction; the kernel's P)
#   aol_wt = (1 + P*K)*1024 where K = AOL/(a + b/AOL), a=0.0625, b=1.28
#           — the exact value the AOL kernel's `htmm_aol:` printk reports as
#             weight= for a window dominated by this pattern. 1024 = neutral.
#
# Expected signatures:
#   chase : HIGH AOL, HIGH llc/s   (latency-bound)   -> MEMTIS sees hot
#   scatter   : LOW  AOL, HIGH llc/s   (bandwidth-bound) -> MEMTIS sees hot
#
# chase and scatter split on AOL (exposed latency per request): that gap is exactly
# what AOL-weighted hotness exploits and stock MEMTIS, ranking by miss count
# alone, cannot. macc/s yields k, the equal-wall-time calibration multiplier.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$HERE/src/membench}"
DUR="${DUR:-5}"
REGION_MB="${REGION_MB:-2048}"
THREADS="${THREADS:-1}"
CORE0="${CORE0:-0}"
PATTERNS="${PATTERNS:-chase scatter}"
NODE="${NODE:--1}"
DELAY="${DELAY:-0}"

if [[ ! -x "$BIN" ]]; then
    echo "membench binary not found at $BIN (run 'make -C $HERE/src')" >&2
    exit 1
fi

EVENTS="cpu/event=0xb1,umask=0x01,cmask=0x01/,cpu/event=0xb0,umask=0x01/,cpu/event=0xd1,umask=0x20/,cpu/event=0xa3,umask=0x06,cmask=0x06/,cycles"

# perf -x, CSV columns: value,unit,event,runtime,pct,...  ; we key by event name.
field() { # $1=csvfile $2=event-substring
    # NB: -F, splits the event encoding on its own internal commas, so the
    # event name spans several fields. Match the whole line, take col 1.
    awk -F, -v ev="$2" '$0 ~ ev {gsub(/ /,"",$1); print $1; exit}' "$1"
}

declare -A MACC   # pattern -> maccess_per_s (for the k multiplier below)

if [[ "$NODE" -lt 0 ]]; then tier="unbound / local DRAM (fast tier)";
else                         tier="pinned to NUMA node $NODE"; fi
printf 'region: %s MB, %s, %s threads, %ss each, scatter think-time -D %s\n\n' \
       "$REGION_MB" "$tier" "$THREADS" "$DUR" "$DELAY"

printf "%-7s %12s %12s %8s %11s %9s %6s %8s\n" \
       pattern A1 A3 AOL "llc_miss/s" "macc/s" P aol_wt
printf '%.0s-' {1..82}; echo

for pat in $PATTERNS; do
    perf_csv="$(mktemp)"
    perf stat -x, -o "$perf_csv" \
        -e "$EVENTS" \
        -- "$BIN" -p "$pat" -L "$REGION_MB" -s "$DUR" -t "$THREADS" -c "$CORE0" \
                  -X "$NODE" -D "$DELAY" \
        >"$perf_csv.out" 2>"$perf_csv.err"

    # membench prints one CSV line on stdout. Missing => it never ran (NUMA
    # build? region alloc failed?); perf will still report tiny startup counts,
    # so guard on this FIRST or the noise masquerades as data.
    macc="$(awk -F, '/^membench,/{for(i=1;i<=NF;i++) if($i ~ /^maccess_per_s=/){
                sub(/maccess_per_s=/,"",$i); print $i; exit}}' "$perf_csv.out")"
    if [[ -z "$macc" ]]; then
        echo "$pat: membench produced no result line — it did not run." >&2
        echo "  likely: binary built without NUMA but -X $NODE requested," >&2
        echo "          or region alloc failed. membench stderr:" >&2
        sed 's/^/    /' "$perf_csv.err" >&2
        rm -f "$perf_csv" "$perf_csv.err" "$perf_csv.out"
        continue
    fi
    MACC[$pat]="$macc"

    a1="$(field "$perf_csv" 'event=0xb1,umask=0x01,cmask=0x01')"
    a3="$(field "$perf_csv" 'event=0xb0,umask=0x01')"
    llc="$(field "$perf_csv" 'event=0xd1,umask=0x20')"
    sllc="$(field "$perf_csv" 'event=0xa3,umask=0x06,cmask=0x06')"
    cyc="$(field "$perf_csv" 'cycles')"

    if [[ -z "${a1:-}" || -z "${a3:-}" ]]; then
        echo "$pat: perf returned no counts (permissions? unsupported events?)" >&2
        echo "  see $perf_csv.err" >&2
        rm -f "$perf_csv" "$perf_csv.err" "$perf_csv.out"
        continue
    fi

    # AOL, llc/s, P, and the predicted aol_weight (kernel's fixed-point).
    read -r aol llcs p wt < <(awk \
        -v a1="$a1" -v a3="$a3" -v llc="${llc:-0}" \
        -v sllc="${sllc:-0}" -v cyc="${cyc:-0}" -v d="$DUR" 'BEGIN{
            aol  = (a3>0 ? a1/a3 : 0);
            llcs = (d>0   ? llc/d : 0);
            p    = (cyc>0 ? sllc/cyc : 0);
            a=0.0625; b=1.28;
            k  = (aol>0 ? aol/(a*aol + b) : 0);
            wt = (1.0 + p*k) * 1024.0;     # kernel weight, AOL_SCALE fixed point
            printf "%.2f %.3e %.3f %.0f", aol, llcs, p, wt }')

    printf "%-7s %12s %12s %8s %11s %9s %6s %8s\n" \
           "$pat" "$a1" "$a3" "$aol" "$llcs" "$macc" "$p" "$wt"
    rm -f "$perf_csv" "$perf_csv.err" "$perf_csv.out"
done

# k = scatter/chase throughput in the fast tier. corun regions are equal size
# (-L), so one pass = nlines accesses for both; equal fast-tier time needs
# scatter to run k x chase's passes -> start the corun sweep at -M = k*-N, then
# fine-tune -N/-M/-D. Re-measure on the target box.
if [[ -n "${MACC[chase]:-}" && -n "${MACC[scatter]:-}" ]]; then
    awk -v c="${MACC[chase]}" -v m="${MACC[scatter]}" 'BEGIN{
        if (c>0) printf "\nmultiplier  k = scatter/chase throughput = %.2f  (corun: -M ~= k*-N)\n", m/c
        else     print  "\nmultiplier  chase throughput is zero — check the run" }'
fi
