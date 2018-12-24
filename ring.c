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
 *  Purpose of the tool is to emulate eventfd and epoll_wait in userspace.
 *
 *  $ gcc -o ring ring.c -O2 -Wall -lpthread
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <search.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifndef bool
#define bool _Bool
enum {
	false = 0,
	true  = 1
};
#endif



#define READ_ONCE(v) (*(volatile typeof(v)*)&v)

#define rmb()	asm volatile("lfence":::"memory")

#define L1_CACHE_BYTES 64
#define __cacheline_aligned __attribute__((__aligned__(L1_CACHE_BYTES)))

enum {
	UNSIGNALED = 0,
	SIGNALED   = 1,
	READING    = 2,
	WRITING    = 3,

	ALL        = (SIGNALED | READING | WRITING)
};

struct user_epitem {
	unsigned events;
	struct epoll_event event;
};

struct user_header {
	unsigned magic;
	unsigned header_length; /* length of the header */
	unsigned items_length;  /* length of the items array */
	unsigned index_length;  /* length of the index ring */
	unsigned nr;            /* number of ep items */
	unsigned head;          /* updated by userland */
	unsigned tail;          /* updated by kernel */
	unsigned padding;

	struct user_epitem items[] __cacheline_aligned;
};

struct ring {
	struct user_header *user_header;
	unsigned           *user_itemsindex;

	unsigned nr;
};

static inline void add_event__kernel(struct ring *ring, unsigned bit)
{
	unsigned i, *item_idx;

	i = __atomic_fetch_add(&ring->user_header->tail, 1, __ATOMIC_ACQUIRE);
	item_idx = &ring->user_itemsindex[i % ring->nr];

	/* Update data */
	__atomic_store_n(item_idx, bit + 1, __ATOMIC_RELEASE);
}

static inline bool read_event__user(struct ring *ring, unsigned idx,
				    struct epoll_event *event)
{
	struct user_epitem *item;
	unsigned *item_idx;

	item_idx = &ring->user_itemsindex[idx % ring->nr];

	while (!(idx = __atomic_load_n(item_idx, __ATOMIC_RELAXED)))
		;

	/* XXX Check item_idx */
	item = &ring->user_header->items[idx - 1];

	*item_idx = 0;

	/*
	 * Fetch data first, if event is cleared by the kernel we drop the data
	 * returning false.
	 */
	event->data = item->event.data;
	event->events = __sync_lock_test_and_set(&item->events, 0);

	return !!event->events;
}

static inline bool update_event__kernel(struct ring *ring, unsigned idx,
					int events)
{
	struct user_epitem *item;

	/* XXX Check item_idx */
	item = &ring->user_header->items[idx];

	assert(sizeof(struct user_epitem) == 16);
	assert(!((uintptr_t)item & 15));
	assert(!((uintptr_t)&item->events & 15));

	return !!__atomic_fetch_or(&item->events, events, __ATOMIC_ACQUIRE);
}

static inline void free_event__kernel(struct ring *ring, unsigned idx)
{
	struct user_epitem *item;
	unsigned item_idx;

	item_idx = ring->user_itemsindex[idx % ring->nr];
	/* XXX Check item_idx */
	item = &ring->user_header->items[item_idx];

	__sync_lock_test_and_set(&item->events, 0);
}


#define ITERS 10000000ull
#define THRDS 256


struct uepollfd {
	struct ring ring;
};

struct ueventfd {
	unsigned long long count;
	struct uepollfd    *epfd;
	unsigned           efd_bit;
	struct epoll_event event;
};

struct thread_ctx {
	pthread_t       thread;
	struct ueventfd efd;
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

static void uepoll_create(struct uepollfd *epfd)
{
	const size_t size = 4<<12;

	memset(epfd, 0, sizeof(*epfd));

	epfd->ring.user_header = aligned_alloc(L1_CACHE_BYTES, size);
	assert(epfd->ring.user_header);
	memset(epfd->ring.user_header, 0, size);

	epfd->ring.user_itemsindex = aligned_alloc(L1_CACHE_BYTES, size);
	assert(epfd->ring.user_itemsindex);
	memset(epfd->ring.user_itemsindex, 0, size);
}

static void uepoll_ctl(struct uepollfd *epfd, int op, struct ueventfd *efd,
		       struct epoll_event *event)

{
	struct ring *ring = &epfd->ring;
	struct user_epitem *item;

