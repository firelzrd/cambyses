#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A/B of LB_CONCEDE_CHEAPEST on the idle-fill case (see fill.c).
#
# Builds a cpuset root partition of four CPUs, two from each of two LLCs, so
# the balancer has an MC domain per LLC and a domain above them that crosses
# LLCs.  Then, per block, runs every test condition with NO_ and
# LB_CONCEDE_CHEAPEST in ABBA order and writes every trial to LOG.  Summarize
# with fillsum.py, draw with mkfig.py.
#
# usage: sudo ./fillab.sh [LOG] [BLOCKS]
#   CPUS="A0 A1 B0 B1"   pick the CPUs yourself (A0,A1 in one LLC, B0,B1 in
#                        another, preferably not SMT siblings)
#   TESTS="lat tput"     which tests to run
#   DUTIES="6/4 4/6"     run/sleep ms of the duty task, latency test
#   TICKS="0 100"        ticker period in us (0 = no ticker)
#   FREES="50 100 200"   free phase in ms, throughput test (duty 6/4)
#   TRIALS=50 CYCLES=30  per run
#
# Needs root, cgroup v2 with the cpuset controller, debugfs, and a kernel
# with CONFIG_SCHED_DEBUG so the feature can be toggled.  Needs at least two
# LLCs; on a single-LLC machine there is no cross-LLC balance to measure.
set -eu
cd "$(dirname "$0")"
LOG=${1:-fillab.log}; BLOCKS=${2:-6}
TESTS=${TESTS:-"lat tput"}
DUTIES=${DUTIES:-"6/4 4/6"}
TICKS=${TICKS:-"0 100"}
FREES=${FREES:-"50 100 200"}
TRIALS=${TRIALS:-50}; CYCLES=${CYCLES:-30}
FEAT=/sys/kernel/debug/sched/features
CG=/sys/fs/cgroup/cambyses-fill

[ -w "$FEAT" ] || { echo "need root and $FEAT (CONFIG_SCHED_DEBUG, debugfs)"; exit 1; }
grep -qw LB_CONCEDE_CHEAPEST "$FEAT" || { echo "kernel has no LB_CONCEDE_CHEAPEST"; exit 1; }
[ -x ./fill ] || make fill

if [ -z "${CPUS:-}" ]; then
	# First two primary SMT threads of the first two L3 domains.
	CPUS=$(python3 - <<'EOF'
import glob
def parse(s):
    out = []
    for part in s.strip().split(','):
        a, _, b = part.partition('-')
        out += range(int(a), int(b or a) + 1)
    return out
l3 = {}
for d in sorted(glob.glob('/sys/devices/system/cpu/cpu[0-9]*'), key=lambda p: int(p.rsplit('cpu', 1)[1])):
    c = int(d.rsplit('cpu', 1)[1])
    try:
        dom = tuple(parse(open(d + '/cache/index3/shared_cpu_list').read()))
        sib = parse(open(d + '/topology/thread_siblings_list').read())
    except OSError:
        continue
    if sib[0] == c:
        l3.setdefault(dom, []).append(c)
groups = [v[:2] for v in l3.values() if len(v) >= 2]
if len(groups) < 2:
    raise SystemExit('need two LLCs with two cores each; set CPUS=')
print(*groups[0], *groups[1])
EOF
)
fi
set -- $CPUS
[ $# = 4 ] || { echo "CPUS must name four CPUs"; exit 1; }
A0=$1; A1=$2; B0=$3; B1=$4

old_feat=$(grep -ow 'NO_LB_CONCEDE_CHEAPEST\|LB_CONCEDE_CHEAPEST' "$FEAT")
cleanup() {
	echo "$old_feat" > "$FEAT"
	[ -d "$CG" ] && rmdir "$CG" 2>/dev/null || true
}
trap cleanup EXIT

grep -qw cpuset /sys/fs/cgroup/cgroup.subtree_control ||
	echo +cpuset > /sys/fs/cgroup/cgroup.subtree_control
mkdir -p "$CG"
echo "$A0,$A1,$B0,$B1" > "$CG/cpuset.cpus"
echo root > "$CG/cpuset.cpus.partition"
part=$(cat "$CG/cpuset.cpus.partition")
[ "$part" = root ] || { echo "partition not valid: $part"; exit 1; }

# run TAG TICK ARGS...: one fill run per feature setting, in the given order
run() {
	local tag=$1 tick=$2 f; shift 2
	for f in $order; do
		echo "$f" > "$FEAT"
		bash -c "echo \$\$ > $CG/cgroup.procs; TICK_US=$tick exec ./fill $A0 $A1 $B0 $B1 $*" |
			sed "s|^|block=$b feat=$f $tag tick=$tick |" >> "$LOG"
		tail -1 "$LOG"
	done
}

echo "# fillab $(uname -r) cpus=$A0,$A1|$B0,$B1 blocks=$BLOCKS tests=$TESTS" | tee "$LOG"
for b in $(seq 0 $((BLOCKS - 1))); do
	if [ $((b % 2)) = 0 ]; then order="NO_LB_CONCEDE_CHEAPEST LB_CONCEDE_CHEAPEST"
	else order="LB_CONCEDE_CHEAPEST NO_LB_CONCEDE_CHEAPEST"; fi
	for t in $TICKS; do
		case " $TESTS " in *" lat "*)
			for d in $DUTIES; do
				run "test=lat duty=$d" "$t" "${d%/*} ${d#*/} lat $TRIALS"
			done;;
		esac
		case " $TESTS " in *" tput "*)
			for fr in $FREES; do
				run "test=tput free=$fr" "$t" "6 4 tput $fr $CYCLES"
			done;;
		esac
	done
done
