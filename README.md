# Cambyses

Two changes to the CFS load balancer's `detach_tasks()`, the function that
decides which task to pull off a busy runqueue.

This is what remains of a much larger project. The original attempt replaced
the balancer's task selection with a scored pipeline backed by a side array of
task pointers, shadowed copies of scheduler state in `struct rq`, and SIMD
argmax. That was measured on real hardware and abandoned — see
[What was tried and dropped](#what-was-tried-and-dropped). What survived is
smaller than the machinery built to support it: two patches, ~110 lines, no
new struct fields, no hot-path hooks, no tunables.

Both are behind `sched_feat()` so they can be compared at runtime without a
reboot.

---

## 1. `LB_ROTATE_BLOCK` — advance the scan window as a block

`detach_tasks()` walks `cfs_tasks` from the tail and moves every task that
fails `can_migrate_task()` to the head of the list, one at a time. That does
two jobs at once: it steps past the reject, and it leaves the list rotated so
the next balance pass starts on tasks this one did not reach.

Only the second job needs the list touched at all. Doing it per reject costs
one list operation per rejected task with the rq lock held, and because only
rejects are promoted, the resulting order is a function of migration outcome.

Walk with a cursor instead and rotate the examined block to the head once, at
the end of the scan. Coverage is unchanged — a later pass still resumes where
this one stopped — but the cost drops to one list operation per scan.

A userspace model of the loop (same list primitives, same structure, under
ASan/UBSan) shows identical coverage across runqueue sizes and scan budgets,
with list operations down 64% in a steady-state mix and 85% when few tasks are
movable. It does not reproduce any difference in where rejects end up sitting
in the list, so this is a cost reduction rather than an ordering fix.

**Real hardware: no measurable effect either way, no regression.** Expected for
a cost reduction of this size.

## 2. `LB_STRICT_BUDGET` — size candidates against the budget, not a failure count

A migration candidate whose cost exceeds the remaining imbalance is rejected.
Upstream relaxes that comparison by `sd->nr_balance_failed`, halving the
candidate's apparent cost once per recorded balance failure, so an oversized
task is eventually let through.

**That counter is reset on any successful migration at the domain.** A steady
supply of cheap-to-move tasks therefore keeps resetting it, and the relaxation
never reaches the expensive candidates — they are only considered once nothing
cheaper is left. The relaxation is per-domain and history-dependent, but the
question it answers ("is this task too big for the budget?") is per-task and
local to the scan.

Removing the relaxation on its own is not safe: it is also the only route by
which an over-budget task ever moves under `migrate_load`/`migrate_util`,
because `imbalanced_active_balance()` escalates for `migrate_task` only.
Modelling that showed the source left 34–98% skewed. So the implicit guarantee
is replaced with an explicit one:

> **A scan that admits nothing concedes to the smallest overshoot it saw.**

Progress is then guaranteed by the candidates actually present rather than by a
counter other tasks keep clearing, and it acts on the first scan that admits
nothing instead of once enough failures have been recorded.

### Evidence

Mixed saturated workload (24–64 CPU hogs oversubscribing 16 CPUs, light
`schbench` probe measuring latency), same boot, `sched_feat` toggling only,
order-rotated, warmup discarded, N=10:

| run | Request p99 | probe RPS |
|---|---|---|
| 1 | −9.3% (10/10, p=0.002) | +14.5% (10/10, p=0.002) |
| 2 | −6.3% (9/9, p=0.004) | +7.8% (9/10, p=0.021) |
| 3 | −8.4% (9/9, p=0.004) | +14.0% (9/10, p=0.021) |

Three independent replications, exact sign test and exact paired permutation
test. Hog-side throughput (`stress-ng` bogo-ops/s) was −0.9% to −1.2%, not
significant — **the probe's gain is not taken from the hogs.**

Mechanism (`/proc/schedstat`): LLC-domain newidle migrations **+17.7%**,
failures −5.4% — the concession converting give-ups into migrations, as
designed.

The effect is regime-specific: absent in homogeneous workloads, absent when the
probe does no work, present when the probe does real work under saturation.

---

## Usage

Apply `patches/6.12/0001-6.12.74-cambyses-v0.6.0.patch` to a 6.12.74 tree.
Both features default on:

```sh
echo NO_LB_ROTATE_BLOCK  > /sys/kernel/debug/sched/features
echo NO_LB_STRICT_BUDGET > /sys/kernel/debug/sched/features
```

Requires `CONFIG_SCHED_DEBUG=y` and `CONFIG_JUMP_LABEL=y` for the runtime
toggle; otherwise the features compile to constants.

Build the workload generator with `make -C benchmark`.  `benchmark/` holds the
harnesses used above — `lbsweep.sh` (homogeneous),
`lbmix2.sh` (mixed heavy+light with hog-side throughput), `clsab.sh` +
`mixbench.c` (multi-class deep-runqueue), `lbstat.py` (schedstat deltas),
`lbstats.py` (summary), `lbsig.py` (paired significance tests). `RESULTS.md`
has the full measurement record.

## Scope

Measured on one machine: Ryzen 7 7840HS, 8C/16T, single CCD, 16 MB shared L3,
one NUMA node, `governor=performance`, synthetic workloads. Cross-CCD and NUMA
topologies — where migration is far more expensive — are untested, as are real
workloads such as a kernel build.

Neither patch references BORE or POC Selector; both operate on stock CFS.

## License

GPL-2.0
