// SPDX-License-Identifier: GPL-2.0
/*
 * selection_bench — Cambyses migration selection quality benchmark
 *
 * Tests WHICH task the load balancer selects for migration by reading
 * ftrace output directly.  No userspace detection timing tricks.
 *
 * Setup:
 *   - hot_cpu: n_hot workers with mixed weights (heavy + light)
 *   - Other CPUs: fillers to create load imbalance
 *   - Workers have affinity to ALL CPUs (not just hot_cpu + drain)
 *     so the load balancer can freely migrate them.
 *
 * Measurement:
 *   1. Workers spin on hot_cpu for T seconds.
 *   2. ftrace captures detach_one/detach_tasks events.
 *   3. Parse trace to count heavy vs light first-selections per event.
 *
 * Requires: sched_cambyses_debug=1, trace buffer enabled.
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
#include <stdint.h>
#include <limits.h>
#include <sys/resource.h>

#define MAX_HOT_WORKERS 64
#define MAX_FILLERS     2048
#define TRACE_PATH      "/sys/kernel/tracing"

static int num_cpus;

static void sleep_ms(int ms)
{
	struct timespec ts = {
		.tv_sec  = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L
	};
	nanosleep(&ts, NULL);
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void pin_thread(pthread_t thr, int cpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	pthread_setaffinity_np(thr, sizeof(cpuset), &cpuset);
}

static void set_nice(int nice_val)
{
	if (setpriority(PRIO_PROCESS, 0, nice_val) < 0)
		perror("setpriority");
}

/* Write a string to a file (for controlling ftrace) */
static int write_file(const char *path, const char *val)
{
	FILE *fp = fopen(path, "w");
	if (!fp) return -1;
	fputs(val, fp);
	fclose(fp);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Hot workers (pinned to hot_cpu initially, affinitied to all CPUs)   */
/* ------------------------------------------------------------------ */

struct hot_worker {
	int             id;
	int             is_heavy;
	int             nice_val;
	pid_t           tid;
	atomic_int      stop;
};

static void *hot_worker_fn(void *arg)
{
	struct hot_worker *w = arg;
	/* Ensure we're CFS (main thread is SCHED_FIFO) */
	struct sched_param sp_other = { .sched_priority = 0 };
	sched_setscheduler(0, SCHED_OTHER, &sp_other);
	set_nice(w->nice_val);
	w->tid = gettid();

	while (!atomic_load_explicit(&w->stop, memory_order_relaxed)) {
		volatile unsigned long x = 0;
		for (int i = 0; i < 500; i++)
			for (int j = 0; j < 500; j++)
				x += (unsigned long)j * j + 1;
		(void)x;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Filler threads                                                      */
/* ------------------------------------------------------------------ */

struct filler {
	int         cpu;
	int         nice_val;
	atomic_int  stop;
};

static void *filler_fn(void *arg)
{
	struct filler *f = arg;
	struct sched_param sp_other = { .sched_priority = 0 };
	sched_setscheduler(0, SCHED_OTHER, &sp_other);
	pin_to_cpu(f->cpu);
	set_nice(f->nice_val);

	while (!atomic_load_explicit(&f->stop, memory_order_relaxed)) {
		volatile unsigned long x = 0;
		for (int i = 0; i < 100000; i++)
			x += (unsigned long)i * i + 1;
		(void)x;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Trace parsing                                                       */
/* ------------------------------------------------------------------ */

/*
 * Parse ftrace output for detach events involving our worker PIDs.
 *
 * Vanilla trace lines look like:
 *   "detach_one: PICK pid=NNN ... nice=N ..."
 *   "vanilla: DETACHED pid=NNN nice=N ..."
 *
 * Cambyses trace lines:
 *   "cambyses: scalar selected=N pid=NNN ... nice=N ..."
 *   "cambyses: SIMD selected[0]=N pid=NNN ... nice=N ..."
 *
 * For each balance event (group of lines with same timestamp),
 * we count the FIRST detached/selected task as the "selection".
 */
struct trace_result {
	int heavy_first;    /* events where first selected was heavy */
	int light_first;    /* events where first selected was light */
	int heavy_total;    /* total heavy tasks detached */
	int light_total;    /* total light tasks detached */
	int events;         /* total balance events */
};

static void parse_trace(struct hot_worker *workers, int n_hot,
			struct trace_result *res)
{
	FILE *fp;
	char line[1024];
	int pid_is_heavy[65536] = {0};  /* pid→heavy lookup */
	int pid_is_ours[65536] = {0};

	memset(res, 0, sizeof(*res));

	/* Build pid lookup */
	for (int i = 0; i < n_hot; i++) {
		pid_t tid = workers[i].tid;
		if (tid > 0 && tid < 65536) {
			pid_is_ours[tid] = 1;
			pid_is_heavy[tid] = workers[i].is_heavy;
		}
	}

	fp = fopen(TRACE_PATH "/trace", "r");
	if (!fp) {
		perror("open trace");
		return;
	}

	/*
	 * Track events by timestamp groups.
	 * A "balance event" is a cluster of lines within 100μs.
	 */
	double prev_ts = 0;
	int first_in_event = 1;
	int first_was_heavy = 0;

	while (fgets(line, sizeof(line), fp)) {
		int pid = 0;
		int nice = 0;
		int is_pick = 0;

		/* Match: "detach_one: PICK pid=NNN" */
		char *p = strstr(line, "detach_one: PICK pid=");
		if (p) {
			sscanf(p, "detach_one: PICK pid=%d", &pid);
			is_pick = 1;
		}

		/* Match: "vanilla: DETACHED pid=NNN nice=N" */
		if (!is_pick) {
			p = strstr(line, "vanilla: DETACHED pid=");
			if (p) {
				sscanf(p, "vanilla: DETACHED pid=%d nice=%d",
				       &pid, &nice);
				is_pick = 1;
			}
		}

		/* Match: "cambyses: scalar selected=" or "SIMD selected[0]=" */
		if (!is_pick) {
			p = strstr(line, "scalar selected=");
			if (!p)
				p = strstr(line, "SIMD selected[0]=");
			if (p) {
				char *pp = strstr(p, "pid=");
				if (pp) {
					sscanf(pp, "pid=%d", &pid);
					is_pick = 1;
				}
			}
		}

		if (!is_pick || pid <= 0 || pid >= 65536 || !pid_is_ours[pid])
			continue;

		/* Extract nice from "nice=N" */
		p = strstr(line, "nice=");
		if (p) sscanf(p, "nice=%d", &nice);

		/* Extract timestamp */
		double ts = 0;
		p = line;
		while (*p && *p != ':') p++;
		if (*p == ':') {
			/* skip to the function timestamp like "123.456789:" */
			char *ts_start = NULL;
			for (char *s = line; s < p; s++) {
				if (*s >= '0' && *s <= '9' && (s == line || *(s-1) == ' ')) {
					/* Look for NNN.NNN: pattern */
					char *dot = strchr(s, '.');
					if (dot && dot < p) {
						ts_start = s;
						break;
					}
				}
			}
			if (ts_start) ts = strtod(ts_start, NULL);
		}

		/* New event if timestamp gap > 100ms */
		if (ts - prev_ts > 0.1 || first_in_event) {
			if (!first_in_event && res->events > 0) {
				/* Record previous event's first selection */
				/* already recorded below */
			}
			first_in_event = 0;
			prev_ts = ts;

			int heavy = pid_is_heavy[pid];
			if (heavy)
				res->heavy_first++;
			else
				res->light_first++;
			res->events++;
		}

		/* Count totals */
		if (pid_is_heavy[pid])
			res->heavy_total++;
		else
			res->light_total++;
	}

	fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "  -H N    hot CPU workers total (default: 8)\n"
	    "  -h N    heavy workers among hot (default: 2)\n"
	    "  -n N    heavy nice (default: -3)\n"
	    "  -N N    light nice (default: 5)\n"
	    "  -F N    fillers per non-hot CPU (default: 1)\n"
	    "  -f N    filler nice value (default: -3)\n"
	    "  -T S    measurement duration seconds (default: 30)\n"
	    "  -S S    settle time before measurement (default: 2)\n"
	    "  -C CPU  hot CPU (default: 0)\n",
	    prog);
	exit(1);
}

int main(int argc, char **argv)
{
	int n_hot        = 8;
	int n_heavy      = 2;
	int heavy_nice   = -3;
	int light_nice   = 5;
	int fillers_per  = 1;
	int filler_nice  = -3;
	int duration_s   = 30;
	int settle_s     = 2;
	int hot_cpu      = 0;

	int c;
	while ((c = getopt(argc, argv, "H:h:n:N:F:f:T:S:C:")) != -1) {
		switch (c) {
		case 'H': n_hot       = atoi(optarg); break;
		case 'h': n_heavy     = atoi(optarg); break;
		case 'n': heavy_nice  = atoi(optarg); break;
		case 'N': light_nice  = atoi(optarg); break;
		case 'F': fillers_per = atoi(optarg); break;
		case 'f': filler_nice = atoi(optarg); break;
		case 'T': duration_s  = atoi(optarg); break;
		case 'S': settle_s    = atoi(optarg); break;
		case 'C': hot_cpu     = atoi(optarg); break;
		default:  usage(argv[0]);
		}
	}

	num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpus < 3) {
		fprintf(stderr, "Error: need at least 3 CPUs\n");
		return 1;
	}
	int n_light = n_hot - n_heavy;
	if (n_heavy > n_hot || n_hot > MAX_HOT_WORKERS || n_light < 0) {
		fprintf(stderr, "Error: invalid n_heavy=%d / n_hot=%d\n",
			n_heavy, n_hot);
		return 1;
	}

	int monitor_cpu = (hot_cpu == 0) ? 1 : 0;

	/* CFS nice→weight table (nice -20..+19 → index 0..39) */
	static const int prio_to_weight[] = {
	/* -20 */ 88761, 71755, 56483, 46273, 36291,
	/* -15 */ 29154, 23254, 18705, 14949, 11916,
	/* -10 */  9548,  7620,  6100,  4904,  3906,
	/*  -5 */  3121,  2501,  1991,  1586,  1277,
	/*   0 */  1024,   820,   655,   526,   423,
	/*   5 */   335,   272,   215,   172,   137,
	/*  10 */   110,    87,    70,    56,    45,
	/*  15 */    36,    29,    23,    18,    15,
	};
	int hw = prio_to_weight[heavy_nice + 20];
	int lw = prio_to_weight[light_nice + 20];
	int fw = prio_to_weight[filler_nice + 20];
	int hot_weight = n_heavy * hw + n_light * lw;

	int cambyses_on = 0;
	{
		FILE *fp = fopen("/proc/sys/kernel/sched_cambyses", "r");
		if (fp) { int r = fscanf(fp, "%d", &cambyses_on); (void)r; fclose(fp); }
	}

	printf("\n=== Selection Quality Benchmark (trace-based) ===\n");
	printf("CPUs:       %d\n", num_cpus);
	printf("Hot CPU:    %d  (%d workers: %d heavy nice %d w=%d, "
	       "%d light nice %d w=%d)\n",
	       hot_cpu, n_hot, n_heavy, heavy_nice, hw,
	       n_light, light_nice, lw);
	printf("Hot weight: %d\n", hot_weight);
	printf("Fillers:    %d per CPU (nice %d w=%d)\n",
	       fillers_per, filler_nice, fw);
	printf("Duration:   %ds  (settle: %ds)\n", duration_s, settle_s);
	printf("Cambyses:   %s\n", cambyses_on ? "enabled" : "disabled (vanilla)");
	printf("Baseline:   %.0f%% heavy (random)\n\n",
	       100.0 * n_heavy / n_hot);

	/* ---- Pin main ---- */
	pin_to_cpu(monitor_cpu);
	struct sched_param sp = { .sched_priority = 50 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		perror("main: sched_setscheduler SCHED_FIFO");

	/* ---- Create hot workers ---- */
	struct hot_worker workers[MAX_HOT_WORKERS];
	pthread_t         wthr[MAX_HOT_WORKERS];

	for (int i = 0; i < n_hot; i++) {
		workers[i].id       = i;
		workers[i].is_heavy = (i < n_heavy);
		workers[i].nice_val = (i < n_heavy) ? heavy_nice : light_nice;
		workers[i].tid      = 0;
		atomic_store(&workers[i].stop, 0);
		pthread_create(&wthr[i], NULL, hot_worker_fn, &workers[i]);
		pin_thread(wthr[i], hot_cpu);
	}

	/* ---- Create fillers ---- */
	int n_fillers = 0;
	int n_filler_cpus = num_cpus - 1; /* all except monitor */
	int n_fillers_alloc = n_filler_cpus * fillers_per;
	if (n_fillers_alloc > MAX_FILLERS) n_fillers_alloc = MAX_FILLERS;
	struct filler *fillers = calloc(n_fillers_alloc, sizeof(struct filler));
	pthread_t     *fthr    = calloc(n_fillers_alloc, sizeof(pthread_t));

	for (int cpu = 0; cpu < num_cpus; cpu++) {
		if (cpu == hot_cpu || cpu == monitor_cpu)
			continue;
		for (int k = 0; k < fillers_per; k++) {
			if (n_fillers >= n_fillers_alloc) break;
			fillers[n_fillers].cpu = cpu;
			fillers[n_fillers].nice_val = filler_nice;
			atomic_store(&fillers[n_fillers].stop, 0);
			pthread_create(&fthr[n_fillers], NULL, filler_fn,
				       &fillers[n_fillers]);
			n_fillers++;
		}
	}

	/* ---- Settle: let PELT converge ---- */
	printf("Settling for %d seconds...\n", settle_s);
	fflush(stdout);
	sleep(settle_s);

	/* Wait for all worker TIDs to be set */
	for (int i = 0; i < n_hot; i++) {
		while (workers[i].tid == 0)
			usleep(1000);
	}

	/* Release worker affinity so the load balancer can migrate them */
	{
		cpu_set_t all_cpus;
		CPU_ZERO(&all_cpus);
		for (int cpu = 0; cpu < num_cpus; cpu++)
			CPU_SET(cpu, &all_cpus);
		for (int i = 0; i < n_hot; i++)
			pthread_setaffinity_np(wthr[i], sizeof(all_cpus),
					       &all_cpus);
	}

	/* Print worker PIDs */
	printf("Worker PIDs: ");
	for (int i = 0; i < n_hot; i++) {
		printf("%d(%s) ", workers[i].tid,
		       workers[i].is_heavy ? "H" : "L");
	}
	printf("\n");

	/* ---- Enable trace ---- */
	write_file(TRACE_PATH "/tracing_on", "0");
	write_file(TRACE_PATH "/trace", "");   /* clear */
	write_file(TRACE_PATH "/tracing_on", "1");

	printf("Tracing for %d seconds...\n", duration_s);
	fflush(stdout);
	sleep(duration_s);

	write_file(TRACE_PATH "/tracing_on", "0");
	printf("Trace complete. Parsing...\n");

	/* ---- Parse trace ---- */
	struct trace_result res;
	parse_trace(workers, n_hot, &res);

	/* ---- Stop workers ---- */
	for (int i = 0; i < n_hot; i++)
		atomic_store_explicit(&workers[i].stop, 1, memory_order_release);
	for (int i = 0; i < n_fillers; i++)
		atomic_store_explicit(&fillers[i].stop, 1, memory_order_release);
	for (int i = 0; i < n_hot; i++)
		pthread_join(wthr[i], NULL);
	for (int i = 0; i < n_fillers; i++)
		pthread_join(fthr[i], NULL);

	/* ---- Summary ---- */
	int total_first = res.heavy_first + res.light_first;
	double heavy_pct = total_first > 0
		? 100.0 * res.heavy_first / total_first : 0;
	double expected_pct = 100.0 * n_heavy / n_hot;

	printf("\n=== Results ===\n");
	printf("  Balance events:    %d\n", res.events);
	printf("  First-selected:    heavy=%d  light=%d\n",
	       res.heavy_first, res.light_first);
	printf("  Heavy first rate:  %.1f%%\n", heavy_pct);
	printf("  Expected (random): %.1f%%\n", expected_pct);
	printf("  Preference ratio:  %.2fx\n",
	       expected_pct > 0 ? heavy_pct / expected_pct : 0);
	printf("  Total detached:    heavy=%d  light=%d\n",
	       res.heavy_total, res.light_total);
	printf("RESULT heavy_first_pct=%.1f events=%d preference=%.2f "
	       "heavy_total=%d light_total=%d\n",
	       heavy_pct, res.events,
	       expected_pct > 0 ? heavy_pct / expected_pct : 0,
	       res.heavy_total, res.light_total);

	free(fillers);
	free(fthr);
	return 0;
}
