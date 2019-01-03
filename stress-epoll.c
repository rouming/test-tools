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
 *  $ gcc -o stress-epoll stress-epoll.c -O2 -lpthread
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

#define ITERS     1000000ull

struct thread_ctx {
	pthread_t thread;
	int efd;
};

static volatile unsigned int thr_ready;
static volatile unsigned int start;

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

static int do_bench(unsigned int nthreads)
{
	struct epoll_event ev, events[nthreads];
	struct thread_ctx threads[nthreads];
	struct thread_ctx *ctx;
	int rc, epfd, nfds;
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

	(void)argc; (void)argv;

	printf("threads  events/ms  run-time ms\n");
	for (i = 0; i < sizeof(nthreads_arr)/sizeof(nthreads_arr[0]); i++)
		do_bench(nthreads_arr[i]);

	return 0;
}
