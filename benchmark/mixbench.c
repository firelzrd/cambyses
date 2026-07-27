/*
 * mixbench -- a workload built to discriminate on lb_task_score().
 *
 * The ranked pick prefers max(staleness x runnable_avg).  runnable_avg counts
 * time ON the runqueue, not time running, so a starved low-priority task that
 * never sleeps sits near 1024 while its staleness grows large.  A nice-0 task
 * on the same runqueue is scheduled constantly, so its staleness stays small.
 * That makes the two classes differ in score by roughly the CFS weight ratio.
 *
 * Pair that with opposite migration costs and the choice starts to matter:
 *
 *   HEAVY  nice 0,  512KB working set, pointer-chased  -> low score, dear to move
 *   LIGHT  nice 19, 4KB working set,   arithmetic      -> high score, free to move
 *
 * Vanilla picks by position in cfs_tasks, so with a 1:1 mix it moves the
 * expensive class about half the time.  Ranking should move it far less.
 *
 * Reports per-class throughput and per-class se.nr_migrations, so the
 * mechanism prediction (heavy migrates less) is falsifiable independently of
 * whether throughput follows.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static int heavy_kb = 512, light_kb = 4, run_secs = 10, sleep_us = 400,
	   sleep_every = 20000;

struct th {
	pthread_t t;
	int heavy, nice;
	pid_t tid;
	unsigned long long iters;
	unsigned long migrations;
};

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned long read_migrations(pid_t tid)
{
	char p[64], line[256];
	unsigned long v = 0;
	FILE *f;

	snprintf(p, sizeof p, "/proc/self/task/%d/sched", tid);
	f = fopen(p, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (strstr(line, "se.nr_migrations") && !strstr(line, "cold")) {
			char *c = strchr(line, ':');
			if (c)
				v = strtoul(c + 1, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

/* Build a cyclic pointer chase over the buffer so the loop is serialised on
 * cache latency -- that is what makes losing L2 on migration visible. */
static void **make_chain(size_t bytes)
{
	size_t n = bytes / 64, i;
	void **buf = aligned_alloc(64, n * 64);
	size_t *idx = malloc(n * sizeof *idx);

	if (!buf || !idx)
		exit(1);
	memset(buf, 0, n * 64);
	for (i = 0; i < n; i++)
		idx[i] = i;
	for (i = n - 1; i > 0; i--) {	/* deterministic shuffle */
		size_t j = (i * 1103515245UL + 12345UL) % (i + 1);
		size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
	}
	for (i = 0; i < n; i++)
		buf[idx[i] * 8] = &buf[idx[(i + 1) % n] * 8];
	free(idx);
	return buf;
}

static void *worker(void *arg)
{
	struct th *th = arg;
	unsigned long long it = 0;

	th->tid = syscall(SYS_gettid);
	if (setpriority(PRIO_PROCESS, th->tid, th->nice))
		/* nice may fail without privilege when lowering; ignore */;

	if (th->heavy) {
		void **p = make_chain((size_t)heavy_kb * 1024);
		void **c = p;
		while (!stop) {
			int k;
			for (k = 0; k < 512; k++)
				c = (void **)*c;
			it++;
		}
		__asm__ volatile("" :: "r"(c));
	} else {
		volatile unsigned long acc = 0;
		char *small = malloc((size_t)light_kb * 1024);
		memset(small, 1, (size_t)light_kb * 1024);
		while (!stop) {
			int k;
			for (k = 0; k < 512; k++)
				acc += small[(k * 64) % (light_kb * 1024)];
			it++;
			/* brief sleeps free CPUs, so newidle balance keeps firing
			 * and the balancer keeps having to choose */
			if (sleep_us && (it % sleep_every) == 0)
				usleep(sleep_us);
		}
		free(small);
	}
	th->iters = it;
	th->migrations = read_migrations(th->tid);
	return NULL;
}

int main(int argc, char **argv)
{
	int nheavy = 8, nlight = 8, i;
	int cls_nice[8], cls_cnt[8], ncls = 0;
	struct th *th;
	double t0;
	unsigned long long hi = 0, li = 0;
	unsigned long hm = 0, lm = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--heavy")) nheavy = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--light")) nlight = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--heavy-kb")) heavy_kb = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--secs")) run_secs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--sleep-us")) sleep_us = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--sleep-every")) sleep_every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--class")) {   /* nice:count, all same work */
			char *a = argv[++i], *c = strchr(a, ':');
			cls_nice[ncls] = atoi(a); cls_cnt[ncls] = atoi(c + 1); ncls++;
		}
	}

	if (ncls) {			/* class mode: every thread does heavy work */
		int n = 0, k, j;
		for (k = 0; k < ncls; k++) n += cls_cnt[k];
		nheavy = n; nlight = 0;
		th = calloc(n, sizeof *th);
		for (k = 0, i = 0; k < ncls; k++)
			for (j = 0; j < cls_cnt[k]; j++, i++) {
				th[i].heavy = 1; th[i].nice = cls_nice[k];
			}
	} else {
	th = calloc(nheavy + nlight, sizeof *th);
	for (i = 0; i < nheavy + nlight; i++) {
		th[i].heavy = i < nheavy;
		th[i].nice = th[i].heavy ? 0 : 19;
	}
	}
	t0 = now_s();
	for (i = 0; i < nheavy + nlight; i++)
		pthread_create(&th[i].t, NULL, worker, &th[i]);
	usleep(run_secs * 1000000);
	stop = 1;
	for (i = 0; i < nheavy + nlight; i++)
		pthread_join(th[i].t, NULL);
	double el = now_s() - t0;

	if (ncls) {
		int k;
		printf("MIXCLASS");
		for (k = 0; k < ncls; k++) {
			unsigned long long it = 0; unsigned long mg = 0; int n = 0;
			for (i = 0; i < nheavy; i++)
				if (th[i].nice == cls_nice[k]) {
					it += th[i].iters; mg += th[i].migrations; n++;
				}
			printf(" n%d:thr=%.0f,mig=%lu,cnt=%d",
			       cls_nice[k], it / el, mg, n);
		}
		printf(" elapsed=%.2f\n", el);
		free(th);
		return 0;
	}
	for (i = 0; i < nheavy + nlight; i++) {
		if (th[i].heavy) { hi += th[i].iters; hm += th[i].migrations; }
		else             { li += th[i].iters; lm += th[i].migrations; }
	}
	printf("MIXBENCH heavy_thr=%.1f light_thr=%.1f heavy_mig=%lu light_mig=%lu "
	       "heavy_mig_share=%.3f elapsed=%.2f\n",
	       hi / el, li / el, hm, lm,
	       (hm + lm) ? (double)hm / (hm + lm) : 0.0, el);
	free(th);
	return 0;
}
