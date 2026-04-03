#!/bin/bash
# A/B comparison: Cambyses vs Vanilla CFS migration efficiency
#
# Usage: sudo ./run_selection_bench.sh [selection_bench options...]
#
# Measures: selection quality, scan efficiency, migration volume,
#           SKIP_TO_HEAD churn, and heavy/light selection bias.
#
# Requires root for sysctl + ftrace.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="$SCRIPT_DIR/selection_bench"
ANALYZE="$SCRIPT_DIR/analyze_trace.py"
OUTDIR="$SCRIPT_DIR/results/$(date +%Y%m%d_%H%M%S)"

DURATION=30
HEAVY_COUNT=2
HEAVY_NICE=-3
LIGHT_NICE=5
FILLERS=1
HOT_CPU=0
WORKERS=8
RUNS=3

usage() {
    cat <<EOF
Usage: sudo $0 [options]

Options:
  -T SEC      Duration per run (default: $DURATION)
  -H NCPU     Workers on hot CPU (default: $WORKERS)
  -h COUNT    Heavy workers (default: $HEAVY_COUNT)
  -n NICE     Heavy nice (default: $HEAVY_NICE)
  -N NICE     Light nice (default: $LIGHT_NICE)
  -F COUNT    Fillers per CPU (default: $FILLERS)
  -C CPU      Hot CPU (default: $HOT_CPU)
  -r RUNS     Runs per mode (default: $RUNS)
  -o DIR      Output directory (default: auto-timestamped)
EOF
    exit 1
}

while getopts "T:H:h:n:N:F:C:r:o:" opt; do
    case $opt in
        T) DURATION=$OPTARG ;;
        H) WORKERS=$OPTARG ;;
        h) HEAVY_COUNT=$OPTARG ;;
        n) HEAVY_NICE=$OPTARG ;;
        N) LIGHT_NICE=$OPTARG ;;
        F) FILLERS=$OPTARG ;;
        C) HOT_CPU=$OPTARG ;;
        r) RUNS=$OPTARG ;;
        o) OUTDIR=$OPTARG ;;
        *) usage ;;
    esac
done

if [ ! -x "$BENCH" ]; then
    echo "Building selection_bench..."
    make -C "$SCRIPT_DIR" selection_bench -s
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root" >&2
    exit 1
fi

SYSCTL_CAMBYSES="/proc/sys/kernel/sched_cambyses"
SYSCTL_DEBUG="/proc/sys/kernel/sched_cambyses_debug"
SYSCTL_BORE="/proc/sys/kernel/sched_bore"
MIGCOST="/sys/kernel/debug/sched/migration_cost_ns"
TRACEFS="/sys/kernel/tracing"

for f in "$SYSCTL_CAMBYSES" "$SYSCTL_DEBUG"; do
    if [ ! -f "$f" ]; then
        echo "Error: $f not found" >&2
        exit 1
    fi
done

# Save original values
ORIG_CAMBYSES=$(cat "$SYSCTL_CAMBYSES")
ORIG_DEBUG=$(cat "$SYSCTL_DEBUG")
ORIG_BORE=$(cat "$SYSCTL_BORE" 2>/dev/null || echo "")
ORIG_MIGCOST=$(cat "$MIGCOST" 2>/dev/null || echo "")

cleanup() {
    echo "$ORIG_CAMBYSES" > "$SYSCTL_CAMBYSES"
    echo "$ORIG_DEBUG" > "$SYSCTL_DEBUG"
    [ -n "$ORIG_BORE" ] && echo "$ORIG_BORE" > "$SYSCTL_BORE" 2>/dev/null || true
    [ -n "$ORIG_MIGCOST" ] && echo "$ORIG_MIGCOST" > "$MIGCOST" 2>/dev/null || true
    echo "Restored original sysctl values."
}
trap cleanup EXIT

mkdir -p "$OUTDIR"

BENCH_ARGS="-T $DURATION -H $WORKERS -h $HEAVY_COUNT -n $HEAVY_NICE -N $LIGHT_NICE -F $FILLERS -C $HOT_CPU"

echo "========================================================"
echo " Selection Quality + Migration Efficiency — A/B Test"
echo "========================================================"
echo
echo "Config: $BENCH_ARGS"
echo "Runs per mode: $RUNS"
echo "Output: $OUTDIR"
echo

# Disable BORE if present
if [ -n "$ORIG_BORE" ]; then
    echo 0 > "$SYSCTL_BORE"
    echo "BORE disabled"
fi

# Enable debug tracing
echo 1 > "$SYSCTL_DEBUG"
echo "Debug tracing enabled"
echo

run_one() {
    local mode=$1    # "cambyses" or "vanilla"
    local run_id=$2
    local prefix="${mode}_run${run_id}"

    # Clear trace buffer
    echo > "$TRACEFS/trace"

    # Run benchmark
    local output
    output=$("$BENCH" $BENCH_ARGS 2>&1)
    echo "$output" > "$OUTDIR/${prefix}_bench.txt"

    # Save trace
    cat "$TRACEFS/trace" > "$OUTDIR/${prefix}_trace.txt"

    # Extract RESULT line
    local result_line
    result_line=$(echo "$output" | grep "^RESULT " || echo "RESULT heavy_first_pct=0 events=0 preference=0 heavy_total=0 light_total=0")
    echo "$result_line" > "$OUTDIR/${prefix}_result.txt"

    # Run analysis
    python3 "$ANALYZE" "$OUTDIR/${prefix}_trace.txt" "${mode} run${run_id}" \
        > "$OUTDIR/${prefix}_analysis.txt" 2>&1

    echo "$output" | grep -E '(Heavy first rate|Total detached|Balance events)' | sed 's/^/    /'
}

