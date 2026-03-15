// SPDX-License-Identifier: GPL-2.0
/*
 * asymmetric_drain — Cambyses pull-migration benchmark
 *
 * Measures the quality of CFS load balancer pull decisions by repeatedly
 * creating and resolving inter-core imbalance.
 *
 * Design:
 *   - All tasks are CPU-bound (no sleep) → zero wake-up migrations.
 *   - Every migration counted is a pull migration from load_balance(),
 *     which is exactly the path Cambyses modifies.
 *   - Heavy workers (nice 0, high load weight) and light workers (nice +10,
 *     low load weight) create score differentiation for Cambyses.
 *   - Each round: pin tasks to overloaded cores → settle → open affinity →
 *     measure migrations during burst → re-pin for next round.
 *   - Reports per-round stats and summary with mean/stddev.
 *
 * Usage:
 *   ./asymmetric_drain [options]
 *
 * Options:
 *   -r, --rounds N       Number of measurement rounds (default: 20)
 *   -b, --burst-ms MS    Burst duration per round in ms (default: 200)
 *   -S, --settle-ms MS   Settle time after re-pinning in ms (default: 500)
 *   -H, --heavy N        Heavy workers (nice 0) per overloaded core (default: 8)
 *   -l, --light N        Light workers (nice +10) per overloaded core (default: 8)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <getopt.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define MAX_CPUS   1024
#define MAX_CORES  512
#define MAX_TASKS  512
#define MAX_ROUNDS 200

#define PHASE_PIN   0
#define PHASE_BURST 1
#define PHASE_STOP  2

struct shared_state {
	atomic_int phase;
	atomic_int round;
	atomic_int ready;
};

static struct shared_state *shared;

struct core_info {
	int core_id;
	int cpus[16];
	int nr_cpus;
};

static struct core_info cores[MAX_CORES];
static int nr_cores;

static cpu_set_t full_set;
static cpu_set_t pin_sets[MAX_CORES];

struct round_result {
	unsigned long mig_total;
	unsigned long mig_heavy;
	unsigned long mig_light;
	double elapsed;
};

/*
 * Discover physical core topology from sysfs.
 * Groups logical CPUs by their core_id within the same package.
 */
static int discover_topology(void)
{
	int nr_online = (int)sysconf(_SC_NPROCESSORS_ONLN);
	int max_cpu = (nr_online < MAX_CPUS) ? nr_online : MAX_CPUS;

	nr_cores = 0;

	for (int cpu = 0; cpu < max_cpu; cpu++) {
		char path[256];
		FILE *fp;
		int core_id, pkg_id;
		int found = 0;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fscanf(fp, "%d", &core_id) != 1) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
			 cpu);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fscanf(fp, "%d", &pkg_id) != 1) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		int unique_id = pkg_id * 10000 + core_id;

		for (int i = 0; i < nr_cores; i++) {
			if (cores[i].core_id == unique_id) {
				if (cores[i].nr_cpus < 16)
					cores[i].cpus[cores[i].nr_cpus++] = cpu;
				found = 1;
				break;
			}
		}
		if (!found && nr_cores < MAX_CORES) {
			cores[nr_cores].core_id = unique_id;
			cores[nr_cores].cpus[0] = cpu;
			cores[nr_cores].nr_cpus = 1;
			nr_cores++;
		}
	}

	return nr_cores;
}

/*
 * CPU-bound worker.
 *
 * Watches shared phase/round and adjusts affinity:
 *   PHASE_PIN  + new round → re-pin to assigned core, ack ready
 *   PHASE_BURST            → open affinity to all CPUs
 *   PHASE_STOP             → exit
 *
 * Never sleeps → all migrations are pull migrations from load_balance().
 */
static void worker(int core_idx)
{
	int my_last_round = -1;
	int affinity_open = 0;

	for (;;) {
		int phase = atomic_load_explicit(&shared->phase,
						 memory_order_acquire);

		if (phase == PHASE_STOP)
			break;

		if (phase == PHASE_PIN) {
			int round = atomic_load_explicit(&shared->round,
							 memory_order_acquire);
			if (round != my_last_round) {
				sched_setaffinity(0, sizeof(cpu_set_t),
						  &pin_sets[core_idx]);
				affinity_open = 0;
				my_last_round = round;
				atomic_fetch_add_explicit(&shared->ready, 1,
							  memory_order_release);
			}
		}

		if (phase == PHASE_BURST && !affinity_open) {
			sched_setaffinity(0, sizeof(cpu_set_t), &full_set);
			affinity_open = 1;
		}

		/* CPU-bound work — burn cycles */
		for (volatile int i = 0; i < 1000; i++)
			;
	}
}

/*
 * Read se.nr_migrations from /proc/PID/sched.
 */
static unsigned long read_task_migrations(pid_t pid)
{
	char path[64];
	FILE *fp;
	char line[256];
	unsigned long val = 0;

	snprintf(path, sizeof(path), "/proc/%d/sched", pid);
	fp = fopen(path, "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "se.nr_migrations %*c %lu", &val) == 1)
			break;
	}

	fclose(fp);
	return val;
}

