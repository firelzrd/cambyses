# Cambyses

**Context-Aware Migration Balancer Yielding Scored Entity Selection**

A Linux kernel patch that replaces the default FIFO task selection in the CFS load balancer with scoring-based optimal candidate selection, improving the quality of task migration decisions.

## Problem

The CFS load balancer's `detach_tasks()` scans the `cfs_tasks` list from the tail and unconditionally picks the first task that passes `can_migrate_task()`. There is no comparison of which task is most suitable for migration. The cache-hot check is binary (migrate or not) and does not consider the *degree* of migration cost.

This means that in a runqueue with a mix of cache-hot, cache-cold, heavy, and light tasks, the load balancer may migrate a cache-hot lightweight task when a cache-cold heavyweight task would have been a far better choice — resolving the imbalance more efficiently with lower migration cost.

Because the tail position in `cfs_tasks` depends on insertion order and vruntime-driven requeuing, which candidate happens to be at the tail at the moment `load_balance()` fires is essentially timing-dependent. The same workload can produce different migration decisions depending on when the balancer runs relative to wakeups and context switches — a stochastic signal that may or may not align with the optimal choice.

## Approach

Cambyses introduces a 2-phase pipeline that collects, scores, and selects migration candidates before detaching:

1. **Phase 1 — Sampling + Scoring**: Collect up to `SCHED_NR_MIGRATE_BREAK` (32) candidates that pass `can_migrate_task()` and `check_imbalance_cambyses()`, computing a migration score for each.
2. **Phase 2 — Argmax Extraction**: Repeatedly extract the highest-scoring candidate, detaching in score order while consuming the imbalance budget.

Both the pull path (`detach_tasks()`) and push path (`detach_one_task()`) are supported. The push path uses a simple max search since only one task is migrated.

All Cambyses paths run under the existing `rq_lock_irqsave` — no additional locking is required.

![Cambyses Algorithm Overview](algorithm_overview.svg)

## Key Techniques

### Argmax extraction

Phase 2 extracts the best candidate via O(N) argmax scan instead of establishing total order over all candidates. Since typically only K=1..4 tasks are detached per balance run, full sorting (O(N log²N)) would waste work on the remaining candidates. Repeated argmax extraction costs O(K×N) — for K=3, N=32 this is ~186 micro-ops vs ~588+ for a sort network.

The scalar argmax uses **4-way parallel reduction**: four independent accumulators break the serial dependency chain, allowing the OOO engine to overlap the comparison streams. A 3-comparison tree merge produces the global best. The 64-byte `s16 scores[]` array stays L1-hot across extractions. Consumed candidates are marked with `S16_MIN` (-32768), which can never be a valid score.

### SIMD argmax acceleration

When `CONFIG_SCHED_CAMBYSES_SIMD` is enabled and 8+ candidates are available, Phase 2 uses SIMD instructions to find the highest-scored candidate. The 32-entry `s16 scores[]` array is loaded into SIMD registers and reduced to the argmax index in a single pass:

| ISA | Registers | Algorithm | Ops | Cycles (measured) | ns @ 5 GHz |
|-----|-----------|-----------|-----|-------------------|------------|
| Scalar (4-way) | GPR only | 4-way parallel reduction → 3-CMP tree merge | N×~1.3 + 3 | N=32: ~27–43, N=8: ~14 | ~5–9 |
| AVX2 / SSE4.1 | 4 xmm (32 scores) | XOR 0x7FFF → 4× PHMINPOSUW → 3 scalar CMP | ~15 | ~7 | ~1.4 |
| SSSE3 (no SSE4.1) | 4 xmm (32 scores) | PMAXSW → scalar hmax → PCMPEQW → PMOVMSKB → BSF | ~24 SIMD + ~7 scalar | ~14 | ~3 |
| NEON | 4 Q regs (32 scores) | VMAXQ → SMAXV → CMEQ → positional weight bitmask → CTZ | ~20 | ~12 | ~2 |

