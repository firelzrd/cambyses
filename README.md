# Cambyses

**Context-Aware Migration Balancer Yielding Scored Entity Selection**

A Linux kernel patch that replaces the default FIFO task selection in the CFS load balancer with scoring-based optimal candidate selection, improving the quality of task migration decisions.

## Problem

The CFS load balancer's `detach_tasks()` scans the `cfs_tasks` list from the tail and unconditionally picks the first task that passes `can_migrate_task()`. There is no comparison of which task is most suitable for migration. The cache-hot check is binary (migrate or not) and does not consider the *degree* of migration cost.

This means that in a runqueue with a mix of cache-hot, cache-cold, heavy, and light tasks, the load balancer may migrate a cache-hot lightweight task when a cache-cold heavyweight task would have been a far better choice — resolving the imbalance more efficiently with lower migration cost.

## Approach

Cambyses introduces a 2-phase pipeline that collects, scores, and selects migration candidates before detaching:

1. **Phase 1 — Sampling + Scoring**: Collect up to `SCHED_NR_MIGRATE_BREAK` (32) candidates that pass `can_migrate_task()` and `check_imbalance_cambyses()`, computing a migration score for each.
2. **Phase 2 — Argmax Extraction**: Repeatedly extract the highest-scoring candidate, detaching in score order while consuming the imbalance budget.

Both the pull path (`detach_tasks()`) and push path (`detach_one_task()`) are supported. The push path uses a simple max search since only one task is migrated.

All Cambyses paths run under the existing `rq_lock_irqsave` — no additional locking is required.

## Key Techniques

### Argmax extraction

Phase 2 extracts the best candidate via O(N) argmax scan instead of establishing total order over all candidates. Since typically only K=1..4 tasks are detached per balance run, full sorting (O(N log²N)) would waste work on the remaining candidates. Repeated argmax extraction costs O(K×N) — for K=3, N=32 this is ~186 micro-ops vs ~588+ for a sort network.

The scalar argmax compiles to branchless CMP+CMOV (no branch mispredictions), and the 64-byte `s16 scores[]` array stays L1-hot across extractions. Consumed candidates are marked with `S16_MIN` (-32768), which can never be a valid score (real range: -765 to +2295).

### SIMD argmax acceleration

When `CONFIG_SCHED_CAMBYSES_SIMD` is enabled and 8+ candidates are available, Phase 2 uses SIMD instructions to find the highest-scored candidate. The 32-entry `s16 scores[]` array is loaded into SIMD registers and reduced via lane-wise max + broadcast compare + bitmask extraction:

| ISA | Registers | Algorithm | Ops | Cycles | ns @ 5 GHz |
|-----|-----------|-----------|-----|--------|------------|
| Scalar | — | CMP+CMOV loop (N entries) | N×2 | N=10: ~20, N=24: ~48 | ~4–10 |
| AVX2 | 2 ymm (32 scores) | VPMAXSW → scalar hmax → VPCMPEQW → VPMOVMSKB → BSF | ~16 SIMD + ~15 scalar | ~10 | ~2 |
| SSSE3 | 4 xmm (32 scores) | PMAXSW → scalar hmax → PCMPEQW → PMOVMSKB → BSF | ~24 SIMD + ~7 scalar | ~14 | ~3 |
| NEON | 4 Q regs (32 scores) | SMAXP → SMAXV → CMEQ → positional weight bitmask → CTZ | ~20 | ~12 | ~2 |

The FPU context cost is near-zero in the common scheduler path: `cambyses_simd_begin()` checks `TIF_NEED_FPU_LOAD` (x86) / `TIF_FOREIGN_FPSTATE` (ARM64), which is typically already set after a context switch, making the save a no-op. Falls back to scalar argmax when candidate count is below `CAMBYSES_SIMD_THRESHOLD` (8) or CPU lacks support.

### Score Shadow

Two of the four scoring features — F2 (vol_switch_ratio) and F3 (wakee_penalty) — reside on distant cache lines within `task_struct` (`nvcsw` at offset ~248, `wakee_flips` at offset ~272). Fetching them during Phase 1 would add 2 DRAM misses per candidate.

Score Shadow pre-computes F2 and F3 at their natural update points and caches them in `sched_entity`:

```c
/* sched_entity — fits in the existing 4-byte hole at +84 */
struct sched_entity {
    ...
    unsigned char           custom_slice;
#ifdef CONFIG_SCHED_CAMBYSES
    u8                      cambyses_f2;    /* cached vol_switch_ratio */
    u8                      cambyses_f3;    /* cached wakee_penalty */
    u8                      __cambyses_pad[2];
#endif
    u64                     exec_start;     /* same cache line */
    ...
};
```

The cached fields occupy a natural 4-byte padding hole at offset +84, co-located on the same 64-byte cache line as `group_node` (+64) and `exec_start` (+88) — fields already fetched during Phase 1. No additional cache-line access is needed.

