#!/usr/bin/env bash
#
# calibrate.sh - fit the AOL K-curve parameters (a, b) for the kernel, using the
# SOAR/ALTO microbenchmark (the same workloads the paper calibrated against).
#   https://github.com/MoatLab/SoarAlto/tree/main/src/microbenchmark
#
# The AOL-weighted hotness kernel weights each PEBS sample by
#       weight = 1 + P * K,   K = 1 / (a + b/AOL)
# AOL = A1/A3 (exposed memory latency per demand read), P = s_LLC/c (fraction of
# cycles stalled on L3 misses). a, b are HARDWARE-DEPENDENT; this measures them.
#
# Why SoarAlto and not membench: the paper's a/b come from these exact two
# access patterns (random pointer-chase, intensive sequential read). To
# reproduce / compare against the paper, calibrate on the same microbench.
#
# Method, per (workload, buffer) data point:
#   1. Pilot on FAST tier with -i 1, measure wall t1.
#   2. iter = round(DUR / t1), clamped >=1, so the timed run takes ~DUR.
#   3. FAST tier under perf  -> A1, A3, s_LLC, c, t_fast.
#   4. SLOW tier (no perf)   -> t_slow.
#   5. AOL=A1/A3 ; P=s_LLC/c ; S=t_slow/t_fast - 1 ; K=S/P.
#   Solve 1/K = a + b/AOL over the two points; print kernel-scaled a, b.
#
# TWO points only (paper method): pchase (-R 0.0, high AOL) and stream
# (-R 1.0, low AOL), both at buffer 2048 MB. SoarAlto's -R only gives clean
# single-workload PMU at 0.0 and 1.0 (anything between runs BOTH threads at
# once), so two points is the ceiling for this microbench. Two unknowns, two
# points -> the line is exact (zero residual). For a real regression you'd need
# more distinct workloads, not more -R values.
#
# Requires: numactl, perf (sudo), bc, the built SoarAlto `bench`.
#
# Env:
#   BENCH     SoarAlto bench binary (default ./soar-microbench/src/bench)
#   FAST      fast NUMA node        (default 0)
#   SLOW      slow NUMA node        (default 2)
#   DUR       target sec per run    (default 15)
#   BUF       buffer size MB        (default 2048, per paper Sec 2.1)
#   OUT       output csv            (default results/<host>-calibrate.csv)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# vendored, locally-patched SoarAlto bench (see soar-microbench/LOCAL_PATCHES.md)
BENCH="${BENCH:-$HERE/soar-microbench/src/bench}"
FAST="${FAST:-0}"
SLOW="${SLOW:-2}"
DUR="${DUR:-35}"
BUF="${BUF:-2048}"
OUT="${OUT:-$HERE/results/$(hostname)-calibrate.csv}"

# Raw event encodings - byte layout [cmask][edge..][umask][event], identical to
# linux/include/linux/htmm.h. The four counters the kernel consumes:
#   A1    r010001b1  OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD, cmask=1 (cycles)
#   A3    r000001b0  OFFCORE_REQUESTS.DEMAND_DATA_RD              (count)
#   s_LLC r060006a3  CYCLE_ACTIVITY.STALLS_L3_MISS, cmask=6
#   c     r0000003c  CPU_CLK_UNHALTED.THREAD
EVENTS=r010001b1,r000001b0,r060006a3,r0000003c

