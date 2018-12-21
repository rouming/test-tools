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
 *  $ gcc -o ring ring.c -O2 -lpthread
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

#define L1_CACHE_BYTES 128
#define __cacheline_aligned __attribute__((__aligned__(L1_CACHE_BYTES)))

enum {
	UNSIGNALED = 0,
	SIGNALED   = 1,
	READING    = 2,
	WRITING    = 3,

	ALL        = (SIGNALED | READING | WRITING)
};

struct user_header {
	unsigned magic;
	unsigned header_length; /* length of the header */
	unsigned bitmap_length; /* length of the bitmap */
	unsigned ring_length;   /* length of the events ring */
	unsigned nr;            /* number of ep items */
	unsigned head;          /* updated by userland */
	unsigned tail;          /* updated by kernel */
	unsigned padding;

	unsigned long long bitmap[] __cacheline_aligned;
};

struct user_event {
	unsigned bit;
	struct epoll_event event;
};

struct ring {
	struct user_header *user_header;
	struct user_event  *user_events;

	unsigned nr;
	unsigned cntr;
	unsigned commit_cntr;
};

#define BITMASK_BITS 64
#define BITMASK_SHIFT (64 > BITMASK_BITS ?				\
		       (6 - (__builtin_ffs(BITMASK_BITS) - 1)) :	\
		       ((__builtin_ffs(BITMASK_BITS) - 1) - 6))

static inline unsigned long long idx_to_bitmask(unsigned idx, unsigned state)
{
#if BITMASK_BITS < 32
	return (unsigned long long)state <<
		(((idx & ((1ull << BITMASK_BITS)-1)) * (64 > BITMASK_BITS ? BITMASK_BITS : 0)));
#else
	return (unsigned long long)state << (idx * (64 > BITMASK_BITS ? BITMASK_BITS : 0));
#endif
}

static inline unsigned long long *bitmap_word(unsigned long long *bitmap,
					      unsigned bit)
{
#if BITMASK_BITS < 64
	return &bitmap[bit >> BITMASK_SHIFT];
#else
	return &bitmap[bit << BITMASK_SHIFT];
#endif
}

static inline struct user_event *add_event__kernel(struct ring *ring,
						   unsigned bit,
						   struct epoll_event *event)
{
	struct user_event *uev;

	unsigned long long *pbm, mask, bm, old, new;
	unsigned i;

	i = __atomic_add_fetch(&ring->cntr, 1, __ATOMIC_RELAXED) - 1;
	uev = &ring->user_events[i % ring->nr];

	/* Update data */
	uev->bit = bit;
	uev->event = *event;

	pbm = bitmap_word(ring->user_header->bitmap, bit);
	mask = idx_to_bitmask(bit, ALL);

	/*
	 * Switch from any to SIGNALED, if fail - another bit update in progress
	 */
	bm = *pbm;
	do {
		old = bm;
		new = (old & ~mask) | idx_to_bitmask(bit, SIGNALED);

	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);

	/*
	 * Wait for other commits in front of us and then commit our event.
	 * We can't spin on ->tail directly, because this chunk of memory is
	 * not controlled by the kernel, thus userspace can hang kernel by
	 * writing garbage to ->tail.
	 */
	while (__sync_val_compare_and_swap(&ring->commit_cntr, i, i + 1) != i)
		;

	/* Now it is safe to propagate event to userspace, order does not matter */
	__atomic_add_fetch(&ring->user_header->tail, 1, __ATOMIC_RELAXED);

	return uev;
}

static inline bool read_event__user(struct ring *ring, unsigned idx,
				    struct epoll_event *event)
{
	unsigned long long *pbm, mask, bm, old, new;
	struct user_event *uev;
	unsigned bit;

	uev = &ring->user_events[idx % ring->nr];
	bit = uev->bit;

	//XXX proper bit overflow check is required
	assert(bit < (4096 - sizeof(struct user_header)) * 8);

	pbm = bitmap_word(ring->user_header->bitmap, bit);
	mask = idx_to_bitmask(bit, ALL);

	bm = *pbm;
	do {
		if (!(bm & idx_to_bitmask(bit, SIGNALED)))
			/* Freed? */
			return false;

		old = (bm & ~mask) | idx_to_bitmask(bit, SIGNALED);
		new = (bm & ~mask) | idx_to_bitmask(bit, READING);

		/* Switch from SIGNALED to READING, if fail - writer in progress */
		if ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old)
			continue;

		*event = uev->event;

		/* Switch from READING to UNSIGNALED, if fail - writer in progress */
		do {
			old = (bm & ~mask) | idx_to_bitmask(bit, READING);
			new = (bm & ~mask) | idx_to_bitmask(bit, UNSIGNALED);
		} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old &&
			 (bm & mask) == idx_to_bitmask(bit, READING));

	} while (bm != old);

	return true;
}

static inline bool update_event__kernel(struct ring *ring, unsigned bit,
					struct user_event *uev, int events)
{
	unsigned long long *pbm, mask, bm, old, new;

	pbm = bitmap_word(ring->user_header->bitmap, bit);
	mask = idx_to_bitmask(bit, ALL);

	/*
	 * Switch from any to WRITING, if fail - another bit update in progress
	 */
	bm = *pbm;
	do {
		if ((bm & mask) == idx_to_bitmask(bit, UNSIGNALED))
			/* Unsignaled, freed by user */
			return false;
		old = bm;
		new = (old & ~mask) | idx_to_bitmask(bit, WRITING);

	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);

	/* Here we are sure userland waits for us to finish events update */
	uev->event.events |= events;

