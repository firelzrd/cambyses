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

## 2. `LB_CONCEDE_CHEAPEST` — size candidates against the budget, not a failure count

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

**Idle destinations only (0.7.0).** Handing an idle CPU work is worth an
overshoot, so the concession applies when the balance pulls into an idle CPU
(`env->idle != __CPU_NOT_IDLE`, newidle and periodic idle). A busy destination
gains nothing from an overshoot and can flip the imbalance and bounce the task
back, so it keeps the upstream `nr_balance_failed` relaxation. The test uses
`env->idle`, which is fixed for the whole balance, not `idle_cpu(dst)`, which a
wakeup can change between candidates.

The feature was called `LB_STRICT_BUDGET` up to 0.6.0. "Strict" was a misnomer:
the change is eager (it concedes at once), not strict.

### Evidence (measured with 0.6.0)

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

These runs were made before the idle-only rule, when the concession applied to
every destination. Re-checked for 0.7.0 on Ryzen AI MAX+ 395 (32 threads, 2 L3,
Linux 6.18.62, 48 hogs + `schbench` probe, 10 order-rotated rounds): the probe
gain is unchanged by the idle-only rule (probe p99 and RPS the same as with the
concession everywhere), and the concessions made for busy destinations (about
900 per run) drop to zero. Migration counts and throughput of burst and
`stress-ng` matrix workloads are unchanged — the avoidable migrations there are
a few per second.

---

## Usage

Apply the patch for your kernel from `patches/` to a clean tree:
`0001-6.12.74-cambyses-v0.7.0.patch`, `0001-6.18.22-cambyses-v0.7.0.patch` or
`0001-7.3.0-rc5-cambyses-v0.7.0.patch`. Both features default on:

```sh
echo NO_LB_ROTATE_BLOCK     > /sys/kernel/debug/sched/features
echo NO_LB_CONCEDE_CHEAPEST > /sys/kernel/debug/sched/features
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
one NUMA node, `governor=performance`, synthetic workloads. 0.7.0 was also
checked on a Ryzen AI MAX+ 395 (two L3 domains, `governor=powersave`). NUMA
topologies — where migration is far more expensive — are untested, as are real
workloads such as a kernel build.

Neither patch references BORE or POC Selector; both operate on stock CFS.

On Linux 7.3 the balancer also has cache-aware scheduling (`LBF_LLC_PINNED`),
which does not count an LLC-locality refusal in `nr_balance_failed`. For a busy
destination, where this patch keeps the upstream relaxation, that relaxation
therefore advances more slowly there than on 6.12. The LLC check runs before
the budget test, so a task it refuses is never a concession candidate.

## License

GPL-2.0
