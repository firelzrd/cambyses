// SPDX-License-Identifier: GPL-2.0
/*
 * fill - how long an idle CPU in another LLC waits for work, and how much
 * throughput that wait costs.
 *
 * Four CPUs: A0, A1 share one LLC, B0, B1 share another.  Three CPU hogs are
 * pinned to {A0, A1}, so that LLC is overloaded.  B0 runs one more hog and B1
 * runs a duty-cycled task (busy RUN ms, sleep SLEEP ms), so B1 goes idle for
 * SLEEP ms at a time while still carrying blocked utilization.  Optionally a
 * light ticker, also on B1, wakes every TICK_US microseconds and sleeps again;
 * each time it sleeps, B1 runs a newidle balance.
 *
 * Each trial or cycle pins the three hogs to {A0, A1} for STACK_MS, then
 * widens their affinity to all four CPUs.
 *
 *   lat   time until one of the three hogs first runs in LLC B (ms)
 *   tput  CPU time and work the three hogs get during the FREE_MS after the
 *         widening; work is in ms of a hog running alone on a CPU
 *
 * The hogs timestamp their own arrival in LLC B; the main thread sleeps
 * throughout, so it does not add balance opportunities of its own.
 *
 * Run it inside a cpuset partition that contains exactly these four CPUs, so
 * that the load balancer sees two LLC groups of two CPUs; see fillab.sh.
 *
 * usage: fill A0 A1 B0 B1 RUN_MS SLEEP_MS lat TRIALS
 *        fill A0 A1 B0 B1 RUN_MS SLEEP_MS tput FREE_MS CYCLES
 * environment: TICK_US (default 0, no ticker), STACK_MS (300), WARMUP (5)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define NHOG		4	/* hogs 0..2 move, hog 3 stays on B0 */
#define CHUNK		20000
#define TIMEOUT_MS	1000.0

static volatile unsigned long work[NHOG];
static volatile pid_t tid[NHOG];
static volatile int stop;
static pthread_t th[NHOG];
static int A0, A1, B0, B1, tick_us;
static double run_ms, sleep_ms;

/* First arrival of a moving hog in LLC B; 0 while none has arrived. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static volatile double first_b;

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sleep_for(double ms)
{
	struct timespec ts = { (time_t)(ms / 1e3), (long)((ms - (time_t)(ms / 1e3) * 1e3) * 1e6) };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static double thread_cpu_ms(pthread_t t)
{
	struct timespec ts;
	clockid_t c;

	pthread_getcpuclockid(t, &c);
	clock_gettime(c, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void pin(pid_t t, int n, const int *cpus)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	for (int i = 0; i < n; i++)
		CPU_SET(cpus[i], &s);
	if (sched_setaffinity(t, sizeof(s), &s)) {
		perror("sched_setaffinity");
		exit(1);
	}
}

static void pin_self(int cpu)
{
	pin(syscall(SYS_gettid), 1, &cpu);
}

static void *hog(void *arg)
{
	long i = (long)arg;
	volatile unsigned long x = 0;

	tid[i] = syscall(SYS_gettid);
	while (!stop) {
		for (int k = 0; k < CHUNK; k++)
			x += k;
		work[i]++;
		if (i < 3 && first_b == 0) {
			int c = sched_getcpu();

			if (c == B0 || c == B1) {
				pthread_mutex_lock(&lock);
				if (first_b == 0) {
					first_b = now_ms();
					pthread_cond_signal(&arrived);
				}
				pthread_mutex_unlock(&lock);
			}
		}
	}
	return NULL;
}

static void *duty(void *arg)
{
	volatile unsigned long x = 0;

	(void)arg;
	pin_self(B1);
	while (!stop) {
		double s = now_ms();

		while (now_ms() - s < run_ms)
			x++;
		sleep_for(sleep_ms);
	}
	return NULL;
}

static void *ticker(void *arg)
{
	struct timespec ts = { 0, tick_us * 1000L };

	(void)arg;
	pin_self(B1);
	while (!stop)
		nanosleep(&ts, NULL);
	return NULL;
}

/* Pin the movers to LLC A, let them settle, then release them. */
static double stack_and_release(double stack_ms)
{
	int a[2] = { A0, A1 }, all[4] = { A0, A1, B0, B1 };
	double t0;

	for (int i = 0; i < 3; i++)
		pin(tid[i], 2, a);
	sleep_for(stack_ms);
	pthread_mutex_lock(&lock);
	first_b = 0;
	pthread_mutex_unlock(&lock);
	t0 = now_ms();
	for (int i = 0; i < 3; i++)
		pin(tid[i], 4, all);
	return t0;
}

