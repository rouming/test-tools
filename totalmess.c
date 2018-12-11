/*
 *  totalmess by Davide Libenzi (epoll multithread test app)
 *  Copyright (C) 2003,..,2007  Davide Libenzi
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
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 *
 *  gcc -o totalmess totalmess.c -lpthread
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>


#define EPOLL_WAIT_TIMEO 500

#define TMPRINTF(a) fprintf a


static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int *pipes;
static int num_pipes, num_active, num_threads, num_evts;
static int epfd;
static int epet;
static int stop;




static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return (unsigned long long) tm.tv_sec * 1000000ULL + (unsigned long long) tm.tv_usec;
}

static void *thproc(void *data) {
	long thn = (long) data, nevt, i, idx, widx, fd;
	unsigned int rseed = (unsigned int) thn;
	struct epoll_event *events;
	struct epoll_event ev;
	char ch;

	events = calloc(num_evts, sizeof(struct epoll_event));
	if (!events) {
		perror("malloc");
		exit(1);
	}
	pthread_mutex_lock(&mtx);
	pthread_mutex_unlock(&mtx);
	TMPRINTF((stderr, "[%d] started\n", thn));

	for (; !stop;) {
		nevt = epoll_wait(epfd, events, num_evts, EPOLL_WAIT_TIMEO);
		TMPRINTF((stderr, "[%d] nevents=%d\n", thn, nevt));
		for (i = 0; i < nevt; i++) {
			idx = (int) events[i].data.u32;
			widx = rand_r(&rseed) % num_pipes;
			fd = pipes[2 * idx];

			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
			ev.events = EPOLLIN | epet;
			ev.data.u32 = idx;
			epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

			read(fd, &ch, 1);
			write(pipes[2 * widx + 1], "e", 1);
		}
	}
	free(events);
	TMPRINTF((stderr, "[%d] ended\n", thn));
	return (void *) 0;
}

static void sig_int(int sig) {

	++stop;
	signal(sig, sig_int);
}

static void usage(char const *prg) {

	fprintf(stderr,
		"use: %s [-atnveh]\n\n"
		"\t-t NTHREADS     : Number of threads\n"
		"\t-a NACTIVE      : Number of active threads\n"
		"\t-n NPIPES       : Number of pipes\n"
		"\t-v NEVENTS      : Max number of events per wait\n\n"
		"\t-e              : Use epoll ET\n"
		"\t-h              : Usage screen\n", prg);
}

int main (int argc, char **argv) {
	int i, c;
	unsigned long long tr;
	int *cp;
	struct epoll_event ev;
	extern char *optarg;
	pthread_t *thid;
	struct rlimit rl;

	num_pipes = 100;
	num_evts = -1;
	num_active = 1;
	num_threads = 2;
	while ((c = getopt(argc, argv, "n:a:t:v:eh")) != -1) {
		switch (c) {
		case 'n':
			num_pipes = atoi(optarg);
			break;
		case 'a':
			num_active = atoi(optarg);
			break;
		case 't':
			num_threads = atoi(optarg);
			break;
		case 'v':
			num_evts = atoi(optarg);
			break;
		case 'e':
			epet = EPOLLET;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (num_evts < 0)
		num_evts = num_pipes / 5 + 1;
	rl.rlim_cur = rl.rlim_max = num_pipes * 2 + 50;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		perror("setrlimit");
		return 1;
	}
	pipes = calloc(num_pipes * 2, sizeof(int));
	thid = calloc(num_threads, sizeof(pthread_t));
	if (!pipes || !thid) {
		perror("malloc");
		return 1;
	}
	signal(SIGINT, sig_int);
	if ((epfd = epoll_create(num_pipes)) == -1) {
		perror("epoll_create");
		return 2;
	}
	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
		if (pipe(cp) == -1) {
			perror("pipe");
			return 3;
		}
		fcntl(cp[0], F_SETFL, fcntl(cp[0], F_GETFL) | O_NONBLOCK);
	}
	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
		ev.events = EPOLLIN | epet;
		ev.data.u32 = i;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, cp[0], &ev) < 0) {
			perror("epoll_ctl");
			return 4;
		}
	}
	pthread_mutex_lock(&mtx);
	TMPRINTF((stderr, "creating %d threads ...", num_threads));
	for (i = 0; i < num_threads; i++) {
		if (pthread_create(&thid[i], NULL, thproc,
				   (void *) (long) i) != 0) {
			perror("pthread_create()");
			return 5;
		}
	}
	TMPRINTF((stderr, "done\n"));

	for (i = 0; i < num_active; i++)
		write(pipes[2 * i + 1], "e", 1);
	pthread_mutex_unlock(&mtx);

	for (; !stop;)
		sleep(1);
	for (i = 0; i < num_threads; i++)
		pthread_join(thid[i], NULL);
	TMPRINTF((stderr, "quitting\n"));

	return 0;
}