	/*
	 * Switch from any to SIGNALED, if fail - another bit update in progress,
	 * reader should not come in-between.
	 */
	bm = *pbm;
	do {
		old = bm;
		new = (old & ~mask) | idx_to_bitmask(bit, SIGNALED);

	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);

	return true;
}

static inline void free_event__kernel(struct ring *ring, unsigned idx)
{
	unsigned long long *pbm, mask, bm, old, new;

	pbm = bitmap_word(ring->user_header->bitmap, idx);
	mask = idx_to_bitmask(idx, ALL);

	/*
	 * Switch from any to UNSIGNALED, if fail - another bit update in progress
	 */
	bm = *pbm;
	do {
		if ((bm & mask) == idx_to_bitmask(idx, UNSIGNALED))
			/* Unsignaled, freed by user */
			return;
		old = bm;
		new = (old & ~mask) | idx_to_bitmask(idx, UNSIGNALED);

	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);
}


#define ITERS 10000000ull
//#define THRDS 16
#define THRDS 256


struct uepollfd {
	struct ring ring;
};

struct ueventfd {
	unsigned long long count;
	struct uepollfd    *epfd;
	unsigned           efd_bit;
	struct epoll_event event;
	struct user_event  *uev;
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
	memset(epfd, 0, sizeof(*epfd));

	epfd->ring.user_header = aligned_alloc(L1_CACHE_BYTES, 4096);
	assert(epfd->ring.user_header);
	memset(epfd->ring.user_header, 0, 4096);

	epfd->ring.user_events = aligned_alloc(L1_CACHE_BYTES, 4096);
	assert(epfd->ring.user_events);
	memset(epfd->ring.user_events, 0, 4096);
}

static void uepoll_ctl(struct uepollfd *epfd, int op, struct ueventfd *efd,
		       struct epoll_event *event)

{
	struct ring *ring = &epfd->ring;

	assert(op == EPOLL_CTL_ADD);
	/* No pending events so far */
	assert(efd->count == 0);
	assert(efd->epfd == NULL);

	efd->epfd = epfd;
	efd->efd_bit = epfd->ring.nr++;
	efd->event = *event;

	ring->user_header->nr = epfd->ring.nr;
}

static int uepoll_wait(struct uepollfd *epfd, struct epoll_event *events,
		       int maxevents)

{
	struct ring *ring = &epfd->ring;
	struct user_header *uheader = ring->user_header;
	unsigned tail;
	int i;

	assert(maxevents > 0);

	/*
	 * Cache the tail because we don't want refetch it on each iteration
	 * and then catch live events updates, i.e. we don't want events array
	 * consist of events from same fds.
	 */
	tail = READ_ONCE(uheader->tail);

	if (uheader->head >= tail)
		return -EAGAIN;

	for (i = 0; uheader->head < tail && i < maxevents; uheader->head++) {
		if (read_event__user(ring, uheader->head, &events[i]))
			i++;
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

	if (efd->uev) {
		if (update_event__kernel(ring, efd->efd_bit, efd->uev, events))
			/* User event was successfully updated, keep the uev pointer */
			return;
	}
	efd->event.events |= events;
	efd->uev = add_event__kernel(ring, efd->efd_bit, &efd->event);
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

	__atomic_add_fetch(&efd->count, count, __ATOMIC_RELAXED);

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

	__atomic_add_fetch(&thr_ready, 1, __ATOMIC_RELAXED);

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


//////////////

#if 0

static unsigned long long idx_to_bitmask(unsigned idx, unsigned state)
{
	return (unsigned long long)state << ((idx & 0x3f) << 2);
}

enum {
	UNSIGNALED = 0,
	SIGNALED   = 1,
	READING    = 2,
	WRITING    = 4,

	ALL        = (SIGNALED | READING | WRITING)
};

static inline bool read_event(struct ring *ring, unsigned idx,
			      struct event *event)
{
	unsigned long long *pbm, mask, bm, old, new;

	/* Quarter of 64, since we have 4 bits for the state */
	pbm = bitmap_word(ring->bitmap, idx);
	mask = idx_to_bitmask(idx, ALL);

	bm = *pbm;
	do {
		if (!(bm & mask))
			/* Freed? */
			return false;

		/* Switch to READING. */
		set_bit(pbm, XXX);
		smp_mb();

		bm = *pbm;
		if (bm & idx_to_bitmask(idx, WRITING))
			/* Busy loop, wait for writer go away */
			continue;

		*event = ring->events[idx];

		old = (bm & ~mask) | idx_to_bitmask(idx, READING | SIGNALED);
		new = (bm & ~mask) | idx_to_bitmask(idx, UNSIGNALED);

		/* Switch from READING to UNSIGNALED, if fail - writer in progress */
	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);

	return true;
}

static inline void update_event(struct ring *ring, unsigned idx,
				struct event *event)
{
	unsigned long long *pbm, mask, old, new;

	pbm = bitmap_word(ring->bitmap, idx);

	/*
	 * Switch from any to WRITING, if fail - another bit update in progress
	 */
	bm = *pbm;
	do {
		if (!(bm & idx_to_bitmask(idx, SIGNALED)))
			/* Already freed */
			return false;
		old = bm;
		new = (bm & ~mask) | idx_to_bitmask(idx, WRITING | SIGNALED);
	} while ((bm = __sync_val_compare_and_swap(pbm, old, new)) != old);

	/* Clear possible reader, order with WRITING bit set above is crucial  */
	clear_bit(pbm, XXX);

	ring->events[idx] = *event;

	smp_wmb();

	/* Clear previously set WRITING. */
	clear_bit(pbm, XXX);
}
#endif
