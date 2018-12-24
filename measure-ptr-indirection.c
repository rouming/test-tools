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
 *  $ gcc -o measure-ptr-indirection measure-ptr-indirection.c -O2 -Wall -lpthread
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <syscall.h>
#include <time.h>
#include <string.h>

#define ITERS  1000000000ul
#define ARR_SZ 128

#define unlikely(x)  __builtin_expect((x),0)
#define likely(x)    __builtin_expect((x),1)


static unsigned __thread thr_idx;
static unsigned counter;
static unsigned *thr_level1_arr;
static unsigned *thr_level2_arr[ARR_SZ];

static inline unsigned long long nsecs(void)
{
    struct timespec ts = {0, 0};

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((unsigned long long)ts.tv_sec * 1000000000ull) + ts.tv_nsec;
}

static inline void lin_atomic_inc(int *v)
{
	asm volatile("\n\tlock; incl %0"
		     : "+m" (v));
}

__attribute__((unused))
static inline void clflush(volatile void *p)
{
	asm volatile ("clflush (%0)" :: "r"(p));
}

__attribute__((unused))
static inline void clflushopt(volatile void *p)
{
	asm volatile ("clflushopt (%0)" :: "r"(p));
}

__attribute__ ((noinline))
static void ptr_level1_access(int idx)
{
	thr_level1_arr[idx]++;
}

__attribute__ ((noinline))
static void ptr_level2_access(int idx)
{
	unsigned *ptr;

	ptr = thr_level2_arr[idx];
	if (unlikely(!ptr))
		assert(0);
	ptr[idx]++;
#ifdef MEASURE_CFLUSH
	clflush(ptr);
#endif
}

#define mb()  asm volatile("mfence":::"memory")
#define rmb() asm volatile("lfence":::"memory")
#define wmb() asm volatile("sfence" ::: "memory")

static volatile int stop;

static void *update_thr_level2_arr(void *arg)
{
	unsigned *arr, sum;
	unsigned long calls = 0;

	arr = calloc(ARR_SZ, sizeof(*arr));
	assert(arr);

	while (!stop) {
		unsigned *tmp = thr_level2_arr[0];
		thr_level2_arr[0] = arr;
		arr = tmp;
		/* Reader should see the change */
		wmb();
		calls++;
	}

	sum = *thr_level2_arr[0] + *arr;
	assert(sum == ITERS);
	printf(">>> changes %.2f per iter, orig = %d, temp = %d\n",
		   (double)calls/ITERS, *thr_level2_arr[0], *arr);

	return NULL;
}

static void do_measure(void)
{
	unsigned long long ns, i, clflush_ns = 0;

	thr_idx = __sync_fetch_and_add(&counter, 1);

#if MEASURE_CFLUSH
#if 0
	ns = nsecs();
	for (i = 0; i < ITERS; i++)
		clflushopt(thr_level1_arr);
	ns = nsecs() - ns;
	printf(">> cflushopt: %.3f ms, %.3f ns per access\n",
		   ns/1000.0/1000.0, (double)ns/ITERS);

#endif

	ns = nsecs();
	for (i = 0; i < ITERS; i++)
		clflush(thr_level2_arr);
	clflush_ns = nsecs() - ns;
	printf(">> cflush: %.3f ms, %.3f ns per access\n",
		   clflush_ns/1000.0/1000.0, (double)clflush_ns/ITERS);
#endif

	ns = nsecs();
	for (i = 0; i < ITERS; i++)
		ptr_level1_access(thr_idx);
	ns = nsecs() - ns;
	printf(">> level1: %.3f ms, %.3f ns per access\n",
		   ns/1000.0/1000.0, (double)ns/ITERS);

	ns = nsecs();
	for (i = 0; i < ITERS; i++)
		ptr_level2_access(thr_idx);
	ns = nsecs() - ns - clflush_ns;
	printf(">> level2: %.3f ms, %.3f ns per access\n",
		   ns/1000.0/1000.0, (double)ns/ITERS);

}

int main()
{
	pthread_t thr1;
	int i, rc;

	thr_level1_arr = calloc(ARR_SZ, sizeof(*thr_level1_arr));
	assert(thr_level1_arr);

	for (i = 0; i < ARR_SZ; i++) {
		thr_level2_arr[i] = calloc(ARR_SZ, sizeof(*thr_level2_arr[0]));
		assert(thr_level2_arr[i]);
	}
	rc = pthread_create(&thr1, NULL, update_thr_level2_arr, NULL);
	assert(rc == 0);

	do_measure();

	stop = 1;
	pthread_join(thr1, NULL);

	return 0;
}

/**
 * get_bit() - atomically sets first found 0-bit and returns it
 *             if no bits are found - number of bits is returned
 */

static unsigned get_bit(unsigned long *bitmask, size_t bits)
{
	unsigned bit, i;
	int b;

	for (bit = 0, i = 0; bit < bits;
	     bit += sizeof(*bitmask) * 8, i++) {
		typeof(*bitmask) bmask;
repeat:
		bmask = bitmask[i];
		b = __builtin_ffsl(~bmask) - 1;
		if (b < 0)
			continue;
		if (bit + b >= bits)
			break;

		if (bmask != __sync_val_compare_and_swap(&bitmask[i], bmask,
							 bmask | (1<<b)))
			/* Lost a race */
			goto repeat;

		/* Got a bit */
		return bit + b;
	}

	/* Nothing found */
	return bits;
}

static void put_bit(unsigned long *bitmask, unsigned bit)
{
	typeof(*bitmask) bmask;
	unsigned i, b;

	i = bit / (sizeof(*bitmask) * 8);
	b = bit % (sizeof(*bitmask) * 8);

	do {
		bmask = bitmask[i];
	} while (bmask != __sync_val_compare_and_swap(&bitmask[i], bmask,
						      bmask & ~(1<<b)));
}

int bitmap_main()
{
	unsigned long bitmask[2] = {0};
	unsigned bit1, bit2, bit3, bits = sizeof(bitmask) * 8;

	bitmask[0] = ~0;
	bitmask[1] =  0;

	bit1 = get_bit(bitmask, bits);
	printf("bit=%d, bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bit1, bitmask[0], bitmask[1], bits);
	bit2 = get_bit(bitmask, bits);
	printf("bit=%d, bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bit2, bitmask[0], bitmask[1], bits);
	bit3 = get_bit(bitmask, bits);
	printf("bit=%d, bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bit3, bitmask[0], bitmask[1], bits);

	put_bit(bitmask, bit2);
	printf("bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bitmask[0], bitmask[1], bits);
	put_bit(bitmask, bit1);
	printf("bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bitmask[0], bitmask[1], bits);
	put_bit(bitmask, bit3);
	printf("bmask[0]=%lx, bmask[1]=%lx, bits=%d\n",
	       bitmask[0], bitmask[1], bits);

	
	return 0;
}