| Field | Update point | Trigger |
|-------|-------------|---------|
| `cambyses_f2` | `__schedule()` (after `++*switch_count`) | Every context switch — `nvcsw`/`nivcsw` just incremented |
| `cambyses_f3` | `record_wakee()` (after flip decay and increment) | Wakee relationship change |

| CPU class | Cache lines saved | Cycle reduction | Examples |
|-----------|-------------------|-----------------|----------|
| OOO (wide) | 2 | ~0% (loads already parallel) | Zen 3/4, Golden Cove |
| Limited OOO | 2 | ~20–30% | Silvermont, Gracemont |
| In-order | 2 | ~40–50% | Cortex-A55, Bonnell |

### Prefetch

Phase 1 uses 1-ahead software prefetch: while scoring task N, the cache lines of task N+1 are prefetched. This hides DRAM latency at zero cost on OOO CPUs (prefetch of already-in-flight loads is a NOP) and provides 60–67% cycle reduction on in-order cores.

With Score Shadow, only 3 prefetch targets are needed per candidate:

```c
prefetch(&next->se.exec_start);      /* F0 + F2/F3 (same cache line) */
prefetch(&next->se.avg.load_avg);    /* F1 */
prefetch(&next->cpus_ptr);           /* can_migrate_task() */
```

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

Default weights (`w0=1, w1=2, w2=1, w3=1`) make load contribution the dominant factor — migrating one heavy task is more efficient than multiple light tasks in terms of migration count, lock contention, and IPIs. Cache coldness acts as a secondary selector among tasks of similar load, while voluntary switch ratio and wakee penalty serve as tiebreakers.

Score range fits in `s16`: max +2295, min -765 with maximum weights.

## Configuration

### Build

| Option | Description |
|--------|-------------|
| `CONFIG_SCHED_CAMBYSES` | Core Cambyses functionality. Depends on `SMP`. Default `y`. |
| `CONFIG_SCHED_CAMBYSES_SIMD` | SIMD argmax for Phase 2 candidate extraction. Depends on `X86_64` or `ARM64 && KERNEL_MODE_NEON`. Default `y`. |

### Runtime (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_cambyses` | 1 | Enable/disable via static key. When set to 0, all Cambyses paths are NOPed out at zero cost, falling back to the vanilla FIFO path. |
| `kernel.sched_cambyses_w0` | 1 | Cache coldness weight (0–3) |
| `kernel.sched_cambyses_w1` | 2 | Load contribution weight (0–3) |
| `kernel.sched_cambyses_w2` | 1 | Voluntary switch ratio weight (0–3) |
| `kernel.sched_cambyses_w3` | 1 | Wakee penalty weight (0–3) |

## Performance

### Cost Model

Cambyses's overhead is dominated by **Phase 1 (scanning + scoring)**, which requires reading 3 cache lines per `task_struct`. The per-candidate cost depends heavily on cache residency:

| Cache state | Per-task cost | N=32 total | When |
|-------------|---------------|------------|------|
| L1/L2 warm | ~8ns | ~260ns | Immediately after context switch |
| L3 hit (typical) | ~12–20ns | ~400–650ns | Normal load balancer invocation |
| LLC miss → DRAM (worst case) | ~60–80ns | ~2,000–2,500ns | Cold runqueue, NUMA migration |

In contrast, the scoring phase (scalar `log2p1` + weighted sum) costs ~3–4ns per candidate and the argmax extraction costs ~2–10ns per extraction (SIMD: ~2–3ns fixed, scalar: ~4–10ns depending on N) — both are register/L1-resident operations and negligible compared to memory access.

### Vanilla vs Cambyses

Instruction-level analysis at 5 GHz (0.2 ns/cycle):

#### Typical case (16 tasks scanned, N=10 candidates, K=3 detached)

| Phase | Vanilla | Scalar | AVX2 | SSSE3 | NEON |
|-------|---------|--------|------|-------|------|
| Phase 1: Scan + Score | ~106 cyc | ~548 cyc | ~548 cyc | ~548 cyc | ~548 cyc |
| Phase 2: Selection | — | ~60 cyc (3×20) | ~30 cyc (3×10) | ~42 cyc (3×14) | ~36 cyc (3×12) |
| Detach | ~99 cyc | ~99 cyc | ~99 cyc | ~99 cyc | ~99 cyc |
| **Total** | **~205 cyc / ~41 ns** | **~707 cyc / ~141 ns** | **~685 cyc / ~137 ns** | **~697 cyc / ~139 ns** | **~691 cyc / ~138 ns** |

#### Worst case (32 tasks scanned, N=24 candidates, K=8 detached)

| Phase | Vanilla | Scalar | AVX2 | SSSE3 | NEON |
|-------|---------|--------|------|-------|------|
| Phase 1: Scan + Score | ~206 cyc | ~1168 cyc | ~1168 cyc | ~1168 cyc | ~1168 cyc |
| Phase 2: Selection | — | ~384 cyc (8×48) | ~80 cyc (8×10) | ~112 cyc (8×14) | ~96 cyc (8×12) |
| Detach | ~264 cyc | ~264 cyc | ~264 cyc | ~264 cyc | ~264 cyc |
| **Total** | **~470 cyc / ~94 ns** | **~1816 cyc / ~363 ns** | **~1516 cyc / ~303 ns** | **~1548 cyc / ~310 ns** | **~1532 cyc / ~306 ns** |

