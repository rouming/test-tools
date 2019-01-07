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
 *  $ gcc -o userpolled-epoll userpolled-epoll.c -O2 -lpthread -lnuma
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>
#include <numa.h>

typedef _Bool bool;
enum {
	false = 0,
	true  = 1,
};

#define BUILD_BUG_ON(condition) ((void )sizeof(char [1 - 2*!!(condition)]))
#define READ_ONCE(v) (*(volatile typeof(v)*)&(v))

#define ITERS     1000000ull

#define EPOLL_USERPOLL_HEADER_MAGIC 0xeb01eb01
#define EPOLL_USERPOLL_HEADER_SIZE  128
#define EPOLL_USERPOLL 1

/* User item marked as removed for EPOLL_USERPOLL */
#define EPOLLREMOVED	(1U << 27)


#ifndef __NR_epoll_create2
#define __NR_epoll_create2  428
#endif

static inline long epoll_create2(int flags, size_t size)
{
	return syscall(__NR_epoll_create2, flags, size);
}

/*
 * Item, shared with userspace.  Unfortunately we can't embed epoll_event
 * structure, because it is badly aligned on all 64-bit archs, except
 * x86-64 (see EPOLL_PACKED).  sizeof(epoll_uitem) == 16
 */
struct epoll_uitem {
	uint32_t ready_events;
	uint32_t events;
	uint64_t data;
};

/*
 * Header, shared with userspace. sizeof(epoll_uheader) == 128
 */
struct epoll_uheader {
	uint32_t magic;          /* epoll user header magic */
	uint32_t header_length;  /* length of the header + items */
	uint32_t index_length;   /* length of the index ring, always pow2 */
	uint32_t max_items_nr;   /* max number of items */
	uint32_t head;           /* updated by userland */
	uint32_t tail;           /* updated by kernel */

	struct epoll_uitem items[]
		__attribute__((aligned(EPOLL_USERPOLL_HEADER_SIZE)));
};

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

static inline unsigned int max_index_nr(struct epoll_uheader *header)
{
	return header->index_length >> 2;
}

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

static inline bool read_event(struct epoll_uheader *header, unsigned int *index,
			      unsigned int idx, struct epoll_event *event)
{
	struct epoll_uitem *item;
	unsigned int *item_idx_ptr;
	unsigned int indeces_mask;

	indeces_mask = max_index_nr(header) - 1;
	if (indeces_mask & max_index_nr(header)) {
		assert(0);
		/* Should be pow2, corrupted header? */
		return false;
	}

	item_idx_ptr = &index[idx & indeces_mask];

	/*
	 * Spin here till we see valid index
	 */
	while (!(idx = __atomic_load_n(item_idx_ptr, __ATOMIC_ACQUIRE)))
		;

	if (idx > header->max_items_nr) {
		assert(0);
		/* Corrupted index? */
		return false;
	}

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
	event->data.u64 = item->data;
	event->events = __atomic_exchange_n(&item->ready_events, 0,
					    __ATOMIC_RELEASE);

	return (event->events & ~EPOLLREMOVED);
}

static int uepoll_wait(struct epoll_uheader *header, unsigned int *index,
		       int epfd, struct epoll_event *events, int maxevents)

{
	/*
	 * Before entering kernel we do busy wait for ~1ms, naively assuming
	 * each iteration costs 1 cycle, 1 ns.
	 */
	unsigned int spins = 1000000;
	unsigned int tail;
	int i;

	assert(maxevents > 0);

again:
	/*
	 * Cache the tail because we don't want refetch it on each iteration
	 * and then catch live events updates, i.e. we don't want user @events
	 * array consist of events from the same fds.
	 */
	tail = READ_ONCE(header->tail);

	if (header->head == tail) {
		if (spins--)
			/* Busy loop a bit */
			goto again;

		i = epoll_wait(epfd, NULL, 0, -1);
		assert(i < 0);
		if (errno != ESTALE)
			return i;

		tail = READ_ONCE(header->tail);
		assert(header->head != tail);
	}

	for (i = 0; header->head != tail && i < maxevents; header->head++) {
		if (read_event(header, index, header->head, &events[i]))
			i++;
		else
			/* Event can't be removed under us */
			assert(0);
	}

	return i;
}

static void uepoll_mmap(int epfd, struct epoll_uheader **_header,
		       unsigned int **_index)
{
	struct epoll_uheader *header;
	unsigned int *index, len;

	BUILD_BUG_ON(sizeof(*header) != EPOLL_USERPOLL_HEADER_SIZE);
	BUILD_BUG_ON(sizeof(header->items[0]) != 16);

	len = sysconf(_SC_PAGESIZE);
again:
	header = mmap(NULL, len, PROT_WRITE|PROT_READ, MAP_SHARED, epfd, 0);
	if (header == MAP_FAILED)
		err(EXIT_FAILURE, "mmap(header)");

	if (header->header_length != len) {
		unsigned int tmp_len = len;

		len = header->header_length;
		munmap(header, tmp_len);
		goto again;
	}
	assert(header->magic == EPOLL_USERPOLL_HEADER_MAGIC);

	index = mmap(NULL, header->index_length, PROT_WRITE|PROT_READ, MAP_SHARED,
		     epfd, header->header_length);
	if (index == MAP_FAILED)
		err(EXIT_FAILURE, "mmap(index)");

	*_header = header;
	*_index = index;
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

	struct epoll_uheader *header;
	unsigned int *index;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0, eagains = 0;

	thr_ready = 0;
	start = 0;

	epfd = epoll_create2(EPOLL_USERPOLL, nthreads);
	if (epfd < 0)
		err(EXIT_FAILURE, "epoll_create2");

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];

		ctx->efd = eventfd(0, EFD_NONBLOCK);
		if (ctx->efd < 0)
			err(EXIT_FAILURE, "eventfd");

		ev.events = EPOLLIN | EPOLLET;
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

	/* Mmap all pointers */
	uepoll_mmap(epfd, &header, &index);

	while (thr_ready != nthreads)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = uepoll_wait(header, index, epfd, events, nthreads);
		if (nfds < 0)
			err(EXIT_FAILURE, "epoll_wait");

		epoll_calls++;

		for (i = 0; i < (unsigned int)nfds; ++i) {
			ctx = events[i].data.ptr;
			rc = read(ctx->efd, &ucnt, sizeof(ucnt));
			if (rc < 0) {
				assert(errno == EAGAIN);
				continue;
			}
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
	close(epfd);

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