The SIMD path is guarded by `irq_fpu_usable()`: `kernel_fpu_begin()` is unsafe in softirq context (`irq_fpu_usable()` returns false when `softirq_count() != 0`), so the SIMD argmax is only entered from `newidle_balance` (non-softirq). Load balancing via `SCHED_SOFTIRQ` (`run_rebalance_domains`) falls back to the scalar 4-way path. In the `newidle_balance` path, the FPU context cost is near-zero: `TIF_NEED_FPU_LOAD` (x86) / `TIF_FOREIGN_FPSTATE` (ARM64) is typically already set after a context switch, making the save a no-op. Falls back to scalar argmax when candidate count is below `CAMBYSES_SIMD_THRESHOLD` (8) or CPU lacks support.

### Score Shadow

Signals that are expensive to compute fresh at scoring time (require reading distant `task_struct` fields or expensive arithmetic) are cached in `sched_entity` and updated at their natural event points:

```c
struct sched_entity {
    ...
#ifdef CONFIG_SCHED_CAMBYSES
    u8  cambyses_sig2;          /* sig2 io_boundness  (updated at context switch) */
    u8  cambyses_sig3;          /* sig3 wakee_penalty (updated at record_wakee) */
    u8  cambyses_sig5;          /* sig5 nvcsw_ratio   (updated at context switch) */
    u8  __cambyses_pad[1];
    u64 cambyses_last_migrate;  /* timestamp of last migration (updated at set_task_cpu) */
#endif
    ...
};
```

`cambyses_sig2/3/5` occupy the natural 4-byte padding hole at offset +84, co-located on the same 64-byte cache line as `group_node` (+64) and `exec_start` (+88) — already fetched during Phase 1 scoring. `cambyses_last_migrate` lives near `nr_migrations` in `sched_entity` and is prefetched.

| Field | Update point | Trigger |
|-------|-------------|---------|
| `cambyses_sig2`, `cambyses_sig5` | `__schedule()` (context switch) | Every context switch of `prev` |
| `cambyses_sig3` | `record_wakee()` | Wakee relationship change |
| `cambyses_last_migrate` | `set_task_cpu()` | Every migration |

Both `cambyses_update_sig3()` and `cambyses_update_ctxsw()` check a static key at entry and return immediately (NOP-patched) when no active slot uses the corresponding cached signals — eliminating update overhead when those signals are not configured.

The following table shows the **Phase 1 scoring cost reduction from Score Shadow caching alone** — not the overall effectiveness of Cambyses. OOO CPUs see ~0% reduction from caching because their hardware already parallelizes the loads that caching eliminates; Cambyses's core benefit (scoring-based candidate selection) applies equally to all CPU classes.

| CPU class | Cache lines saved | Phase 1 scoring cycle reduction (from caching) | Examples |
|-----------|-------------------|-------------------------------------------------|----------|
| OOO (wide) | 1–2 | ~0% (loads already parallelized by hardware) | Zen 3/4, Golden Cove |
| Limited OOO | 1–2 | ~10–20% | Silvermont, Gracemont |
| In-order | 1–2 | ~20–30% | Cortex-A55, Bonnell |

### Prefetch

