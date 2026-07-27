#!/usr/bin/env bash
# Mixed heavy+light A/B for the detach_tasks() changes.
#
# As lbmix.sh, plus hog-side throughput (stress-ng bogo-ops/s), so a probe gain
# that merely redistributes CPU away from the saturating load is visible rather
# than being reported as a win.  24 hogs oversubscribe 16 CPUs while a light
# schbench probe measures latency:
#
#   probe -F0   : probe median 152 vs hog 1024  -> 6.7x separation
#   probe -F256 : probe median ~870 vs hog 1024 -> 1.2x (near-unimodal control)
#
# If ranking helps, it should show at F0 and not at F256.
set -u
RH="$(cd "$(dirname "$0")" && pwd)"
SCH="${SCH:-$(command -v schbench 2>/dev/null || echo "$HOME/.local/bin/schbench")}"
FEAT=/sys/kernel/debug/sched/features
OUT="$1"; ROUNDS="$2"; shift 2; CFGS="$*"
FPS="${FPS:-0 256}"
HOGS="${HOGS:-24}"
RUN="${RUN:-8}"

setf(){ echo "$1" | sudo tee "$FEAT" >/dev/null; }
apply(){ case "$1" in
  base)   setf NO_LB_ROTATE_BLOCK; setf NO_LB_STRICT_BUDGET;;
  rotate) setf LB_ROTATE_BLOCK;    setf NO_LB_STRICT_BUDGET;;
  budget) setf NO_LB_ROTATE_BLOCK; setf LB_STRICT_BUDGET;;
  both)   setf LB_ROTATE_BLOCK;    setf LB_STRICT_BUDGET;;
esac; }

battery(){
  local cfg="$1" r="$2" fp sg
  apply "$cfg"
  for fp in $FPS; do
    stress-ng --cpu "$HOGS" --cpu-method matrixprod -t $((RUN + 3)) \
        --metrics-brief > /tmp/.hog.$$ 2>&1 &
    sg=$!
    sleep 2                                   # let the hogs saturate
    echo "SCHB_BEGIN $cfg.F${fp}.r$r"
    "$RH/lbstat.py" snap > /tmp/.lbm_a
    $SCH -m 1 -t 4 -F "$fp" -w 2 -r "$RUN" 2>&1
    "$RH/lbstat.py" snap > /tmp/.lbm_b
    "$RH/lbstat.py" diff /tmp/.lbm_a /tmp/.lbm_b "$cfg.F${fp}.r$r"
    echo "SCHB_END $cfg.F${fp}.r$r"
    wait "$sg" 2>/dev/null
    echo "HOG $cfg.F${fp}.r$r bogo=$(awk '/ cpu /{print $(NF-1)}' /tmp/.hog.$$ | tail -1)"
    rm -f /tmp/.hog.$$
    sleep 1
  done
}

rotate(){ local n="$1"; shift; local a=("$@") m=${#a[@]} i out=""
  for ((i=0;i<m;i++)); do out+="${a[(i+n)%m]} "; done; echo "$out"; }

{
echo "###### LBMIX2-START gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) cfgs=[$CFGS] N=$ROUNDS fps=[$FPS] hogs=$HOGS kern=$(uname -r) ######"
first=$(echo $CFGS | awk '{print $1}'); apply "$first"
stress-ng --cpu "$HOGS" --cpu-method matrixprod -t 8 >/dev/null 2>&1 &
w=$!; sleep 1; $SCH -m1 -t4 -F0 -w1 -r4 >/dev/null 2>&1; kill $w 2>/dev/null; wait $w 2>/dev/null
r=0
while [ "$r" -lt "$ROUNDS" ]; do
  for cfg in $(rotate "$r" $CFGS); do battery "$cfg" "$r"; done
  r=$((r+1))
done
echo "###### LBMIX2-DONE ######"
} | tee "$OUT"
