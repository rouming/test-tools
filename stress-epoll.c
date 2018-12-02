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
#include <time.h>
#include <assert.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#define ITERS 10000000ull
#define THRDS 8

struct thread_ctx {
	pthread_t thread;
	int efd;
};

static struct thread_ctx threads[THRDS];
static volatile int thr_ready;
static volatile int start;

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
	int i, rc;

	__atomic_add_fetch(&thr_ready, 1, __ATOMIC_RELAXED);

	while (!start)
		;

	for (i = 0; i < ITERS; i++) {
		rc = write(ctx->efd, &ucnt, sizeof(ucnt));
		assert(rc == sizeof(ucnt));
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	struct epoll_event ev, events[THRDS];
	struct thread_ctx *ctx;
	int i, rc, epfd, nfds;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0;

	epfd = epoll_create1(0);
	assert(epfd >= 0);

	for (i = 0; i < THRDS; i++) {
		ctx = &threads[i];

		ctx->efd = eventfd(0, EFD_NONBLOCK);
		assert(ctx->efd >= 0);

		ev.events = EPOLLIN;
		ev.data.ptr = ctx;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->efd, &ev);
		assert(rc == 0);

		rc = pthread_create(&ctx->thread, NULL, thread_work, ctx);
		assert(rc == 0);
	}

	while (thr_ready == THRDS)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = epoll_wait(epfd, events, THRDS, -1);
		assert(nfds > 0);

		epoll_calls++;

		for (i = 0; i < nfds; ++i) {
			ctx = events[i].data.ptr;
			rc = read(ctx->efd, &ucnt, sizeof(ucnt));
			assert(rc == sizeof(ucnt));
			ucnt_sum += ucnt;
			if (ucnt_sum == THRDS * ITERS)
				goto end;
		}
	}
end:
	epoll_nsecs = nsecs() - epoll_nsecs;

	for (i = 0; i < THRDS; i++) {
		ctx = &threads[i];
		pthread_join(ctx->thread, NULL);
	}

	printf("threads  events/ms  run-time ms\n");
	printf("%7d   %8lld     %8lld\n",
		   THRDS,
		   ITERS*THRDS/(epoll_nsecs/1000/1000),
		   epoll_nsecs/1000/1000);

	return 0;
}
