#!/usr/bin/env bash
# Sample the spread of se.avg.runnable_avg across the runnable threads of a
# workload -- i.e. how heterogeneous the migration candidate pool actually is.
# Use it to check a workload is not unimodal before drawing conclusions from
# it: measuring a selection change on a pool with no spread proves nothing.
set -u
LABEL="$1"; shift
"$@" >/dev/null 2>&1 &
WL=$!
sleep 3
{
  for tid in $(ps -L -o tid= --ppid $WL 2>/dev/null; ps -L -o tid= -p $WL 2>/dev/null); do
    v=$(grep -m1 'se.avg.runnable_avg' /proc/$tid/sched 2>/dev/null | awk '{print $3}')
    [ -n "${v:-}" ] && echo "$v"
  done
} | sort -n > /tmp/.spread.$$
kill $WL 2>/dev/null; wait $WL 2>/dev/null

python3 - "$LABEL" /tmp/.spread.$$ <<'PY'
import sys
from statistics import median, pstdev
label, path = sys.argv[1], sys.argv[2]
v = [int(x) for x in open(path) if x.strip()]
if len(v) < 2:
    print(f"{label:<34} (only {len(v)} threads sampled)"); raise SystemExit
lo, hi, med = min(v), max(v), median(v)
cv = pstdev(v) / (sum(v)/len(v)) * 100 if sum(v) else 0
print(f"{label:<34} n={len(v):<4} min={lo:<5} med={med:<6.0f} max={hi:<5} "
      f"spread={hi-lo:<5} CV={cv:.0f}%")
PY
rm -f /tmp/.spread.$$
