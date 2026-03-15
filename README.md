# Cambyses

**Context-Aware Migration Balancer Yielding Scored Entity Selection**

A Linux kernel patch that replaces the default FIFO task selection in the CFS load balancer with scoring-based optimal candidate selection, improving the quality of task migration decisions.

## Problem

The CFS load balancer's `detach_tasks()` scans the `cfs_tasks` list from the tail and unconditionally picks the first task that passes `can_migrate_task()`. There is no comparison of which task is most suitable for migration. The cache-hot check is binary (migrate or not) and does not consider the *degree* of migration cost.

This means that in a runqueue with a mix of cache-hot, cache-cold, heavy, and light tasks, the load balancer may migrate a cache-hot lightweight task when a cache-cold heavyweight task would have been a far better choice — resolving the imbalance more efficiently with lower migration cost.

## Approach

Cambyses introduces a 3-phase pipeline that collects, scores, and ranks migration candidates before detaching:

1. **Phase 1 — Sampling**: Collect up to `loop_max` candidates that pass `can_migrate_task()`, compute a migration score for each, and filter by imbalance budget eligibility.
2. **Phase 2 — Sorting**: Sort candidates in descending score order using a fixed-size bitonic sort network (19 comparators for <= 8 candidates, 63 for <= 16, 159 for <= 32). Comparison counts are compile-time determined with no branch mispredictions.
3. **Phase 3 — Detach**: Detach candidates in score order while consuming the imbalance budget, maintaining the same imbalance semantics as the vanilla path.

Both the pull path (`detach_tasks()`) and push path (`detach_one_task()`) are supported. The push path uses a simple max search since only one task is migrated.

All Cambyses paths run under the existing `rq_lock_irqsave` — no additional locking is required.

## Scoring

Four features are extracted per candidate and combined into a weighted linear score:

| Feature | Computation | Meaning | Typical range |
|---------|-------------|---------|---------------|
| F0: cache_coldness | `log2p1(rq_clock_task - exec_start)` | Time since last execution — colder cache means lower migration cost | 40–130 |
| F1: load_contribution | `log2p1(task_h_load(p))` | Hierarchical load weight — heavier tasks resolve imbalance more efficiently | 0–66 |
| F2: vol_switch_ratio | `log2p1(nvcsw) - log2p1(total) + 64` | Voluntary context switch ratio — I/O-bound tasks have transient cache footprint, cheaper to migrate | 0–64 |
| F3: wakee_penalty | `log2p1(wakee_flips + 1)` | Wakee relationship complexity — frequent wakee switches indicate migration risk | 0–26 |

