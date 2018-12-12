/*
 * Run (ie 12 cpus):
 *  for c in $(seq 1 12);do echo ================ $c ===================;for i in $(seq 1 $c); do NO_READ=1 timeout 5 ./epoll-test& done;sleep 6; done >log 2>&1
 * Collect numbers: cat log|awk '/====/{cpus=$2}/calls/{sum[cpus]+=$1;num[cpus]++}END{for (c=1;c<=12;c++){printf "%d %2.2d\n", c,sum[c]/num[c]}}'|awk '{print $NF}'
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <sys/epoll.h>
#include <assert.h>
#include <signal.h>

long calls;
long empty;
void alarm_handler(int s)
{
  alarm(1);
  fprintf(stderr,"%ld calls, %d empty\n",calls,empty);
  calls = 0;
  empty = 0;
}

int main(int argc, char** argv)
{
  int v;
  int p[2];
  char buf[1024];
  struct epoll_event ev, events[10];
  int noread = getenv("NO_READ") != NULL;
  int nowrite = getenv("NO_WRITE") != NULL;
  int epoll_fd;
  int no_block = 0;

  // Create two epoll sets
  int ep1 = epoll_create(10);
  int ep2 = epoll_create(10);
  if ( ep1 < 0 || ep2 < 0) {
     error(-1, errno, "epoll_create");
  }

  // Default is to use a two level epoll hierarchy
  if(!getenv("NO_2LEVEL_REGISTER")) {
     ev.events = EPOLLIN;
     ev.data.u64 = 42; // Some number
     if (epoll_ctl(ep1, EPOLL_CTL_ADD, ep2, &ev) < 0) {
        error(-1, errno, "epoll_ctl");
     }
  }

  // Pipe to wait for
  if (pipe(p) < 0) {
     error(-1, errno, "pipe");
  }

  // Add the read end of the pipe to the lower level
  // epoll set
  ev.events = EPOLLIN;
  ev.data.u64 = 43; // Some other number
  if (epoll_ctl(ep2, EPOLL_CTL_ADD, p[0], &ev) < 0) {
     error(-1, errno, "epoll_ctl");
  }

  signal(SIGALRM,alarm_handler);
  alarm(1);

  // Add a byte to the pipe
  if (!nowrite) {
     if (write(p[1], buf, 1) < 0) {
        error(-1, errno, "write");
     }
  }

  // Select which epoll set to wait on,
  // default is top level
  epoll_fd = getenv("NO_2LEVEL_POLL") ? ep2 : ep1;

  // Non blocking poll?
  no_block = getenv("NO_BLOCK") ? 1 : 0;

  // Loop on the event(s)
  for(;;) {
     calls++;
     v = epoll_wait(epoll_fd, events, 1, no_block ? 0 : 1000);
     if (v < 0) {
        if(errno == EINTR) {
           continue;
        }
        error(-1, errno, "epoll_wait");
     }
     if (v == 0) {
        empty++;
        continue;
     }
     assert(v == 1 );
     assert(events[0].data.fd == 42 || events[0].data.fd == 43);
     assert(events[0].events & EPOLLIN);
     if (noread) {
        // Spin on the event we already got
        continue;
     }

     // Move a byte through the pipe
     if ((v = read(p[0], buf, sizeof(buf))) <= 0) {
        error(v==0 ? 0 : -1, errno, "read");
     }
     if (!nowrite) {
        if (write(p[1], buf, 1) < 0) {
           error(-1, errno, "write");
        }
     }
  }
}
