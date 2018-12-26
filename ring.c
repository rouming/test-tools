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

#define BUILD_BUG_ON(condition) ((void )sizeof(char [1 - 2*!!(condition)]))
#define READ_ONCE(v) (*(volatile typeof(v)*)&(v))

#define smp_rmb() asm volatile("lfence":::"memory")
#define smp_wmb() asm volatile("sfence":::"memory")

#define EPOLL_USER_HEADER_SIZE  128
#define EPOLL_USER_HEADER_MAGIC 0xf00dce11


enum {
	UNSIGNALED = 0,
	SIGNALED   = 1,
	READING    = 2,
	WRITING    = 3,

	ALL        = (SIGNALED | READING | WRITING)
};

enum {
	EPOLL_USER_POLL_INACTIVE = 0, /* busy poll is inactive, call epoll_wait() */
	EPOLL_USER_POLL_ACTIVE   = 1, /* user can continue busy polling */
};

#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wpadded"

struct user_epitem {
	unsigned active_events;
	struct epoll_event event;
};

struct user_header {
	unsigned magic;          /* epoll user header magic */
	unsigned state;          /* epoll ring state */
	unsigned header_length;  /* length of the header + items */
	unsigned index_length;   /* length of the index ring */
	unsigned items_nr;       /* number of ep items */
	unsigned indeces_nr;     /* number of items indeces */
	unsigned head;           /* updated by userland */
	unsigned tail;           /* updated by kernel */
	unsigned padding[24];    /* Header size is 128 bytes */

	struct user_epitem items[];
};

#pragma GCC diagnostic pop

struct ring {
	struct user_header *user_header;
	unsigned           *user_itemsindex;

	unsigned items_nr;
	unsigned indeces_nr;
};

static inline bool update_event__kernel(struct ring *ring, unsigned bit,
					int events)
{
	struct user_epitem *item;

	item = &ring->user_header->items[bit];

	assert(sizeof(struct user_epitem) == 16);
	assert(!((uintptr_t)item & 15));
	assert(!((uintptr_t)&item->active_events & 15));

	return !!__atomic_fetch_or(&item->active_events, events, __ATOMIC_ACQUIRE);
}

static inline void add_event__kernel(struct ring *ring, unsigned bit)
{
	unsigned i, *item_idx;

	i = __atomic_fetch_add(&ring->user_header->tail, 1, __ATOMIC_ACQUIRE);
	item_idx = &ring->user_itemsindex[i % ring->indeces_nr];

	/* Update data */
	*item_idx = bit + 1;
}

/**
 * free_event__kernel() - frees event from kernel side.  Returns true if event
 * is not signaled and has been read already by userspace, thus it is safe to
 * reuse item bit immediately.  It is not safe to reuse item bit if false is
 * returned, bit put operation should be postponed till userspace calls epoll
 * syscall.
 */
static inline bool free_event__kernel(struct ring *ring, unsigned bit)
{
	struct user_epitem *item;

	item = &ring->user_header->items[bit];

	return !__atomic_exchange_n(&item->active_events, 0, __ATOMIC_RELAXED);
}


static inline bool read_event__user(struct ring *ring, unsigned idx,
				    struct epoll_event *event)
{
	struct user_header *header = ring->user_header;
	struct user_epitem *item;
	unsigned *item_idx_ptr;

	item_idx_ptr = &ring->user_itemsindex[idx % header->indeces_nr];

	/*
	 * Spin here till we see valid index
	 */
	while (!(idx = __atomic_load_n(item_idx_ptr, __ATOMIC_ACQUIRE)))
		;

	if (idx > header->items_nr)
		/* Corrupted index? */
		return false;

	item = &header->items[idx - 1];

	/*
	 * Mark index as invalid, that is for userspace only, kernel does not care
	 * and will refill this pointer only when observes that event is cleared,
	 * which happens below.
	 */
	*item_idx_ptr = 0;

	/*
	 * Fetch data first, if event is cleared by the kernel we drop the data
	 * returning false.
	 */
	event->data = item->event.data;
	event->events = __atomic_exchange_n(&item->active_events, 0,
					    __ATOMIC_RELEASE);

	return !!event->events;
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

	BUILD_BUG_ON(sizeof(*epfd->ring.user_header) != EPOLL_USER_HEADER_SIZE);

	epfd->ring.user_header = aligned_alloc(EPOLL_USER_HEADER_SIZE, size);
	assert(epfd->ring.user_header);
	memset(epfd->ring.user_header, 0, size);

	epfd->ring.user_header->magic = EPOLL_USER_HEADER_MAGIC;
	epfd->ring.user_header->state = EPOLL_USER_POLL_ACTIVE;

	epfd->ring.user_itemsindex = aligned_alloc(EPOLL_USER_HEADER_SIZE, size);
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
	efd->efd_bit = epfd->ring.items_nr++;
	efd->event = *event;
	/* Currencly indeces_nr == items_nr */
	epfd->ring.indeces_nr = epfd->ring.items_nr;

	ring->user_header->items_nr = epfd->ring.items_nr;
	ring->user_header->indeces_nr = epfd->ring.indeces_nr;

	/* That is racy with possible access from userspace, but we do not care */
	item = &ring->user_header->items[efd->efd_bit];
	item->active_events = 0;
	item->event = *event;
}

static int uepoll_wait(struct uepollfd *epfd, struct epoll_event *events,
		       int maxevents)

{
	struct ring *ring = &epfd->ring;
	struct user_header *header = ring->user_header;
	unsigned tail;
	int i;

	BUILD_BUG_ON(sizeof(*header) != EPOLL_USER_HEADER_SIZE);
	assert(maxevents > 0);

	/*
	 * Cache the tail because we don't want refetch it on each iteration
	 * and then catch live events updates, i.e. we don't want user @events
	 * array consist of events from the same fds.
	 */
	tail = READ_ONCE(header->tail);

	if (header->head == tail) {
		if (header->state != EPOLL_USER_POLL_ACTIVE)
			return -ECANCELED;
		return -EAGAIN;
	}

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
	unsigned long long c;

	c = efd->count;
#if 1
	if (c == 0)
		return -EAGAIN;
	__atomic_sub_fetch(&efd->count, c, __ATOMIC_RELAXED);
#else
	do {
		if (c == 0)
			return -EAGAIN;
	} while (!__atomic_compare_exchange_n(&efd->count, &c, 0, false,
					      __ATOMIC_RELAXED, __ATOMIC_RELAXED));
#endif
	*count = c;

	uepoll_signal(efd, EPOLLOUT);

	return 8;
}

static int ueventfd_write(struct ueventfd *efd, unsigned long long count)
{
#if 1
	if (count == ULLONG_MAX)
		return -EINVAL;

	__atomic_fetch_add(&efd->count, count, __ATOMIC_RELAXED);

#else
	unsigned long long c = efd->count;
	do {
		if (ULLONG_MAX - c <= count)
			return -EAGAIN;
	} while (!__atomic_compare_exchange_n(&efd->count, &c, c + count, false,
					      __ATOMIC_RELAXED, __ATOMIC_RELAXED));
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
