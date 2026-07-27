#!/usr/bin/env bash
# A/B the 3-class workload: several always-runnable nice classes, deep runqueues.
# All classes do identical 512KB pointer-chase work; only nice differs, so the
# score gradient comes purely from staleness (low priority waits longer).
# Reports the per-class migration gradient.  Note the gradient exists even with
# both features off (measured 11%/31%/58% across nice 0/5/10) as a by-product
# of task_on_cpu and the budget check -- see README.
set -u
RH="$(cd "$(dirname "$0")" && pwd)"; FEAT=/sys/kernel/debug/sched/features
OUT="$1"; ROUNDS="$2"; shift 2; CFGS="$*"
CLS="${CLS:---class 0:20 --class 5:20 --class 10:20}"; SECS="${SECS:-9}"
setf(){ echo "$1" | sudo tee "$FEAT" >/dev/null; }
apply(){ case "$1" in
  base)   setf NO_LB_ROTATE_BLOCK; setf NO_LB_STRICT_BUDGET;;
  rotate) setf LB_ROTATE_BLOCK;    setf NO_LB_STRICT_BUDGET;;
  budget) setf NO_LB_ROTATE_BLOCK; setf LB_STRICT_BUDGET;;
  both)   setf LB_ROTATE_BLOCK;    setf LB_STRICT_BUDGET;;
esac
  verify "$1"; }
verify(){
  local want_r want_s got
  case "$1" in
    base)   want_r=NO_LB_ROTATE_BLOCK; want_s=NO_LB_STRICT_BUDGET;;
    rotate) want_r=LB_ROTATE_BLOCK;    want_s=NO_LB_STRICT_BUDGET;;
    budget) want_r=NO_LB_ROTATE_BLOCK; want_s=LB_STRICT_BUDGET;;
    both)   want_r=LB_ROTATE_BLOCK;    want_s=LB_STRICT_BUDGET;;
  esac
  got=$(sudo cat "$FEAT" | tr ' ' '\n' | grep -E '^(NO_)?LB_(ROTATE_BLOCK|STRICT_BUDGET)$' | tr '\n' ' ')
  for w in $want_r $want_s; do
    case " $got " in *" $w "*) ;; *) echo "FEATVERIFY-FAIL cfg=$1 want=$w got=[$got]";; esac
  done
}
rotate(){ local n="$1"; shift; local a=("$@") m=${#a[@]} i out=""
  for ((i=0;i<m;i++)); do out+="${a[(i+n)%m]} "; done; echo "$out"; }
{
echo "###### CLSAB-START gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) cfgs=[$CFGS] N=$ROUNDS cls=[$CLS] ######"
apply "$(echo $CFGS|awk '{print $1}')"; "$RH/mixbench" $CLS --secs 4 --sleep-us 0 >/dev/null 2>&1
r=0; while [ "$r" -lt "$ROUNDS" ]; do
  for cfg in $(rotate "$r" $CFGS); do apply "$cfg"; echo -n "R$r $cfg "
    "$RH/mixbench" $CLS --secs $SECS --sleep-us 0; done
  r=$((r+1)); done
echo "###### CLSAB-DONE ######"
} | tee "$OUT"