F0, F1, and F3 are compressed via `log2p1_u64_u8fp2()` — a fixed-point log2 with integer part + 2-bit mantissa, derived from the [BORE scheduler](https://github.com/firelzrd/bore-scheduler)'s `log2p1_u64_u32fp()`. This provides 4x the resolution of `fls64()` (260 levels vs 65) within a single `u8`, at the cost of just a CLZ + 2 shifts. F2 also uses `log2p1` internally — computing `log2(nvcsw) - log2(total)` to approximate the ratio without division.

Score formula: `score = w0*F0 + w1*F1 + w2*F2 - w3*F3`

Default weights (`w0=1, w1=3, w2=1, w3=1`) make load contribution the dominant factor — migrating one heavy task is more efficient than multiple light tasks in terms of migration count, lock contention, and IPIs. Cache coldness acts as a secondary selector among tasks of similar load, while voluntary switch ratio and wakee penalty serve as tiebreakers.

Score range fits in `s16`: max ~392, min ~-26 with default weights.

## SIMD Acceleration

When `CONFIG_SCHED_CAMBYSES_SIMD` is enabled, a two-tier dispatch selects the optimal scoring and sorting path based on candidate count and CPU capabilities:

**AVX2 path** (8 candidates/ymm register):
- Scoring: `VPMADDUBSW` + `VPHADDW` — 2 instructions score 8 candidates simultaneously
- Sorting: SIMD bitonic sort using `VPMINSD`/`VPMAXSD` on s32-packed `(score << 16 | index)` values, tracking scores and indices in a single comparison

**SSSE3 path** (4 candidates/xmm register):
- Scoring: `PMADDUBSW` + `PHADDW`
- Sorting: Scalar bitonic sort (SSSE3 lacks `PMINSD`, which is SSE4.1)

**Scalar path** (candidates <= 8):
- No FPU context switch needed — avoids the ~40-60ns `kernel_fpu_begin/end` overhead
- Bitonic sort network with 19 comparators is fast enough (~25ns for 8 candidates)

SIMD variants are compiled in separate translation units with ISA-specific flags (`-mavx2`, `-mssse3`) to prevent the compiler from auto-vectorizing scalar code with SIMD instructions, following the same pattern as the [Nap scheduler](https://github.com/nicman23/nap). Runtime ISA detection uses `boot_cpu_has()` with static keys for zero-cost dispatch.

Features are packed in AoS (Array of Structures) layout — 4 bytes per candidate — matching the `PMADDUBSW` input format for direct register loads without transposition.

## Configuration

### Build

| Option | Description |
|--------|-------------|
| `CONFIG_SCHED_CAMBYSES` | Core Cambyses functionality. Depends on `SMP`. Default `y`. |
| `CONFIG_SCHED_CAMBYSES_SIMD` | SIMD batch scoring acceleration. Depends on `SCHED_CAMBYSES` and `X86_64` or `ARM64`. Default `n`. |

### Runtime (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_cambyses_enabled` | 1 | Enable/disable via static key. When set to 0, all Cambyses paths are NOPed out at zero cost, falling back to the vanilla FIFO path. |
| `kernel.sched_cambyses_w0` | 1 | Cache coldness weight (0–3) |
| `kernel.sched_cambyses_w1` | 3 | Load contribution weight (0–3) |
| `kernel.sched_cambyses_w2` | 1 | Voluntary switch ratio weight (0–3) |
| `kernel.sched_cambyses_w3` | 1 | Wakee penalty weight (0–3) |

## Performance

Worst-case overhead relative to the ~4ms tick interval:

| Path | 32 candidates (worst case) | 8 candidates (typical) |
|------|---------------------------|------------------------|
| Scalar | ~514ns | ~113ns |
| SSSE3 | ~503ns | — |
| AVX2 | ~389–409ns | — |
| AVX2 (XSAVE init opt) | ~389ns | — |

All paths are **< 0.013%** of the tick interval — negligible overhead.

Feature extraction dominates at ~9ns per candidate (~288ns for 32 candidates). The voluntary switch ratio uses log2 subtraction (`log2(a/b) = log2(a) - log2(b)`) to avoid integer division. The scoring and sorting phases add ~100–220ns depending on the path. A single `kernel_fpu_begin/end` pair wraps both SIMD scoring and sorting to amortize the XSAVE/XRSTOR cost.

Stack usage: 512 bytes for 32 candidates (128 bytes with `CONFIG_PREEMPTION`), plus 704 bytes for AVX2 SIMD sort temporaries — well within the typical 16KB kernel stack.

## File Layout

```
kernel/sched/
├── fair.c                    # Static key branch added to detach_tasks()
├── cambyses.c                # Scalar scoring, detach logic, sysctl interface
├── cambyses.h                # Struct definitions, inline helpers, sysctl declarations
├── cambyses_sort.c           # Bitonic sort networks (separate TU — no FPU flags)
├── cambyses_simd_avx2.c      # AVX2 batch scoring + SIMD bitonic sort
├── cambyses_simd_ssse3.c     # SSSE3 batch scoring
├── Makefile                  # Conditional SIMD object files + ISA flags
└── Kconfig                   # CONFIG_SCHED_CAMBYSES, CONFIG_SCHED_CAMBYSES_SIMD
```

## License

GPL-2.0