static unsigned long read_migrations_batch(pid_t *pids, int n)
{
	unsigned long total = 0;

	for (int i = 0; i < n; i++)
		if (pids[i] > 0)
			total += read_task_migrations(pids[i]);
	return total;
}

static int read_cambyses_enabled(void)
{
	FILE *fp;
	int val = -1;

	fp = fopen("/proc/sys/kernel/sched_cambyses_enabled", "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%d", &val) != 1)
		val = -1;
	fclose(fp);
	return val;
}

static double calc_mean(struct round_result *r, int n,
			unsigned long (*field)(struct round_result *))
{
	double sum = 0;

	for (int i = 0; i < n; i++)
		sum += field(&r[i]);
	return sum / n;
}

static double calc_stddev(struct round_result *r, int n,
			  unsigned long (*field)(struct round_result *),
			  double mean)
{
	double sum = 0;

	for (int i = 0; i < n; i++) {
		double d = (double)field(&r[i]) - mean;
		sum += d * d;
	}
	return sqrt(sum / n);
}

static unsigned long field_total(struct round_result *r) { return r->mig_total; }
static unsigned long field_heavy(struct round_result *r) { return r->mig_heavy; }
static unsigned long field_light(struct round_result *r) { return r->mig_light; }

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -r, --rounds N       Measurement rounds (default: 20)\n"
		"  -b, --burst-ms MS    Burst duration per round (default: 200)\n"
		"  -S, --settle-ms MS   Settle time after re-pinning (default: 500)\n"
		"  -H, --heavy N        Heavy workers per overloaded core (default: 8)\n"
		"  -l, --light N        Light workers per overloaded core (default: 8)\n"
		"  -h, --help           Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	int nr_rounds = 20;
	int burst_ms = 200;
	int settle_ms = 500;
	int nr_heavy = 8;
	int nr_light = 8;
	int nr_overloaded, nr_tasks;
	int cambyses;

	static const struct option longopts[] = {
		{ "rounds",    required_argument, NULL, 'r' },
		{ "burst-ms",  required_argument, NULL, 'b' },
		{ "settle-ms", required_argument, NULL, 'S' },
		{ "heavy",     required_argument, NULL, 'H' },
		{ "light",     required_argument, NULL, 'l' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;

	while ((opt = getopt_long(argc, argv, "r:b:S:H:l:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'r': nr_rounds  = atoi(optarg); break;
		case 'b': burst_ms   = atoi(optarg); break;
		case 'S': settle_ms  = atoi(optarg); break;
		case 'H': nr_heavy   = atoi(optarg); break;
		case 'l': nr_light   = atoi(optarg); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (nr_rounds > MAX_ROUNDS)
		nr_rounds = MAX_ROUNDS;

	if (discover_topology() < 4) {
		fprintf(stderr, "Error: need at least 4 physical cores (found %d)\n",
			nr_cores);
		return 1;
	}

	nr_overloaded = nr_cores / 2;
	nr_tasks = nr_overloaded * (nr_heavy + nr_light);

	if (nr_tasks > MAX_TASKS) {
		fprintf(stderr, "Error: too many tasks (%d > %d)\n",
			nr_tasks, MAX_TASKS);
		return 1;
	}

	/* Build CPU sets */
	CPU_ZERO(&full_set);
	for (int i = 0; i < nr_cores; i++) {
		for (int j = 0; j < cores[i].nr_cpus; j++)
			CPU_SET(cores[i].cpus[j], &full_set);
	}

	for (int c = 0; c < nr_overloaded; c++) {
		CPU_ZERO(&pin_sets[c]);
		for (int j = 0; j < cores[c].nr_cpus; j++)
			CPU_SET(cores[c].cpus[j], &pin_sets[c]);
	}

	/* Print configuration */
	cambyses = read_cambyses_enabled();

	printf("=== Asymmetric Drain Benchmark ===\n");
	printf("Physical cores: %d (%d overloaded, %d idle)\n",
	       nr_cores, nr_overloaded, nr_cores - nr_overloaded);
	printf("Topology:\n");
	for (int i = 0; i < nr_cores; i++) {
		printf("  core %d: CPUs", i);
		for (int j = 0; j < cores[i].nr_cpus; j++)
			printf(" %d", cores[i].cpus[j]);
		printf("%s\n", i < nr_overloaded ? " [OVERLOADED]" : " [idle]");
	}
	printf("Tasks per overloaded core: %d heavy (nice 0) + %d light (nice +10)\n",
	       nr_heavy, nr_light);
	printf("Total tasks: %d (all CPU-bound, no wake-up migrations)\n", nr_tasks);
	printf("Rounds: %d × %dms burst + %dms settle\n",
	       nr_rounds, burst_ms, settle_ms);
	if (cambyses >= 0)
		printf("Cambyses: %s\n", cambyses ? "enabled" : "disabled (vanilla FIFO)");
	else
		printf("Cambyses: not available\n");
	printf("\n");

	/* Allocate shared state */
	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	atomic_init(&shared->phase, PHASE_PIN);
	atomic_init(&shared->round, 0);
	atomic_init(&shared->ready, 0);

	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);

	/* Fork workers */
	pid_t heavy_pids[MAX_TASKS];
	pid_t light_pids[MAX_TASKS];
	int nr_heavy_total = 0, nr_light_total = 0;

	for (int c = 0; c < nr_overloaded; c++) {
		for (int i = 0; i < nr_heavy; i++) {
			pid_t pid = fork();

			if (pid < 0) {
				perror("fork");
				goto stop;
			}
			if (pid == 0) {
				worker(c);
				_exit(0);
			}
			heavy_pids[nr_heavy_total++] = pid;
		}

		for (int i = 0; i < nr_light; i++) {
			pid_t pid = fork();

			if (pid < 0) {
				perror("fork");
				goto stop;
			}
			if (pid == 0) {
				setpriority(PRIO_PROCESS, 0, 10);
				worker(c);
				_exit(0);
			}
			light_pids[nr_light_total++] = pid;
		}
	}

	/* Wait for initial pin */
	while (atomic_load_explicit(&shared->ready, memory_order_acquire) < nr_tasks)
		usleep(1000);

	printf("  Round  Migrations  Heavy  Light  Time(ms)\n");
	printf("  -----  ----------  -----  -----  --------\n");

	/* Run rounds */
	struct round_result results[MAX_ROUNDS];

	for (int r = 0; r < nr_rounds; r++) {
		unsigned long mig_h_before, mig_l_before;
		unsigned long mig_h_after, mig_l_after;
		struct timespec t_start, t_end;

		/*
		 * PIN phase: re-pin for rounds > 0.
		 * Round 0 is already pinned from the initial fork.
		 */
		if (r > 0) {
			atomic_store_explicit(&shared->ready, 0,
					      memory_order_relaxed);
			atomic_store_explicit(&shared->round, r,
					      memory_order_relaxed);
			atomic_store_explicit(&shared->phase, PHASE_PIN,
					      memory_order_release);

			while (atomic_load_explicit(&shared->ready,
						    memory_order_acquire) < nr_tasks)
				usleep(1000);
		}

		/* Settle: let tasks warm caches on pinned cores */
		usleep(settle_ms * 1000);

		/* Snapshot migration counts before burst */
		mig_h_before = read_migrations_batch(heavy_pids, nr_heavy_total);
		mig_l_before = read_migrations_batch(light_pids, nr_light_total);

		/* BURST phase: open affinity, let load balancer pull */
		clock_gettime(CLOCK_MONOTONIC, &t_start);
		atomic_store_explicit(&shared->phase, PHASE_BURST,
				      memory_order_release);

		usleep(burst_ms * 1000);

		/* Snapshot migration counts after burst */
		mig_h_after = read_migrations_batch(heavy_pids, nr_heavy_total);
		mig_l_after = read_migrations_batch(light_pids, nr_light_total);
		clock_gettime(CLOCK_MONOTONIC, &t_end);

		results[r].mig_heavy = mig_h_after - mig_h_before;
		results[r].mig_light = mig_l_after - mig_l_before;
		results[r].mig_total = results[r].mig_heavy + results[r].mig_light;
		results[r].elapsed = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
				     (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

		printf("  %5d  %10lu  %5lu  %5lu  %8.1f\n",
		       r + 1,
		       results[r].mig_total,
		       results[r].mig_heavy,
		       results[r].mig_light,
		       results[r].elapsed);
	}

stop:
	/* Stop all workers */
	atomic_store_explicit(&shared->phase, PHASE_STOP, memory_order_release);
	usleep(100000);

	for (int i = 0; i < nr_heavy_total; i++) {
		kill(heavy_pids[i], SIGKILL);
		waitpid(heavy_pids[i], NULL, 0);
	}
	for (int i = 0; i < nr_light_total; i++) {
		kill(light_pids[i], SIGKILL);
		waitpid(light_pids[i], NULL, 0);
	}

	/* Summary statistics */
	if (nr_rounds > 0) {
		double mean_t = calc_mean(results, nr_rounds, field_total);
		double mean_h = calc_mean(results, nr_rounds, field_heavy);
		double mean_l = calc_mean(results, nr_rounds, field_light);
		double std_t  = calc_stddev(results, nr_rounds, field_total, mean_t);
		double std_h  = calc_stddev(results, nr_rounds, field_heavy, mean_h);
		double std_l  = calc_stddev(results, nr_rounds, field_light, mean_l);

		printf("\n=== Summary (%d rounds) ===\n", nr_rounds);
		printf("  Total migrations:  mean=%6.1f  std=%5.1f\n", mean_t, std_t);
		printf("  Heavy migrations:  mean=%6.1f  std=%5.1f\n", mean_h, std_h);
		printf("  Light migrations:  mean=%6.1f  std=%5.1f\n", mean_l, std_l);
		if (mean_t > 0)
			printf("  Heavy ratio:       %5.1f%%\n",
			       mean_h / mean_t * 100.0);
	}

	munmap(shared, sizeof(*shared));
	return 0;
}
