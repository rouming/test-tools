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
#include <err.h>
#include <numa.h>

#ifndef bool
#define bool _Bool
enum {
	false = 0,
	true  = 1
};
#endif

#define BUILD_BUG_ON(condition) ((void )sizeof(char [1 - 2*!!(condition)]))
#define READ_ONCE(v) (*(volatile typeof(v)*)&(v))

#define EPOLL_USER_HEADER_SIZE  128
#define EPOLL_USER_HEADER_MAGIC 0xeb01eb01


enum {
	EPOLL_USER_POLL_INACTIVE = 0, /* user poll disactivated */
	EPOLL_USER_POLL_ACTIVE   = 1, /* user can continue busy polling */
};

#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wpadded"

struct user_epitem {
	unsigned int ready_events;
	struct epoll_event event;
};

struct user_header {
	unsigned int magic;          /* epoll user header magic */
	unsigned int state;          /* epoll ring state */
	unsigned int header_length;  /* length of the header + items */
	unsigned int index_length;   /* length of the index ring */
	unsigned int max_items_nr;   /* max num of items slots */
	unsigned int max_index_nr;   /* max num of items indeces, always pow2 */
	unsigned int head;           /* updated by userland */
	unsigned int tail;           /* updated by kernel */
	unsigned int padding[24];    /* Header size is 128 bytes */

	struct user_epitem items[];
};

#pragma GCC diagnostic pop

struct ring {
	struct user_header *user_header;
	unsigned           *user_itemsindex;

	unsigned items_nr;
	unsigned max_index_nr;
};

static inline bool update_event__kernel(struct ring *ring, unsigned bit,
					int events)
{
	struct user_epitem *item;

	item = &ring->user_header->items[bit];

	assert(sizeof(struct user_epitem) == 16);
	assert(!((uintptr_t)item & 15));
	assert(!((uintptr_t)&item->ready_events & 15));

	return !!__atomic_fetch_or(&item->ready_events, events, __ATOMIC_ACQUIRE);
}

static inline void add_event__kernel(struct ring *ring, unsigned bit)
{
	unsigned i, *item_idx, indeces_mask;

	indeces_mask = (ring->max_index_nr - 1);
	i = __atomic_fetch_add(&ring->user_header->tail, 1, __ATOMIC_ACQUIRE);
	item_idx = &ring->user_itemsindex[i & indeces_mask];

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

	return !__atomic_exchange_n(&item->ready_events, 0, __ATOMIC_RELAXED);
}


static inline bool read_event__user(struct ring *ring, unsigned idx,
				    struct epoll_event *event)
{
	struct user_header *header = ring->user_header;
	struct user_epitem *item;
	unsigned *item_idx_ptr;
	unsigned indeces_mask;

	indeces_mask = (header->max_index_nr - 1);
	if (indeces_mask & header->max_index_nr)
		/* Should be pow2, corrupted header? */
		return false;

	item_idx_ptr = &ring->user_itemsindex[idx & indeces_mask];

	/*
	 * Spin here till we see valid index
	 */
	while (!(idx = __atomic_load_n(item_idx_ptr, __ATOMIC_ACQUIRE)))
		;

	if (idx > header->max_items_nr)
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
	event->events = __atomic_exchange_n(&item->ready_events, 0,
					    __ATOMIC_RELEASE);

	return !!event->events;
}

#define ITERS 10000000ull

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

struct cpu_map {
	unsigned int nr;
	unsigned int map[];
};

static volatile int thr_ready;
static volatile int start;

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

static void uepoll_create(struct uepollfd *epfd)
{
	struct user_header *hdr;
	unsigned *itemsindex;
	const size_t size = 4<<12;

	memset(epfd, 0, sizeof(*epfd));

	BUILD_BUG_ON(sizeof(*hdr) != EPOLL_USER_HEADER_SIZE);

	hdr = aligned_alloc(EPOLL_USER_HEADER_SIZE, size);
	assert(hdr);
	memset(hdr, 0, size);

	hdr->magic = EPOLL_USER_HEADER_MAGIC;
	hdr->state = EPOLL_USER_POLL_ACTIVE;
	hdr->header_length = size;
	hdr->index_length = size;
	hdr->max_items_nr = (size - sizeof(*hdr)) / sizeof(hdr->items[0]);
	hdr->max_index_nr = size / sizeof(*itemsindex);

	itemsindex = aligned_alloc(EPOLL_USER_HEADER_SIZE, size);
	assert(itemsindex);
	memset(itemsindex, 0, size);

	epfd->ring.user_header = hdr;
	epfd->ring.user_itemsindex = itemsindex;
	epfd->ring.max_index_nr = hdr->max_index_nr;
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

	/* That is racy with possible access from userspace, but we do not care */
	item = &ring->user_header->items[efd->efd_bit];
	item->ready_events = 0;
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

static int do_bench(struct cpu_map *cpu, unsigned int nthreads)
{

	struct epoll_event ev, events[nthreads];
	struct thread_ctx threads[nthreads];
	struct thread_ctx *ctx;
	struct uepollfd epfd;
	int i, rc, nfds;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0;

	thr_ready = 0;
	start = 0;

	uepoll_create(&epfd);

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];

		ueventfd_create(&ctx->efd, 0);

		ev.events = EPOLLIN;
		ev.data.ptr = ctx;
		uepoll_ctl(&epfd, EPOLL_CTL_ADD, &ctx->efd, &ev);

		rc = pthread_create(&ctx->thread, NULL, thread_work, ctx);
		assert(rc == 0);
	}

	while (thr_ready != nthreads)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = uepoll_wait(&epfd, events, nthreads);
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
	unsigned int i, nthreads_arr[] = {8, 16, 32, 64, 128, 256};
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