	assert(op == EPOLL_CTL_ADD);
	/* No pending events so far */
	assert(efd->count == 0);
	assert(efd->epfd == NULL);

	efd->epfd = epfd;
	efd->efd_bit = epfd->ring.nr++;
	efd->event = *event;

	ring->user_header->nr = epfd->ring.nr;

	/* That is racy with possible access from userspace, but we do not care */
	item = &ring->user_header->items[efd->efd_bit];
	item->event = *event;
	item->events = 0;
}

static int uepoll_wait(struct uepollfd *epfd, struct epoll_event *events,
		       int maxevents)

{
	struct ring *ring = &epfd->ring;
	struct user_header *header = ring->user_header;
	unsigned tail;
	int i;

	assert(maxevents > 0);

	/*
	 * Cache the tail because we don't want refetch it on each iteration
	 * and then catch live events updates, i.e. we don't want user @events
	 * array consist of events from the same fds.
	 */
	tail = READ_ONCE(header->tail);

	if (header->head == tail)
		return -EAGAIN;

	for (i = 0; header->head != tail && i < maxevents; header->head++) {
		if (read_event__user(ring, header->head, &events[i]))
			i++;
		else
			/* Currently we do not support event cleared by kernel */
			assert(0);
	}

	return i;
}

static void uepoll_signal(struct ueventfd *efd, int events)
{
	struct ring *ring;

	assert(efd->epfd);

	if (!(efd->event.events & events))
		return;

	ring = &efd->epfd->ring;

	if (!update_event__kernel(ring, efd->efd_bit, events))
		add_event__kernel(ring, efd->efd_bit);
}

static void ueventfd_create(struct ueventfd *efd, unsigned long long count)
{
	memset(efd, 0, sizeof(*efd));
	efd->count = count;
}

static int ueventfd_read(struct ueventfd *efd, unsigned long long *count)
{
	unsigned long long c, old;

	c = efd->count;
#if 1
	old = c;
	if (c == 0)
		return -EAGAIN;
	__atomic_sub_fetch(&efd->count, c, __ATOMIC_RELAXED);
#else
	do {
		if (c == 0)
			return -EAGAIN;
		old = c;
	} while ((c = __sync_val_compare_and_swap(&efd->count, old, 0)) != old);
#endif
	*count = old;

	uepoll_signal(efd, EPOLLOUT);

	return 8;
}

static int ueventfd_write(struct ueventfd *efd, unsigned long long count)
{
	unsigned long long c, old, new;

#if 1
	(void)c; (void)old; (void)new;

	if (count == ULLONG_MAX)
		return -EINVAL;

	__atomic_fetch_add(&efd->count, count, __ATOMIC_RELAXED);

#else
	c = efd->count;
	do {
		if (ULLONG_MAX - c <= count)
			return -EAGAIN;
		old = c;
		new = old + count;
	} while ((c = __sync_val_compare_and_swap(&efd->count, old, new)) != old);
#endif

	uepoll_signal(efd, EPOLLIN);

	return 8;
}

static void *thread_work(void *arg)
{
	struct thread_ctx *ctx = arg;
	uint64_t ucnt = 1;
	int i, rc;

	__atomic_fetch_add(&thr_ready, 1, __ATOMIC_RELAXED);

	while (!start)
		;

	for (i = 0; i < ITERS; i++) {
		rc = ueventfd_write(&ctx->efd, ucnt);
		assert(rc == sizeof(ucnt));
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	struct epoll_event ev, events[THRDS];
	struct thread_ctx *ctx;
	struct uepollfd epfd;
	int i, rc, nfds;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0;

	uepoll_create(&epfd);

	for (i = 0; i < THRDS; i++) {
		ctx = &threads[i];

		ueventfd_create(&ctx->efd, 0);

		ev.events = EPOLLIN;
		ev.data.ptr = ctx;
		uepoll_ctl(&epfd, EPOLL_CTL_ADD, &ctx->efd, &ev);

		rc = pthread_create(&ctx->thread, NULL, thread_work, ctx);
		assert(rc == 0);
	}

	while (thr_ready == THRDS)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = uepoll_wait(&epfd, events, THRDS);
		if (nfds == -EAGAIN)
			/* Busy wait */
			continue;
		assert(nfds > 0);

		epoll_calls++;

		for (i = 0; i < nfds; ++i) {
			ctx = events[i].data.ptr;
			rc = ueventfd_read(&ctx->efd, &ucnt);
			if (rc == -EAGAIN)
				continue;
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