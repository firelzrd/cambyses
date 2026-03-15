#!/bin/bash
# Run asymmetric_drain with Cambyses on vs off and compare results.
#
# Usage: sudo ./run.sh [asymmetric_drain options...]
#        sudo ./run.sh --sweep-w1 1,2,3 [asymmetric_drain options...]
#
# Requires root for sysctl.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="$SCRIPT_DIR/asymmetric_drain"
SWEEP_W1=""

# Parse our own flags before passing the rest to the benchmark
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sweep-w1)
            SWEEP_W1="$2"
            shift 2
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

if [ ! -x "$BENCH" ]; then
    echo "Building asymmetric_drain..."
    make -C "$SCRIPT_DIR" -s
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root (needed for sysctl)" >&2
    exit 1
fi

SYSCTL_ENABLED="/proc/sys/kernel/sched_cambyses_enabled"
SYSCTL_W1="/proc/sys/kernel/sched_cambyses_w1"

if [ ! -f "$SYSCTL_ENABLED" ]; then
    echo "Error: $SYSCTL_ENABLED not found — is Cambyses built into the kernel?" >&2
    exit 1
fi

# Save original state
ORIG_ENABLED=$(cat "$SYSCTL_ENABLED")
ORIG_W1=""
if [ -f "$SYSCTL_W1" ]; then
    ORIG_W1=$(cat "$SYSCTL_W1")
fi

run_bench() {
    "$BENCH" "${ARGS[@]}"
}

echo "========================================"
echo " Asymmetric Drain — A/B Comparison"
echo "========================================"
echo

# --- Vanilla (FIFO) ---
echo ">>> Vanilla (Cambyses disabled)"
echo "----------------------------------------"
sysctl -q kernel.sched_cambyses_enabled=0
run_bench
echo
sleep 2

# --- Cambyses runs ---
if [ -n "$SWEEP_W1" ] && [ -f "$SYSCTL_W1" ]; then
    IFS=',' read -ra W1_VALS <<< "$SWEEP_W1"
    for w1 in "${W1_VALS[@]}"; do
        echo ">>> Cambyses (w1=$w1)"
        echo "----------------------------------------"
        sysctl -q kernel.sched_cambyses_enabled=1
        sysctl -q kernel.sched_cambyses_w1="$w1"
        run_bench
        echo
        sleep 2
    done
else
    echo ">>> Cambyses (w1=${ORIG_W1:-default})"
    echo "----------------------------------------"
    sysctl -q kernel.sched_cambyses_enabled=1
    run_bench
    echo
fi

# Restore original state
sysctl -q kernel.sched_cambyses_enabled="$ORIG_ENABLED"
if [ -n "$ORIG_W1" ] && [ -f "$SYSCTL_W1" ]; then
    sysctl -q kernel.sched_cambyses_w1="$ORIG_W1"
fi

echo "========================================"
echo " Done."
echo "========================================"
