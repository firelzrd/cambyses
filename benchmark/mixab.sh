#!/usr/bin/env bash
# A/B mixbench across sched_feat configs.  Same boot, order-rotated, warmup
# discarded.  Primary metric is heavy_mig_share -- the direct mechanism
# prediction: ranking should move the expensive class less often.
set -u
RH="$(cd "$(dirname "$0")" && pwd)"
FEAT=/sys/kernel/debug/sched/features
OUT="$1"; ROUNDS="$2"; shift 2; CFGS="$*"
HEAVY="${HEAVY:-12}"; LIGHT="${LIGHT:-12}"; SECS="${SECS:-10}"; HKB="${HKB:-512}"

setf(){ echo "$1" | sudo tee "$FEAT" >/dev/null; }
apply(){ case "$1" in
  base)   setf NO_LB_ROTATE_BLOCK; setf NO_LB_STRICT_BUDGET;;
  rotate) setf LB_ROTATE_BLOCK;    setf NO_LB_STRICT_BUDGET;;
  budget) setf NO_LB_ROTATE_BLOCK; setf LB_STRICT_BUDGET;;
  both)   setf LB_ROTATE_BLOCK;    setf LB_STRICT_BUDGET;;
esac; }
rotate(){ local n="$1"; shift; local a=("$@") m=${#a[@]} i out=""
  for ((i=0;i<m;i++)); do out+="${a[(i+n)%m]} "; done; echo "$out"; }

{
echo "###### MIXAB-START gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) cfgs=[$CFGS] N=$ROUNDS heavy=$HEAVY light=$LIGHT hkb=$HKB secs=$SECS ######"
apply "$(echo $CFGS | awk '{print $1}')"
"$RH/mixbench" --heavy $HEAVY --light $LIGHT --secs 4 --sleep-us 0 --heavy-kb $HKB >/dev/null 2>&1
r=0
while [ "$r" -lt "$ROUNDS" ]; do
  for cfg in $(rotate "$r" $CFGS); do
    apply "$cfg"
    echo -n "R$r $cfg "
    "$RH/mixbench" --heavy $HEAVY --light $LIGHT --secs $SECS --sleep-us 0 --heavy-kb $HKB
  done
  r=$((r+1))
done
echo "###### MIXAB-DONE ######"
} | tee "$OUT"
