/*
 *  Copyright (C) 2018  Roman Penyaev
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Roman Penyaev <r.peniaev@gmail.com>
 *
 *  Purpose of the tool is to generate N events from different threads and to
 *  measure how fast those events will be delivered to thread which does epoll.
 *
 *  $ gcc -o stress-epoll stress-epoll.c -O2 -lpthread -lnuma
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>
#include <numa.h>

#define ITERS     1000000ull

struct thread_ctx {
	pthread_t thread;
	int efd;
};

struct cpu_map {
	unsigned int nr;
	unsigned int map[];
};

static volatile unsigned int thr_ready;
static volatile unsigned int start;

static int is_cpu_online(int cpu)
{
	char buf[64];
	char online;
	FILE *f;
	int rc;

	snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu%d/online", cpu);
	f = fopen(buf, "r");
	if (!f)
		return 1;

	rc = fread(&online, 1, 1, f);
	assert(rc == 1);
	fclose(f);

	return (char)online == '1';
}

static struct cpu_map *cpu_map__new(void)
{
	struct cpu_map *cpu;
	struct bitmask *bm;

	int i, bit, cpus_nr;

	cpus_nr = numa_num_possible_cpus();
	cpu = calloc(1, sizeof(*cpu) + sizeof(cpu->map[0]) * cpus_nr);
	if (!cpu)
		return NULL;

	bm = numa_all_cpus_ptr;
	assert(bm);

	for (bit = 0, i = 0; bit < bm->size; bit++) {
		if (numa_bitmask_isbitset(bm, bit) && is_cpu_online(bit)) {
			cpu->map[i++] = bit;
		}
	}
	cpu->nr = i;

	return cpu;
}

static void cpu_map__put(struct cpu_map *cpu)
{
	free(cpu);
}

static inline unsigned long long nsecs(void)
{
	struct timespec ts = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((unsigned long long)ts.tv_sec * 1000000000ull) + ts.tv_nsec;
}

static void *thread_work(void *arg)
{
	struct thread_ctx *ctx = arg;
	uint64_t ucnt = 1;
	unsigned int i;
	int rc;

	__atomic_add_fetch(&thr_ready, 1, __ATOMIC_RELAXED);

	while (!start)
		;

	for (i = 0; i < ITERS; i++) {
		rc = write(ctx->efd, &ucnt, sizeof(ucnt));
		assert(rc == sizeof(ucnt));
	}

	return NULL;
}

static int do_bench(struct cpu_map *cpu, unsigned int nthreads)
{
	struct epoll_event ev, events[nthreads];
	struct thread_ctx threads[nthreads];
	pthread_attr_t thrattr;
	struct thread_ctx *ctx;
	int rc, epfd, nfds;
	cpu_set_t cpuset;
	unsigned int i;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0;

	epfd = epoll_create1(0);
	if (epfd < 0)
		err(EXIT_FAILURE, "epoll_create1");

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];

		ctx->efd = eventfd(0, EFD_NONBLOCK);
		if (ctx->efd < 0)
			err(EXIT_FAILURE, "eventfd");

		ev.events = EPOLLIN;
		ev.data.ptr = ctx;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->efd, &ev);
		if (rc)
			err(EXIT_FAILURE, "epoll_ctl");

		CPU_ZERO(&cpuset);
		CPU_SET(cpu->map[i % cpu->nr], &cpuset);

		pthread_attr_init(&thrattr);
		rc = pthread_attr_setaffinity_np(&thrattr, sizeof(cpu_set_t),
						 &cpuset);
		if (rc) {
			errno = rc;
			err(EXIT_FAILURE, "pthread_attr_setaffinity_np");
		}

		rc = pthread_create(&ctx->thread, NULL, thread_work, ctx);
		if (rc) {
			errno = rc;
			err(EXIT_FAILURE, "pthread_create");
		}
	}

	while (thr_ready == nthreads)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = epoll_wait(epfd, events, nthreads, -1);
		if (nfds < 0)
			err(EXIT_FAILURE, "epoll_wait");

		epoll_calls++;

		for (i = 0; i < (unsigned int)nfds; ++i) {
			ctx = events[i].data.ptr;
			rc = read(ctx->efd, &ucnt, sizeof(ucnt));
			assert(rc == sizeof(ucnt));
			ucnt_sum += ucnt;
			if (ucnt_sum == nthreads * ITERS)
				goto end;
		}
	}
end:
	epoll_nsecs = nsecs() - epoll_nsecs;

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];
		pthread_join(ctx->thread, NULL);
	}

	printf("%7d   %8lld     %8lld\n",
		   nthreads,
		   ITERS*nthreads/(epoll_nsecs/1000/1000),
		   epoll_nsecs/1000/1000);

	return 0;
}

int main(int argc, char *argv[])
{
	unsigned int i, nthreads_arr[] = {8, 16, 32, 64, 128};
	struct cpu_map *cpu;

	(void)argc; (void)argv;

	cpu = cpu_map__new();
	if (!cpu) {
		errno = ENOMEM;
		err(EXIT_FAILURE, "cpu_map__new");
	}

	printf("threads  events/ms  run-time ms\n");
	for (i = 0; i < sizeof(nthreads_arr)/sizeof(nthreads_arr[0]); i++)
		do_bench(cpu, nthreads_arr[i]);

	cpu_map__put(cpu);

	return 0;
}