# --- Vanilla runs ---
echo ">>> Vanilla CFS (Cambyses OFF)"
echo "----------------------------------------"
echo 0 > "$SYSCTL_CAMBYSES"
sleep 1

for i in $(seq 1 $RUNS); do
    echo "  Run $i/$RUNS:"
    run_one vanilla "$i"
    [ "$i" -lt "$RUNS" ] && sleep 2
done
echo

# --- Cambyses runs ---
echo ">>> Cambyses (ON)"
echo "----------------------------------------"
echo 1 > "$SYSCTL_CAMBYSES"
sleep 1

for i in $(seq 1 $RUNS); do
    echo "  Run $i/$RUNS:"
    run_one cambyses "$i"
    [ "$i" -lt "$RUNS" ] && sleep 2
done
echo

# --- Summary ---
echo "========================================================"
echo " Summary"
echo "========================================================"

summarize() {
    local mode=$1
    local total_detach=0 total_scans=0 total_skip=0 total_iterations=0
    local total_heavy_d=0 total_light_d=0 total_events=0 total_bal_fail=0
    local heavy_first_sum=0

    for i in $(seq 1 $RUNS); do
        local afile="$OUTDIR/${mode}_run${i}_analysis.txt"
        local rfile="$OUTDIR/${mode}_run${i}_result.txt"

        if [ -f "$afile" ]; then
            local detach scans skip iterations
            detach=$(grep "Total detach:" "$afile" | awk '{print $NF}')
            scans=$(grep "Balance scans:" "$afile" | awk '{print $NF}')
            skip=$(grep "SKIP_TO_HEAD" "$afile" | head -1 | awk '{print $NF}')
            iterations=$(grep "Total scan iterations:" "$afile" | awk '{print $NF}')
            heavy_d=$(grep "Heavy detached:" "$afile" | awk '{print $NF}')
            light_d=$(grep "Light detached:" "$afile" | awk '{print $NF}')

            total_detach=$((total_detach + ${detach:-0}))
            total_scans=$((total_scans + ${scans:-0}))
            total_skip=$((total_skip + ${skip:-0}))
            total_iterations=$((total_iterations + ${iterations:-0}))
            bal_fail=$(grep -A1 "Balance failures" "$afile" | grep "Total:" | awk '{print $NF}')
            total_heavy_d=$((total_heavy_d + ${heavy_d:-0}))
            total_light_d=$((total_light_d + ${light_d:-0}))
            total_bal_fail=$((total_bal_fail + ${bal_fail:-0}))
        fi

        if [ -f "$rfile" ]; then
            local hfp events
            hfp=$(sed 's/.*heavy_first_pct=\([0-9.]*\).*/\1/' "$rfile")
            events=$(sed 's/.*events=\([0-9]*\).*/\1/' "$rfile")
            heavy_first_sum=$(awk "BEGIN{printf \"%.1f\", $heavy_first_sum + $hfp}")
            total_events=$((total_events + ${events:-0}))
        fi
    done

    local avg_heavy_first
    avg_heavy_first=$(awk "BEGIN{printf \"%.1f\", $heavy_first_sum / $RUNS}")

    local useful_pct="100.0"
    if [ "$total_iterations" -gt 0 ]; then
        useful_pct=$(awk "BEGIN{printf \"%.1f\", 100.0 * $total_detach / $total_iterations}")
    fi

    local heavy_ratio="0.0"
    local total_d=$((total_heavy_d + total_light_d))
    if [ "$total_d" -gt 0 ]; then
        heavy_ratio=$(awk "BEGIN{printf \"%.1f\", 100.0 * $total_heavy_d / $total_d}")
    fi

    printf "  %-24s %s\n" "Avg heavy first rate:" "${avg_heavy_first}%"
    printf "  %-24s %d\n" "Total balance scans:" "$total_scans"
    printf "  %-24s %d\n" "Total detach:" "$total_detach"
    printf "  %-24s %d\n" "Total scan iterations:" "$total_iterations"
    printf "  %-24s %s\n" "Scan efficiency:" "${useful_pct}%"
    printf "  %-24s %d\n" "SKIP_TO_HEAD:" "$total_skip"
    printf "  %-24s %d / %d (%s%%)\n" "Heavy/Light detached:" "$total_heavy_d" "$total_light_d" "$heavy_ratio"
    printf "  %-24s %d\n" "Balance events:" "$total_events"
    printf "  %-24s %d\n" "Balance failures:" "$total_bal_fail"
}

echo
echo "--- Vanilla CFS ---"
summarize vanilla
echo
echo "--- Cambyses ---"
summarize cambyses
echo
echo "========================================================"
echo "Full results saved to: $OUTDIR"
echo "========================================================"