### Why the overhead pays for itself

Vanilla uses FIFO — it stops scanning as soon as imbalance is consumed, giving it a ~3x raw-latency advantage. But Cambyses's ~100ns per-invocation overhead is amortized by downstream cost reductions:

1. **Fewer migrations to resolve the same imbalance.** Cambyses preferentially selects heavy-load tasks (F1 weight), so a single well-chosen migration can resolve an imbalance that vanilla would need 2–3 FIFO picks to cover. Each avoided migration saves ~1,000–5,000ns (IPI + rq lock pair + `dequeue_entity`/`enqueue_entity` + context switch on destination CPU). Even one fewer migration per balance run more than repays Cambyses's overhead.

2. **Suppression of re-migration (ping-pong).** Vanilla's blind FIFO selection often picks a task that was recently migrated or has strong wake affinity to its current CPU — leading the balancer to migrate it back on the next run. Cambyses penalizes high-wakee-flip tasks (F3) and favors voluntary-switch-heavy tasks (F2, indicating transient cache footprints), both of which reduce the probability of re-migration. A single avoided re-migration saves ~2,000–10,000ns (two full migration costs) plus the scheduling latency disruption on both CPUs.

3. **Lower post-migration cache miss cost.** Migration invalidates the task's L1/L2 working set on the source CPU. The "cache refill tax" on the destination depends on the task's cache footprint: a cache-hot compute-bound task may suffer 100–500 LLC misses × ~10ns = ~1,000–5,000ns of stall. Cambyses selects cache-cold tasks first (F0 weight) — tasks whose working set is already cold pay near-zero refill cost. The voluntary switch ratio (F2) further selects I/O-bound tasks with small, transient cache footprints that refill cheaply.

4. **Reduced scheduler overhead (positive feedback loop).** The three effects above compound: fewer migrations and fewer re-migrations mean fewer `load_balance()` invocations that find persistent imbalance — the balancer converges faster and enters idle earlier. Each avoided balance run saves the full cost of `rq_lock_irqsave` acquisition on both source and destination CPUs, IPI delivery, and the `detach_tasks()`/`attach_tasks()` path itself. On high-core-count systems where the balancer runs O(N_cpus) times per tick, even a modest reduction in per-run migration count cascades into measurably lower total scheduler CPU time — freeing cycles for actual workloads and improving system throughput.

### Additional observations

- **Phase 1 dominates (75-85%)**: The argmax extraction in Phase 2 is negligible (~30 cycles for K=3 with AVX2). The overwhelming cost is Phase 1's full scan of the runqueue.
- **SIMD scales with K**: Scalar argmax cost is O(K×N) — both extraction count and candidate count increase cost. SIMD argmax is O(K) with fixed ~10–14 cycles per extraction regardless of N. At K=8, AVX2 reduces Phase 2 from ~384 to ~80 cycles (79%), SSSE3 to ~112 (71%), NEON to ~96 (75%).
- **Prefetch closes the gap**: 1-ahead software prefetch brings per-task cost within ~8% of vanilla, despite computing 4-feature scores. On in-order CPUs, prefetch provides 60-67% cycle reduction.

### In-order CPU Impact

On in-order or limited-OOO cores (ARM Cortex-A55, Atom Bonnell, RISC-V), loads cannot be parallelized by the hardware. Software prefetch becomes critical:

| Mode | N=32 without prefetch | N=32 with 1-ahead prefetch | Reduction |
|------|----------------------|---------------------------|-----------|
| In-order (direct) | ~36,800 cyc | ~12,500 cyc | **66%** |
| In-order + cgroup | ~48,000 cyc | ~17,100 cyc | **64%** |

Without prefetch, each of the 3 loads per task serializes into sequential DRAM accesses (~400cy each), yielding ~1,150 cy/task. Prefetch allows the next task's loads to overlap with the current task's scoring computation, reducing effective per-task cost to ~390 cy — close to the OOO baseline.

### Stack Usage

320 bytes: 256 bytes for 32 `cambyses_candidate` structs (8 bytes each: pointer only) + 64 bytes for `s16 scores[32]`. Well within the typical 16KB kernel stack.

## File Layout

```
kernel/sched/
├── fair.c                    # Static key branch added to detach_tasks()
├── cambyses.c                # Scoring, argmax extraction, detach logic, sysctl
├── cambyses.h                # Struct definitions, declarations
├── cambyses_simd_avx2.c      # AVX2 SIMD argmax (x86_64)
├── cambyses_simd_ssse3.c     # SSSE3 SIMD argmax (x86_64)
├── cambyses_simd_neon.c      # NEON SIMD argmax (ARM64)
└── Kconfig                   # CONFIG_SCHED_CAMBYSES, CONFIG_SCHED_CAMBYSES_SIMD
```

## License

GPL-2.0
