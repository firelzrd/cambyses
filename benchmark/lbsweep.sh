#!/usr/bin/env bash
# Real-HW A/B harness for the detach_tasks() load-balance changes.
#
# Same boot, sched_feat toggling only -- no reboot confound.  Follows the
# protocol established in RESULTS.md: warmup discard, order rotation across
# rounds, governor=performance, N rounds of a fixed battery per config.
#
# Usage: lbsweep.sh <outlog> <N_rounds> <cfg1> [cfg2 ...]
#   cfgs: base rotate budget both
set -u
RH="$(cd "$(dirname "$0")" && pwd)"
SCH="${SCH:-$(command -v schbench 2>/dev/null || echo "$HOME/.local/bin/schbench")}"
FEAT=/sys/kernel/debug/sched/features
OUT="$1"; ROUNDS="$2"; shift 2; CFGS="$*"
FPS="${FPS:-256 1024}"

setf(){ echo "$1" | sudo tee "$FEAT" >/dev/null; }
apply(){
  case "$1" in
    base)   setf NO_LB_ROTATE_BLOCK; setf NO_LB_STRICT_BUDGET;;
    rotate) setf LB_ROTATE_BLOCK;    setf NO_LB_STRICT_BUDGET;;
    budget) setf NO_LB_ROTATE_BLOCK; setf LB_STRICT_BUDGET;;
    both)   setf LB_ROTATE_BLOCK;    setf LB_STRICT_BUDGET;;
  esac
}

battery(){
  local cfg="$1" r="$2" fp t
  apply "$cfg"
  for fp in $FPS; do
    echo "SCHB_BEGIN $cfg.F${fp}.r$r"
    "$RH/lbstat.py" snap > /tmp/.lb_a
    $SCH -m 2 -t 8 -F "$fp" -w 2 -r 5 2>&1
    "$RH/lbstat.py" snap > /tmp/.lb_b
    "$RH/lbstat.py" diff /tmp/.lb_a /tmp/.lb_b "$cfg.F${fp}.r$r"
    echo "SCHB_END $cfg.F${fp}.r$r"
  done
  "$RH/lbstat.py" snap > /tmp/.lb_a
  t=$(hackbench -g 4 -l 5000 -s 100 2>/dev/null | awk '/Time/{print $2}')
  "$RH/lbstat.py" snap > /tmp/.lb_b
  echo "HB $cfg.r$r socket=$t"
  "$RH/lbstat.py" diff /tmp/.lb_a /tmp/.lb_b "$cfg.HB.r$r"
}

rotate(){ local n="$1"; shift; local a=("$@") m=${#a[@]} i out=""
  for ((i=0;i<m;i++)); do out+="${a[(i+n)%m]} "; done; echo "$out"; }

{
echo "###### LBSWEEP-START gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) cfgs=[$CFGS] N=$ROUNDS fps=[$FPS] kern=$(uname -r) ######"
first=$(echo $CFGS | awk '{print $1}')
apply "$first"; $SCH -m2 -t8 -F512 -w1 -r3 >/dev/null 2>&1   # warmup, discarded
r=0
while [ "$r" -lt "$ROUNDS" ]; do
  for cfg in $(rotate "$r" $CFGS); do battery "$cfg" "$r"; done
  r=$((r+1))
done
echo "###### LBSWEEP-DONE ######"
} | tee "$OUT"