command -v numactl >/dev/null || { echo "numactl not found" >&2; exit 1; }
command -v bc      >/dev/null || { echo "bc not found" >&2; exit 1; }
[[ -x "$BENCH" ]] || { echo "SoarAlto bench not at $BENCH (build: make -C $HERE/soar-microbench/src)" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

walltime() { # run "$@", echo elapsed seconds
    local t0 t1
    t0=$(date +%s.%N); "$@" >/dev/null 2>&1; t1=$(date +%s.%N)
    echo "$t1 - $t0" | bc -l
}

# pchase -> -R 0.0 -A buf ; stream -> -R 1.0 -B buf
bench_args() { # workload buf  -> echoes the SoarAlto flags
    if [[ "$1" == pchase ]]; then echo "-R 0.0 -A $2"; else echo "-R 1.0 -B $2"; fi
}

pilot_iter() { # workload buf -> iteration count for ~DUR seconds
    local t; t=$(walltime numactl --membind="$FAST" "$BENCH" $(bench_args "$1" "$2") -i 1)
    awk -v t="$t" -v dur="$DUR" 'BEGIN{i=int(dur/t+0.5); if(i<1)i=1; print i}'
}

pfield() { awk -F, -v ev="$2" '$0 ~ ev {gsub(/ /,"",$1); print $1; exit}' "$1"; }

run_fast_perf() { # workload buf iter -> "a1,a3,sllc,c,t_fast"
    local pf t0 t1; pf=$(mktemp)
    t0=$(date +%s.%N)
    sudo perf stat -x, -o "$pf" -e "$EVENTS" -- \
        numactl --membind="$FAST" "$BENCH" $(bench_args "$1" "$2") -i "$3" >/dev/null 2>&1
    t1=$(date +%s.%N)
    printf '%s,%s,%s,%s,%s\n' \
        "$(pfield "$pf" r010001b1)" "$(pfield "$pf" r000001b0)" \
        "$(pfield "$pf" r060006a3)" "$(pfield "$pf" r0000003c)" \
        "$(echo "$t1 - $t0" | bc -l)"
    rm -f "$pf"
}

run_slow() { # workload buf iter -> t_slow
    walltime numactl --membind="$SLOW" "$BENCH" $(bench_args "$1" "$2") -i "$3"
}

echo "workload,buf_mb,iter,a1,a3,s_llc,c,t_fast,t_slow,AOL,P,S,K" > "$OUT"
echo "calibrating with SoarAlto: buf=${BUF}MB fast=$FAST slow=$SLOW dur=${DUR}s" >&2

run_point() {
    local w="$1" buf="$2" iter fast
    echo "=== $w ${buf}MB ===" >&2
    iter=$(pilot_iter "$w" "$buf"); echo "  pilot -> iter=$iter" >&2
    fast=$(run_fast_perf "$w" "$buf" "$iter")
    IFS=, read -r a1 a3 sllc c tf <<< "$fast"
    local ts; ts=$(run_slow "$w" "$buf" "$iter")
    awk -v w="$w" -v buf="$buf" -v it="$iter" -v a1="$a1" -v a3="$a3" -v sllc="$sllc" \
        -v c="$c" -v tf="$tf" -v ts="$ts" 'BEGIN{
            if(a3==0||c==0||tf==0){print w","buf",zero counter">"/dev/stderr"; exit}
            aol=a1/a3; P=sllc/c; S=ts/tf-1.0; K=(P>0)?S/P:0
            printf "%s,%s,%s,%s,%s,%s,%s,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f\n",
                w,buf,it,a1,a3,sllc,c,tf,ts,aol,P,S,K
        }' | tee -a "$OUT"
}

run_point pchase "$BUF"
run_point stream "$BUF"

echo >&2
echo "=== solve  K = 1 / (a + b/AOL)  through the 2 points ===" >&2
# Start from the K-curve and linearize:
#     K = 1 / (a + b/AOL)            # the model
#  => 1/K = a + b/AOL                # reciprocate
#  let x = 1/AOL, y = 1/K:
#     y = a + b*x                    # linear in the unknowns a, b
# Two measured points (x1,y1), (x2,y2) give two equations:
#     y1 = a + b*x1
#     y2 = a + b*x2
# subtract  -> b = (y2 - y1) / (x2 - x1)
# back-sub  -> a = y1 - b*x1
awk -F, '
    NR>1 && $13+0 > 0 && $10+0 > 0 {           # K>0 and AOL>0
        n++; X[n]=1.0/$10; Y[n]=1.0/$13
        printf "  %-6s %5sMB  AOL=%7.2f  K=%7.4f\n", $1, $2, $10, $13 > "/dev/stderr"
    }
    END{
        if(n!=2){print "need exactly 2 sane points (got "n")">"/dev/stderr"; exit 1}
        if(X[1]==X[2]){print "degenerate: both points share one AOL">"/dev/stderr"; exit 1}
        b=(Y[2]-Y[1])/(X[2]-X[1]); a=Y[1]-b*X[1]
        printf "\nsolved:  a = %.4f   b = %.4f\n", a, b > "/dev/stderr"
        printf "scaled (AOL_SCALE=1024):  a=%d  b=%d\n", int(a*1024+0.5), int(b*1024+0.5) > "/dev/stderr"
        printf "apply live (no rebuild):\n" > "/dev/stderr"
        printf "  echo %d | sudo tee /sys/kernel/mm/htmm/htmm_aol_param_a\n", int(a*1024+0.5) > "/dev/stderr"
        printf "  echo %d | sudo tee /sys/kernel/mm/htmm/htmm_aol_param_b\n", int(b*1024+0.5) > "/dev/stderr"
    }
' "$OUT"
echo >&2; echo "wrote $OUT" >&2
