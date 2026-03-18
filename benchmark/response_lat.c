// SPDX-License-Identifier: GPL-2.0
/*
 * response_lat — Cambyses scheduling latency benchmark
 *
 * Measures the scheduling latency of "priority" (sleep/wake) tasks
 * competing with CPU-bound background overload under ASYMMETRIC load.
 *
 * Design rationale
 * ----------------
 * Cambyses v0.3 scores migration candidates using four features:
 *   F0: cache coldness   — high after sleep (exec_start is old)
 *   F1: CPU lightness    — high for sleep/wake tasks (low util_avg)
 *   F2: vol switch ratio — high for sleep/wake tasks (many nvcsw)
 *   F3: wakee penalty    — penalises tasks that wake many others
 *
 * All of F0+F1+F2 point in the same direction: prefer migrating
 * lightweight, cache-cold, I/O-bound tasks.  CPU-bound background
 * tasks have low F0 (recently ran), low F1 (high util_avg), and
 * low F2 (never sleep voluntarily).
 *
 * For Cambyses to outperform vanilla, the benchmark must create
 * ASYMMETRIC loading: some CPUs are heavily overloaded (many bg
 * workers + prio workers pinned there), while other CPUs are lightly
 * loaded.  This forces detach_tasks() to pull from overloaded CPUs,
 * and the CHOICE of which task to pull matters:
 *
 *   Cambyses: selects prio worker (high F0+F1+F2, cache cold,
 *             lightweight) → migrates to less-loaded CPU → lower latency
 *   Vanilla:  selects LRU task (may be bg worker) → bg worker moves,
 *             prio stays on overloaded CPU → higher latency
 *
 * Scenario
 * --------
 *   - "hot" CPUs (half of all CPUs): pinned with bg_per_hot bg workers
 *     each + prio workers initially placed there.  These CPUs are
 *     overloaded, triggering load balancing.
 *   - "cold" CPUs (other half): pinned with 1 bg worker each.
 *     These are lightly loaded — the pull destination.
 *   - Prio workers: sleep SLEEP_MS → burst BURST_MS → repeat.
 *     NOT pinned (free to migrate).  After waking on a hot CPU,
 *     the load balancer should pull them to a cold CPU.
 *
 * Metric
 * ------
 *   Scheduling latency = wall time from wake to burst completion.
 *   Ideal = burst_ms.  Extra time = time spent competing on an
 *   overloaded CPU before being migrated to a less-loaded one.
 *   Compare p50/p95/p99 between Cambyses enabled and disabled.
 *
 * Usage: ./response_lat [options]
 *   -d S    measurement duration (default: 20)
 *   -b N    bg workers per hot CPU (default: 4)
 *   -p N    priority task count (default: num_cpus/4, min 2)
 *   -s MS   priority sleep interval ms (default: 50)
 *   -t MS   priority burst length ms (default: 10)
 *   -w N    warmup rounds before measurement (default: 20)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>
#include <getopt.h>
#include <math.h>
#include <sys/resource.h>

#define MAX_SAMPLES 100000

static int num_cpus;
static unsigned long long iters_per_ms;

static void busy_spin(unsigned long long iters)
{
	volatile unsigned long long x = 0;
	for (unsigned long long i = 0; i < iters; i++)
		x += i * i + 1;
	(void)x;
}

static void calibrate(void)
{
	unsigned long long probe = 10000000ULL;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	busy_spin(probe);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
	            (t1.tv_nsec - t0.tv_nsec) / 1e6;
	iters_per_ms = (unsigned long long)(probe / ms);
	fprintf(stderr, "Calibrated: %lluK iters/ms\n", iters_per_ms / 1000);
}

/* Pin calling thread to a specific CPU */
static void pin_to_cpu(int cpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

/* Allow calling thread to run on any CPU */
static void unpin(void)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	for (int i = 0; i < num_cpus; i++)
		CPU_SET(i, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

struct shared {
	atomic_int measure; /* 1 = measurement phase active  */
	atomic_int stop;    /* 1 = all workers should exit   */
};

/* ------------------------------------------------------------------ */
/* Background worker — pure CPU-bound loop, pinned to assigned CPU     */
/* ------------------------------------------------------------------ */

struct bg_arg {
	struct shared *sh;
	int            cpu;  /* CPU to pin to (-1 = no pin) */
};

static void *bg_worker(void *arg)
{
	struct bg_arg *a = arg;
	if (a->cpu >= 0)
		pin_to_cpu(a->cpu);
	while (!atomic_load_explicit(&a->sh->stop, memory_order_relaxed))
		busy_spin(iters_per_ms * 10);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Priority worker — sleep/wake cycle, initially on hot CPU            */
/* ------------------------------------------------------------------ */

struct prio_arg {
	struct shared  *sh;
	int             initial_cpu; /* hot CPU to start on */
	int             sleep_ms;
	int             burst_ms;
	int             warmup_rounds;
	long long      *samples;
	atomic_int      n_samples;
	int             max_samples;
};

static void *prio_worker(void *arg)
{
	struct prio_arg *a = arg;
	struct timespec sleep_ts = {
		.tv_sec  = a->sleep_ms / 1000,
		.tv_nsec = (long)(a->sleep_ms % 1000) * 1000000L
	};
	unsigned long long burst_iters =
		iters_per_ms * (unsigned long long)a->burst_ms;

	/*
	 * Pin to initial hot CPU for warmup.
	 * This builds up util_avg on the hot CPU and establishes
	 * nvcsw history (F2).
	 */
	pin_to_cpu(a->initial_cpu);

	for (int i = 0; i < a->warmup_rounds; i++) {
		nanosleep(&sleep_ts, NULL);
		busy_spin(burst_iters);
	}

	/* Spin until main thread signals measurement start */
	while (!atomic_load_explicit(&a->sh->measure, memory_order_acquire))
		sched_yield();

	/*
	 * Unpin: allow scheduler to migrate freely.
	 * The task will wake on the hot CPU (last_cpu affinity) and
	 * the load balancer decides whether to pull it to a cold CPU.
	 */
	unpin();

	/* Measurement loop */
	while (!atomic_load_explicit(&a->sh->stop, memory_order_relaxed)) {
		/*
		 * Re-pin to hot CPU before sleeping.
		 * This ensures that on wake, the task is on an overloaded
		 * CPU — the load balancer must decide to migrate it.
		 * Without this, after being migrated to a cold CPU, the
		 * task would wake on the cold CPU and never trigger
		 * detach_tasks() again.
		 */
		pin_to_cpu(a->initial_cpu);
		nanosleep(&sleep_ts, NULL);

		/*
		 * Unpin just before measurement.
		 * The task wakes on the hot CPU (pinned during sleep).
		 * Now allow migration — the load balancer can pull it.
		 */
		unpin();

		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		busy_spin(burst_iters);

		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);

		long long lat_us =
			((t1.tv_sec  - t0.tv_sec)  * 1000000000LL +
			 (t1.tv_nsec - t0.tv_nsec)) / 1000LL;

		int idx = atomic_fetch_add_explicit(
				&a->n_samples, 1, memory_order_relaxed);
		if (idx < a->max_samples)
			a->samples[idx] = lat_us;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;
	return (x > y) - (x < y);
}

static long long percentile(long long *s, int n, double p)
{
	if (n == 0) return 0;
	int i = (int)(p / 100.0 * n);
	if (i >= n) i = n - 1;
	return s[i];
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "  -d S    measurement duration in seconds (default: 20)\n"
	    "  -b N    bg workers per hot CPU (default: 4)\n"
	    "  -p N    priority task count (default: num_cpus/4, min 2)\n"
	    "  -s MS   priority sleep interval ms (default: 50)\n"
	    "  -t MS   priority burst length ms (default: 10)\n"
	    "  -w N    warmup sleep/burst rounds (default: 20)\n",
	    prog);
	exit(1);
}

int main(int argc, char **argv)
{
	int duration_s    = 20;
	int bg_per_hot    = 4;
	int prio_count    = -1;
	int sleep_ms      = 50;
	int burst_ms      = 10;
	int warmup_rounds = 20;

	static const struct option long_opts[] = {
		{ "duration",    required_argument, NULL, 'd' },
		{ "bg-per-hot",  required_argument, NULL, 'b' },
		{ "prio",        required_argument, NULL, 'p' },
		{ "sleep-ms",    required_argument, NULL, 's' },
		{ "burst-ms",    required_argument, NULL, 't' },
		{ "warmup",      required_argument, NULL, 'w' },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "d:b:p:s:t:w:", long_opts, NULL)) != -1) {
		switch (c) {
		case 'd': duration_s    = atoi(optarg); break;
		case 'b': bg_per_hot    = atoi(optarg); break;
		case 'p': prio_count    = atoi(optarg); break;
		case 's': sleep_ms      = atoi(optarg); break;
		case 't': burst_ms      = atoi(optarg); break;
		case 'w': warmup_rounds = atoi(optarg); break;
		default:  usage(argv[0]);
		}
	}

	num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpus < 4) {
		fprintf(stderr, "Error: need at least 4 CPUs for asymmetric test\n");
		return 1;
	}

	int n_hot  = num_cpus / 2;
	int n_cold = num_cpus - n_hot;

	if (prio_count < 0)
		prio_count = num_cpus / 4;
	if (prio_count < 2)
		prio_count = 2;

	/* bg workers: bg_per_hot on each hot CPU, 1 on each cold CPU */
	int bg_hot_total  = n_hot * bg_per_hot;
	int bg_cold_total = n_cold * 1;
	int bg_count      = bg_hot_total + bg_cold_total;

	calibrate();

	int cambyses_on = 0;
	{
		FILE *fp = fopen("/proc/sys/kernel/sched_cambyses", "r");
		if (fp) { int r = fscanf(fp, "%d", &cambyses_on); (void)r; fclose(fp); }
	}

	printf("\n=== Response Latency Benchmark (asymmetric) ===\n");
	printf("CPUs:          %d  (hot: %d, cold: %d)\n",
	       num_cpus, n_hot, n_cold);
	printf("BG workers:    %d  (hot: %d×%d=%d, cold: %d×1=%d)\n",
	       bg_count, n_hot, bg_per_hot, bg_hot_total, n_cold, bg_cold_total);
	printf("Prio workers:  %d  (sleep %dms / burst %dms, placed on hot CPUs)\n",
	       prio_count, sleep_ms, burst_ms);
	printf("Warmup rounds: %d\n", warmup_rounds);
	printf("Duration:      %ds\n", duration_s);
	printf("Cambyses:      %s\n\n",
	       cambyses_on ? "enabled" : "disabled (vanilla FIFO)");

	struct shared sh;
	atomic_store(&sh.measure, 0);
	atomic_store(&sh.stop, 0);

	/* ---- Start background workers ---- */
	pthread_t      *bg_thr  = calloc(bg_count, sizeof(pthread_t));
	struct bg_arg  *bg_args = calloc(bg_count, sizeof(struct bg_arg));
	int bi = 0;

	/* Hot CPUs: bg_per_hot workers each, pinned */
	for (int cpu = 0; cpu < n_hot; cpu++) {
		for (int j = 0; j < bg_per_hot; j++) {
			bg_args[bi].sh  = &sh;
			bg_args[bi].cpu = cpu;
			pthread_create(&bg_thr[bi], NULL, bg_worker, &bg_args[bi]);
			bi++;
		}
	}

	/* Cold CPUs: 1 worker each, pinned */
	for (int cpu = n_hot; cpu < num_cpus; cpu++) {
		bg_args[bi].sh  = &sh;
		bg_args[bi].cpu = cpu;
		pthread_create(&bg_thr[bi], NULL, bg_worker, &bg_args[bi]);
		bi++;
	}

	/*
	 * Let background workers settle on their pinned CPUs.
	 * Hot CPUs become saturated, cold CPUs have exactly 1 bg task.
	 */
	{
		struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
		nanosleep(&ts, NULL);
	}

	/* ---- Start priority workers ---- */
	pthread_t       *prio_thr  = calloc(prio_count, sizeof(pthread_t));
	struct prio_arg *prio_args = calloc(prio_count, sizeof(struct prio_arg));
	long long      **sbufs     = calloc(prio_count, sizeof(long long *));

	for (int i = 0; i < prio_count; i++) {
		sbufs[i] = calloc(MAX_SAMPLES, sizeof(long long));
		prio_args[i].sh            = &sh;
		prio_args[i].initial_cpu   = i % n_hot;  /* round-robin on hot CPUs */
		prio_args[i].sleep_ms      = sleep_ms;
		prio_args[i].burst_ms      = burst_ms;
		prio_args[i].warmup_rounds = warmup_rounds;
		prio_args[i].samples       = sbufs[i];
		atomic_store(&prio_args[i].n_samples, 0);
		prio_args[i].max_samples   = MAX_SAMPLES;
		pthread_create(&prio_thr[i], NULL, prio_worker, &prio_args[i]);
	}

	/*
	 * Wait for all priority workers to finish warmup.
	 * Each worker does warmup_rounds × (sleep_ms + burst_ms).
	 */
	{
		long warmup_ms = (long)(warmup_rounds + 2) * (sleep_ms + burst_ms);
		struct timespec ts = {
			.tv_sec  = warmup_ms / 1000,
			.tv_nsec = (warmup_ms % 1000) * 1000000L
		};
		nanosleep(&ts, NULL);
	}

	fprintf(stderr, "Warmup complete — starting measurement.\n");
	atomic_store_explicit(&sh.measure, 1, memory_order_release);

	printf("  sec  bursts/s   total\n");
	printf("  ---  --------   -----\n");

	int prev = 0;
	for (int s = 1; s <= duration_s; s++) {
		struct timespec ts = { 1, 0 };
		nanosleep(&ts, NULL);
		int total = 0;
		for (int i = 0; i < prio_count; i++)
			total += atomic_load(&prio_args[i].n_samples);
		printf("  %3d  %8d   %5d\n", s, total - prev, total);
		fflush(stdout);
		prev = total;
	}

	atomic_store_explicit(&sh.stop, 1, memory_order_release);

	for (int i = 0; i < bg_count;   i++) pthread_join(bg_thr[i],   NULL);
	for (int i = 0; i < prio_count; i++) pthread_join(prio_thr[i], NULL);

	/* ---- Aggregate all samples ---- */
	int total = 0;
	for (int i = 0; i < prio_count; i++) {
		int n = atomic_load(&prio_args[i].n_samples);
		if (n > MAX_SAMPLES) n = MAX_SAMPLES;
		total += n;
	}

	long long *all = calloc(total, sizeof(long long));
	int idx = 0;
	for (int i = 0; i < prio_count; i++) {
		int n = atomic_load(&prio_args[i].n_samples);
		if (n > MAX_SAMPLES) n = MAX_SAMPLES;
		memcpy(&all[idx], sbufs[i], n * sizeof(long long));
		idx += n;
	}
	total = idx;
	qsort(all, total, sizeof(long long), cmp_ll);

	long long ideal = (long long)burst_ms * 1000;

	printf("\n=== Summary ===\n");
	printf("  Total bursts:   %d\n", total);
	printf("  Ideal latency:  %lldus  (%dms burst with full CPU)\n",
	       ideal, burst_ms);
	if (total > 0) {
		long long p50 = percentile(all, total, 50);
		long long p95 = percentile(all, total, 95);
		long long p99 = percentile(all, total, 99);
		long long pmax = all[total - 1];
		printf("  Latency  p50:  %8lldus  (%.2fx ideal)\n",
		       p50, (double)p50 / ideal);
		printf("  Latency  p95:  %8lldus  (%.2fx ideal)\n",
		       p95, (double)p95 / ideal);
		printf("  Latency  p99:  %8lldus  (%.2fx ideal)\n",
		       p99, (double)p99 / ideal);
		printf("  Latency  max:  %8lldus  (%.2fx ideal)\n",
		       pmax, (double)pmax / ideal);

		/* Machine-readable summary for run script */
		printf("RESULT p50=%lld p95=%lld p99=%lld max=%lld ideal=%lld\n",
		       p50, p95, p99, pmax, ideal);
	}

	for (int i = 0; i < prio_count; i++) free(sbufs[i]);
	free(sbufs);
	free(all);
	free(bg_thr);   free(bg_args);
	free(prio_thr); free(prio_args);
	return 0;
}
