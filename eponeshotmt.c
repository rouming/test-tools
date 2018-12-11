/*
 * multi-threaded EPOLLONESHOT test
 * Copyright (C) 2013  Eric Wong <normalperson@yhbt.net>
 * License: GPLv2 or later <http://www.gnu.org/licenses/gpl-2.0.txt>
 * gcc -o eponeshotmt eponeshotmt.c -lpthread
 */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <semaphore.h>
#include <stdio.h>
#include <limits.h>
static unsigned long ncount = 1000;
static unsigned long nrthr = 2;
static unsigned long nfds = 500;
static unsigned long nwthr = 2;
static sem_t sem;

static void *readmod(void *e)
{
	int epfd = *(int *)e;
	struct epoll_event ev;
	uint64_t val;
	unsigned long i;
	ssize_t r;
	int rc;
	int fd;

	for (i = ncount; --i != 0 ;) {
		do {
			rc = epoll_wait(epfd, &ev, 1, -1);
		} while (rc < 0 && errno == EINTR);
		if (rc != 1)
			printf("rc %d\n", rc);
		assert(rc == 1 && "bad epoll_wait");
		fd = ev.data.fd;
		r = read(fd, &val, sizeof(val));
		assert(r == sizeof(val) || (r < 0 && errno == EAGAIN));
		ev.events = EPOLLIN | EPOLLONESHOT;
		rc = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
		assert(rc == 0 && "epoll_ctl(MOD)");
	}

	rc = sem_post(&sem);
	assert(rc == 0 && "sem_post failure");
	return NULL;
}

static void *writer(void *p)
{
	int *fds = p;
	const size_t n = nfds;
	size_t i;
	const uint64_t val = 1;
	ssize_t w;

	for (;;) {
		for (i = 0; i < n; i++) {
			w = write(fds[i], &val, sizeof(val));

			assert(w == sizeof(val) || (w < 0 && errno == EAGAIN));
		}
	}

	return NULL;
}

static unsigned long parsearg(const char *pfx, unsigned long max)
{
	char *end;
	unsigned long val = strtoul(optarg, &end, 10);

	if (val > max) {
		fprintf(stderr, "%lu too large\n", val);
		exit(EXIT_FAILURE);
	}
	if (val > 0 && val < ULONG_MAX)
		return val;

	fprintf(stderr, "%s invalid: %s\n", pfx, end);
	exit(EXIT_FAILURE);
	return (unsigned long)-1;
}

static void usage(const char *argv_0)
{
	fprintf(stderr, "Usage: %s [-wtfc]\n"
	                "  -w <writer threads> (%lu)\n"
	                "  -t <epoll_wait threads> (%lu)\n"
	                "  -f <open files> (%lu)\n"
	                "  -c <epoll_wait count (per thread)> (%lu)\n",
			argv_0,
			nwthr, nrthr, nfds, ncount);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	int epfd = epoll_create1(0);
	int *fdmap;
	int rc;
	unsigned long i;
	pthread_t thr;

	while ((rc = getopt(argc, argv, "w:t:f:c:h")) != -1) {
		switch (rc) {
		case 'w':
			nwthr = parsearg("-w <writer threads>", ULONG_MAX);
			break;
		case 't':
			nrthr = parsearg("-t <epoll_wait threads>", UINT_MAX);
			break;
		case 'f':
			nfds = parsearg("-f <open files>", INT_MAX);
			break;
		case 'c':
			ncount = parsearg("-c <count>", ULONG_MAX);
			break;
		case 'h':
			usage(argv[0]);
		}
	}

	fdmap = malloc(sizeof(int) * nfds);
	assert(fdmap && "bad malloc");

	assert(epfd > 0 && "epoll_create failed");

	rc = sem_init(&sem, 0, nrthr);
	assert(rc == 0 && "sem_init failure");

	for (i = 0; i < nrthr; i++) {
		rc = sem_wait(&sem);
		assert(rc == 0 && "sem_wait failure");
		rc = pthread_create(&thr, NULL, readmod, &epfd);
		assert(rc == 0 && "pthread_create failure");
	}

	for (i = 0; i < nfds; i++) {
		int fd = eventfd(0, EFD_NONBLOCK);
		struct epoll_event ev;

		assert(fd >= 0 && "eventfd");
		fdmap[i] = fd;
		ev.data.fd = fd;
		ev.events = EPOLLIN | EPOLLONESHOT;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	}

	for (i = 0; i < nwthr; i++) {
		rc = pthread_create(&thr, NULL, writer, fdmap);
		assert(rc == 0 && "pthread_create failure");
	}

	for (i = 0; i < nrthr; i++) {
		do {
			rc = sem_wait(&sem);
		} while (rc < 0 && errno == EINTR);
		assert(rc == 0 && "sem_wait failed");
	}

	return 0;
}
