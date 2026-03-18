#!/bin/bash
# A/B comparison of response_lat with Cambyses on vs off.
#
# Usage: sudo ./run_response_lat.sh [response_lat options...]
#
# Requires root for sysctl.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="$SCRIPT_DIR/response_lat"

if [ ! -x "$BENCH" ]; then
    echo "Building response_lat..."
    make -C "$SCRIPT_DIR" response_lat -s
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root (needed for sysctl)" >&2
    exit 1
fi

SYSCTL="/proc/sys/kernel/sched_cambyses"
if [ ! -f "$SYSCTL" ]; then
    echo "Error: $SYSCTL not found — is Cambyses built into the kernel?" >&2
    exit 1
fi

ORIG=$(cat "$SYSCTL")

extract() {
    local field="$1"
    grep "^RESULT " | head -1 | sed "s/.*${field}=\\([0-9]*\\).*/\\1/"
}

NCPUS=$(nproc)
echo "========================================"
echo " Response Latency — A/B Comparison"
echo " (asymmetric load, $NCPUS CPUs)"
echo "========================================"
echo

echo ">>> Vanilla (Cambyses disabled)"
echo "----------------------------------------"
sysctl -q kernel.sched_cambyses=0
sleep 1
VANILLA_OUT=$("$BENCH" "$@" 2>&1 | tee /dev/stderr)
V_P50=$(echo "$VANILLA_OUT" | extract p50)
V_P95=$(echo "$VANILLA_OUT" | extract p95)
V_P99=$(echo "$VANILLA_OUT" | extract p99)
V_MAX=$(echo "$VANILLA_OUT" | extract max)
V_IDEAL=$(echo "$VANILLA_OUT" | extract ideal)
echo

sleep 3

echo ">>> Cambyses (enabled)"
echo "----------------------------------------"
sysctl -q kernel.sched_cambyses=1
sleep 1
CAMBYSES_OUT=$("$BENCH" "$@" 2>&1 | tee /dev/stderr)
C_P50=$(echo "$CAMBYSES_OUT" | extract p50)
C_P95=$(echo "$CAMBYSES_OUT" | extract p95)
C_P99=$(echo "$CAMBYSES_OUT" | extract p99)
C_MAX=$(echo "$CAMBYSES_OUT" | extract max)
echo

sysctl -q kernel.sched_cambyses="$ORIG"

echo "========================================"
echo " Comparison (latency — lower is better)"
echo "========================================"
if [ -n "$V_P50" ] && [ -n "$C_P50" ]; then
    awk -v vp50="$V_P50" -v cp50="$C_P50" \
        -v vp95="$V_P95" -v cp95="$C_P95" \
        -v vp99="$V_P99" -v cp99="$C_P99" \
        -v vmax="$V_MAX" -v cmax="$C_MAX" \
        -v ideal="$V_IDEAL" \
    'BEGIN {
        printf "  Metric   Vanilla       Cambyses      Ratio\n"
        printf "  ------   -------       --------      -----\n"
        printf "  ideal    %7sus\n", ideal
        printf "  p50      %7sus       %7sus       %.2fx\n", vp50, cp50, cp50/vp50
        printf "  p95      %7sus       %7sus       %.2fx\n", vp95, cp95, cp95/vp95
        printf "  p99      %7sus       %7sus       %.2fx\n", vp99, cp99, cp99/vp99
        printf "  max      %7sus       %7sus       %.2fx\n", vmax, cmax, cmax/vmax
        printf "\n"
        if (cp99 < vp99) {
            pct = (vp99 - cp99) / vp99 * 100
            printf "  --> Cambyses p99 latency is %.1f%% lower\n", pct
        } else if (cp99 > vp99) {
            pct = (cp99 - vp99) / vp99 * 100
            printf "  --> Cambyses p99 latency is %.1f%% higher (no improvement)\n", pct
        } else {
            printf "  --> No difference detected\n"
        }
    }'
else
    echo "  (Could not parse result values)"
fi
echo "========================================"
echo " Done."
echo "========================================"