/* Wait until a mover arrives in LLC B or the timeout passes; ms or -1. */
static double wait_arrival(double t0)
{
	struct timespec dl;
	double end = t0 + TIMEOUT_MS, d;

	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += (time_t)(TIMEOUT_MS / 1e3);
	pthread_mutex_lock(&lock);
	while (first_b == 0 && now_ms() < end)
		if (pthread_cond_timedwait(&arrived, &lock, &dl) == ETIMEDOUT)
			break;
	d = first_b > 0 ? first_b - t0 : -1;
	pthread_mutex_unlock(&lock);
	return d;
}

static int cmp(const void *a, const void *b)
{
	double p = *(const double *)a, q = *(const double *)b;

	return p < q ? -1 : p > q;
}

int main(int argc, char **argv)
{
	double stack_ms, sum = 0;
	int warmup, n;
	pthread_t dt, tt;
	int b0[1];

	if (argc < 9 || (strcmp(argv[7], "lat") && strcmp(argv[7], "tput")) ||
	    (!strcmp(argv[7], "tput") && argc < 10)) {
		fprintf(stderr, "usage: %s A0 A1 B0 B1 RUN_MS SLEEP_MS lat TRIALS\n"
				"       %s A0 A1 B0 B1 RUN_MS SLEEP_MS tput FREE_MS CYCLES\n", argv[0], argv[0]);
		return 2;
	}
	A0 = atoi(argv[1]); A1 = atoi(argv[2]);
	B0 = atoi(argv[3]); B1 = atoi(argv[4]);
	run_ms = atof(argv[5]); sleep_ms = atof(argv[6]);
	tick_us = getenv("TICK_US") ? atoi(getenv("TICK_US")) : 0;
	stack_ms = getenv("STACK_MS") ? atof(getenv("STACK_MS")) : 300;
	/* The first trials run before PELT has settled; discard them. */
	warmup = getenv("WARMUP") ? atoi(getenv("WARMUP")) : 5;

	for (long i = 0; i < NHOG; i++)
		pthread_create(&th[i], NULL, hog, (void *)i);
	pthread_create(&dt, NULL, duty, NULL);
	if (tick_us > 0)
		pthread_create(&tt, NULL, ticker, NULL);
	usleep(100000);
	b0[0] = B0;
	pin(tid[3], 1, b0);

	if (!strcmp(argv[7], "lat")) {
		int trials = atoi(argv[8]), timeouts = 0;
		double *lat = calloc(trials, sizeof(*lat));

		for (n = -warmup; n < trials; n++) {
			double d = wait_arrival(stack_and_release(stack_ms));

			if (n < 0)
				continue;
			if (d < 0) {
				d = TIMEOUT_MS;
				timeouts++;
			}
			lat[n] = d;
			printf("trial %d %.3f\n", n, d);
		}
		qsort(lat, trials, sizeof(*lat), cmp);
		for (n = 0; n < trials; n++)
			sum += lat[n];
		printf("FILL trials=%d run=%g sleep=%g tick=%d mean=%.2f p50=%.2f p90=%.2f max=%.2f timeouts=%d\n",
		       trials, run_ms, sleep_ms, tick_us, sum / trials, lat[trials / 2],
		       lat[trials * 9 / 10], lat[trials - 1], timeouts);
	} else {
		double free_ms = atof(argv[8]), rate, wsum = 0;
		int cycles = atoi(argv[9]);

		/* Work rate of a hog with a CPU to itself: hog 3, alone on B0. */
		{
			double c = thread_cpu_ms(th[3]);
			unsigned long w = work[3];

			sleep_for(500);
			rate = (work[3] - w) / (thread_cpu_ms(th[3]) - c);
		}
		for (n = -warmup; n < cycles; n++) {
			double c0[3], cpu = 0, wk = 0, t0, wait;
			unsigned long w0[3];

			for (int i = 0; i < 3; i++)
				pin(tid[i], 2, (int[]){ A0, A1 });
			sleep_for(stack_ms);
			for (int i = 0; i < 3; i++) {
				c0[i] = thread_cpu_ms(th[i]);
				w0[i] = work[i];
			}
			pthread_mutex_lock(&lock);
			first_b = 0;
			pthread_mutex_unlock(&lock);
			t0 = now_ms();
			for (int i = 0; i < 3; i++)
				pin(tid[i], 4, (int[]){ A0, A1, B0, B1 });
			sleep_for(free_ms);
			for (int i = 0; i < 3; i++) {
				cpu += thread_cpu_ms(th[i]) - c0[i];
				wk += (work[i] - w0[i]) / rate;
			}
			wait = first_b > 0 ? first_b - t0 : -1;
			if (n < 0)
				continue;
			printf("cycle %d cpu=%.3f work=%.3f wait=%.3f\n", n, cpu, wk, wait);
			sum += cpu;
			wsum += wk;
		}
		printf("REFILL cycles=%d run=%g sleep=%g tick=%d free=%g hog_cpu=%.2f hog_work=%.2f ms/cycle\n",
		       cycles, run_ms, sleep_ms, tick_us, free_ms, sum / cycles, wsum / cycles);
	}
	stop = 1;
	return 0;
}