Phase 1 uses 1-ahead software prefetch: while scoring task N, the cache lines of task N+1 are prefetched. This hides DRAM latency at zero cost on OOO CPUs (prefetch of already-in-flight loads is a NOP) and reduces **Phase 1 per-candidate scanning cycles** by 60–67% on in-order cores (see [In-order CPU Impact](#in-order-cpu-impact) for measurements).

Four prefetch targets are issued per candidate:

```c
prefetch(&next->se.exec_start);            /* sig0 — same cache line as group_node */
prefetch(&next->se.avg.util_avg);          /* sig1/6/7 — util_avg, runnable_avg, weight */
prefetch(&next->cpus_ptr);                 /* can_migrate_task() */
prefetch(&next->se.cambyses_last_migrate); /* sig4 — near nr_migrations */
/* sig2/3/5: cached in se.cambyses_sig{2,3,5} — same line as exec_start, no extra prefetch */
```

## Scoring

### Signal Index Table

Eight signal functions are available, each assigned an index 0–7. Slots are configured with `(src, weight)` pairs via `sched_cambyses_config`.

| Index | Name | Computation | Caching | Range | Sign |
|-------|------|-------------|---------|-------|------|
| 0 | exec\_start delta | `log2p1(rq_clock − exec_start)` | inline | 40–130 | **+** |
| 1 | runnable starvation | `(runnable_avg − util_avg) >> 4` | inline | 0–64 | **+** |
| 2 | io\_boundness | `vol_ratio × (1 − util_frac) × 64` | cached: `cambyses_sig2` | 0–64 | **+** |
| 3 | wakee\_penalty | `log2p1(wakee_flips + 1)` | cached: `cambyses_sig3` | 0–128 (fp2) | **−** |
| 4 | last\_migrate delta | `log2p1(rq_clock − last_migrate)` | inline | 40–130 | **+** |
| 5 | nvcsw ratio | `nvcsw / (nvcsw + nivcsw) × 64` | cached: `cambyses_sig5` | 0–64 | **−** |
| 6 | util\_avg | `util_avg >> 4` | inline | 0–64 | **+** |
| 7 | weighted\_load | `(util_avg × log2p1(weight)) >> 10` | inline | 0–66 | **+** |

Signals 0/3/4 use `log2p1_u64_u8fp2()` — a fixed-point log2 with integer part + 2-bit mantissa, derived from the [BORE scheduler](https://github.com/firelzrd/bore-scheduler)'s `log2p1_u64_u32fp()`. This provides 4× the resolution of `fls64()` within a single `u8`.

### Signal Semantics and Recommended Sign

The score formula is fully additive: `score = Σ w_i × signal_i`. The sign of each weight determines whether a signal promotes (+) or penalizes (−) migration. The **Sign** column in the table above shows the recommended polarity for the **offensive** migration strategy (actively migrate heavy/starved tasks to idle CPUs). Note that the compiled-in default configuration uses a **defensive** strategy for sig1 (see [Configuration](#configuration) for rationale).

| Signal | Meaning | Use **+** (offensive) | Use **−** (defensive) |
|--------|---------|----------------------|----------------------|
| **sig0** exec\_start delta | How long since this task last ran | Starved tasks should be prioritized for migration to a free CPU | Rarely useful negative |
| **sig1** runnable starvation | `runnable_avg − util_avg` (time spent waiting in runqueue) | Prioritize tasks being squeezed by contention — migrate them to relieve starvation | Avoid migrating tasks with high runqueue contention — they may be CPU-bound and equally starved elsewhere; prefer migrating less-contended tasks instead |
| **sig2** io\_boundness | I/O intensity: `vol_ratio × (1 − util_frac)` | These tasks have low cache footprint and are cheap to migrate | Their wake affinity is strong; penalize when minimizing wake latency is critical |
| **sig3** wakee\_penalty | Wakeup relationship complexity (wakee\_flips) | Actively migrate tasks that frequently wake others (unusual) | Tasks with complex wake relationships will be pulled back by wake affinity; penalize them |
| **sig4** last\_migrate delta | How long since last migration | Prefer stable tasks that haven't moved recently; prevents migration thrash | Rarely useful negative |
| **sig5** nvcsw ratio | Fraction of voluntary context switches (sleep/I/O rate) | Migrate I/O-bound tasks to free up CPU-bound slots (reduces CPU contention) | Tasks with high sleep rate have strong wake affinity to their CPU; penalize to avoid displacing them |
| **sig6** util\_avg | Raw CPU utilization (0–1024, scaled to 0–64) | Prefer heavier tasks, similar to sig7 but without priority weighting | Prefer lighter tasks (defensive strategy) |
| **sig7** weighted\_load | CPU load contribution (util × priority) | Heavy tasks consume imbalance budget per migration, reducing total migration count | Rarely useful negative |

### Score formula

```
score = Σ sysctl_cambyses_config[2i+1] × signal(sysctl_cambyses_config[2i], p, env)
        for i in 0..3
```

Score fits in `s16`. A slot with weight 0 is NOP-patched entirely (zero runtime cost). Maximum safe weight range is **−7..+7** (4 × 7 × 255 = 7140 < 32767).

### Signal selection via static binary decision tree

Each slot's signal source is encoded as 3 bits (bit2, bit1, bit0) mapped to 3 `DEFINE_STATIC_KEY_FALSE` keys per slot (12 keys total). At scoring time, the `CAMBYSES_SLOT_SIGNAL(n, p, env)` macro evaluates a binary tree of `static_branch_unlikely()` calls to select one of 8 signal functions — all branches are NOP-patched in steady state, so the selected signal compiles to a single inline function call with zero branch overhead.

Signal sources are reconfigured at sysctl write time via `cambyses_update_src_keys()`, which toggles the static keys and triggers the kernel's text patcher (`stop_machine`).

### Cached signal gate keys

Two additional static keys control whether cached-signal update functions are called:

| Key | Guards | Condition |
|-----|--------|-----------|
| `cambyses_sig3_active` | `cambyses_update_sig3()` in `record_wakee()` | Any active slot (weight ≠ 0) uses sig3 |
| `cambyses_ctxsw_active` | `cambyses_update_ctxsw()` in `__schedule()` | Any active slot uses sig2 or sig5 |

When no slot uses the corresponding signal, the update function returns immediately via NOP-patched branch — eliminating per-context-switch and per-wakeup overhead for unused cached signals.

## Configuration

### Build

| Option | Description |
|--------|-------------|
| `CONFIG_SCHED_CAMBYSES` | Core Cambyses functionality. Depends on `SMP`. Default `y`. |
| `CONFIG_SCHED_CAMBYSES_SIMD` | SIMD argmax for Phase 2 candidate extraction. Depends on `X86_64` or `ARM64 && KERNEL_MODE_NEON`. Default `y`. |

### Runtime (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_cambyses` | `1` | Enable/disable via static key. When `0`, all Cambyses paths are NOP-patched out, falling back to the vanilla FIFO path. |
| `kernel.sched_cambyses_config` | `0 2 1 -5 2 0 3 0` | 4 slots of `(src, weight)` pairs. Format: `"src0 w0 src1 w1 src2 w2 src3 w3"`. |
| `kernel.sched_cambyses_config_default` | (same as above) | Read-only. Compiled-in default configuration. |

#### sched\_cambyses\_config format

```
src:    0–7   (signal index, see Signal Index Table above)
weight: −7..+7 (signed; 0 = disable slot entirely, NOP-patched at zero runtime cost)
```

Default `"0 2 1 -5 2 0 3 0"`:

| Slot | src | weight | Signal | Effect |
|------|-----|--------|--------|--------|
| 0 | 0 | +2 | exec\_start delta | Strongly prefer starved tasks |
| 1 | 1 | −5 | runnable starvation | Strongly prefer tasks with **low** runqueue starvation |
| 2 | 2 | 0 | io\_boundness | Disabled |
| 3 | 3 | 0 | wakee\_penalty | Disabled |

**Why sig1 uses negative weight in the default**: The Signal Index Table lists **+** as the recommended sign for the offensive strategy, but the default configuration intentionally uses −5 (defensive strategy). A high `runnable_avg − util_avg` often indicates a CPU-bound task competing on a saturated runqueue — migrating it merely moves the contention. By penalizing high-starvation tasks, the default prefers migrating tasks that are *not* deeply contended, which tend to be lighter and cheaper to move, while sig0 (exec\_start delta, +2) still ensures that genuinely starved tasks get priority based on absolute wait time.

#### Example: disable runnable starvation

```sh
# Disable slot 1 (set weight to 0):
echo "0 2 1 0 2 0 3 0" > /proc/sys/kernel/sched_cambyses_config
# (default has slot 1 weight = -5; setting to 0 disables it)
```

Setting weight to 0 NOP-patches the entire slot. Setting it back to non-zero re-enables it at the next `stop_machine` boundary.

#### Example: use last\_migrate delta instead of io\_boundness

```sh
# Replace slot 2 with sig4 (last_migrate delta), weight +1:
echo "0 2 1 -5 4 1 3 0" > /proc/sys/kernel/sched_cambyses_config
```

#### Example: add weighted\_load

```sh
# Replace slot 2 with sig7 (weighted_load), weight +1:
echo "0 2 1 -5 7 1 3 0" > /proc/sys/kernel/sched_cambyses_config
```

## Performance

### Cost Model

Cambyses's overhead is dominated by **Phase 1 (scanning + scoring)**, which requires reading 3 cache lines per `task_struct`. The per-candidate cost depends heavily on cache residency. Note that `load_balance()` is invoked at most once per tick (typically every 1–4ms), so even the worst-case overhead of ~2.5μs represents <0.25% of the inter-invocation interval:

| Cache state | Per-task cost | N=32 total | When |
|-------------|---------------|------------|------|
| L1/L2 warm | ~8ns | ~260ns | Immediately after context switch |
| L3 hit (typical) | ~12–20ns | ~400–650ns | Normal load balancer invocation |
| LLC miss → DRAM (worst case) | ~60–80ns | ~2,000–2,500ns | Cold runqueue, NUMA migration |

In contrast, the scoring phase (~3–4ns per candidate) and the argmax extraction (~1.4–9ns per extraction depending on ISA path) are register/L1-resident operations and negligible compared to memory access.

### Vanilla vs Cambyses

Cambyses scans all candidates before detaching (unlike vanilla's early-exit FIFO), so it has higher per-invocation latency. However, this cost is a one-time ~100–300ns overhead per `load_balance()` call (invoked every 1–4ms), and is recovered by improved migration quality — see [Why the overhead pays for itself](#why-the-overhead-pays-for-itself) below.

Instruction-level analysis at 5 GHz (0.2 ns/cycle):

#### Typical case (16 tasks scanned, N=10 candidates, K=3 detached)

| Phase | Vanilla | Scalar (4-way) | AVX2 | SSSE3 | NEON |
|-------|---------|----------------|------|-------|------|
| Phase 1: Scan + Score | ~106 cyc | ~548 cyc | ~548 cyc | ~548 cyc | ~548 cyc |
| Phase 2: Selection | — | ~42 cyc (3×14) | ~21 cyc (3×7) | ~42 cyc (3×14) | ~36 cyc (3×12) |
| Detach | ~99 cyc | ~99 cyc | ~99 cyc | ~99 cyc | ~99 cyc |
| **Total** | **~205 cyc / ~41 ns** | **~689 cyc / ~138 ns** | **~668 cyc / ~134 ns** | **~689 cyc / ~138 ns** | **~683 cyc / ~137 ns** |

#### Worst case (32 tasks scanned, N=24 candidates, K=8 detached)

| Phase | Vanilla | Scalar (4-way) | AVX2 | SSSE3 | NEON |
|-------|---------|----------------|------|-------|------|
| Phase 1: Scan + Score | ~206 cyc | ~1168 cyc | ~1168 cyc | ~1168 cyc | ~1168 cyc |
| Phase 2: Selection | — | ~272 cyc (8×34) | ~56 cyc (8×7) | ~112 cyc (8×14) | ~96 cyc (8×12) |
| Detach | ~264 cyc | ~264 cyc | ~264 cyc | ~264 cyc | ~264 cyc |
| **Total** | **~470 cyc / ~94 ns** | **~1704 cyc / ~341 ns** | **~1488 cyc / ~298 ns** | **~1544 cyc / ~309 ns** | **~1528 cyc / ~306 ns** |

### Why the overhead pays for itself

Vanilla uses FIFO — it stops scanning as soon as imbalance is consumed, giving it a ~3x raw-latency advantage. But Cambyses's ~100ns per-invocation overhead is amortized by downstream improvements:

1. **Better budget utilization per migration.** By preferring I/O-bound and starved tasks (sig1+, sig2+), each migration resolves contention more efficiently — the migrated task is cheap to move (low cache footprint) and the remaining tasks get more CPU. Fewer total migrations means fewer IPI round-trips, fewer rq lock acquisitions, and fewer cache invalidations.

2. **Suppression of re-migration (ping-pong).** Vanilla's blind FIFO selection may pick a task with strong wake affinity to its current CPU, causing the balancer to move it back on the next run. Cambyses penalizes high-wakee-flip tasks (sig3−), reducing ping-pong. A single avoided re-migration saves ~2,000–10,000ns (two full migration costs) plus scheduling latency disruption on both CPUs.

3. **Improved responsiveness for migrated tasks.** Selecting tasks that have waited longest (sig0+) and placing them on less-contended CPUs yields lower p50 scheduling latency — measured at −9% in asymmetric load scenarios.

4. **Reduced scheduler overhead (positive feedback loop).** Fewer migrations and fewer re-migrations mean fewer `load_balance()` invocations — the balancer converges faster and enters idle earlier. On high-core-count systems where the balancer runs O(N_cpus) times per tick, even a modest reduction in per-run migration count cascades into measurably lower total scheduler CPU time.

### Additional observations

- **Phase 1 dominates (65–85%)**: The argmax extraction in Phase 2 is negligible (~21 cycles for K=3 with AVX2). The overwhelming cost is Phase 1's full scan of the runqueue.
- **4-way scalar is competitive**: The 4-way parallel reduction (~27–43 cycles for N=32) closes much of the gap to SIMD (~7 cycles). At K=3, the difference is only ~21 cycles total — negligible compared to Phase 1.
- **SIMD scales with K**: Scalar argmax cost is O(K×N). SIMD argmax is O(K) with fixed ~7–14 cycles per extraction regardless of N. At K=8, AVX2 reduces Phase 2 from ~272 to ~56 cycles (79%).
- **SIMD limited to newidle path (scalar fallback is sufficient)**: `kernel_fpu_begin()` is unsafe in softirq context, so SIMD argmax only activates from `newidle_balance`. The softirq path (`run_rebalance_domains`) uses the scalar 4-way fallback — which is only ~21 cycles slower than AVX2 at K=3 (see above), so the absence of SIMD on this path has negligible impact.
- **Prefetch closes the gap**: 1-ahead software prefetch brings per-task Phase 1 scanning cost within ~8% of vanilla, despite computing multi-signal scores. On in-order CPUs, prefetch reduces Phase 1 per-candidate cycles by 60–67%.

### In-order CPU Impact

On in-order or limited-OOO cores (ARM Cortex-A55, Atom Bonnell, RISC-V), loads cannot be parallelized by the hardware. Software prefetch becomes critical for reducing **Phase 1 scanning cost** (the dominant component of Cambyses's per-invocation overhead):

| Mode | N=32 without prefetch | N=32 with 1-ahead prefetch | Reduction |
|------|----------------------|---------------------------|-----------|
| In-order (direct) | ~36,800 cyc | ~12,500 cyc | **66%** |
| In-order + cgroup | ~48,000 cyc | ~17,100 cyc | **64%** |

### Stack Usage

320–448 bytes: 256 bytes for 32 `cambyses_candidate` structs (8 bytes each: pointer only) + 64 bytes for `s16 scores[32]` + 128 bytes for `int selected[32]` (SIMD path only). Well within the typical 16KB kernel stack.

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
