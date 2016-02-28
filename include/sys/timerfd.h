#ifndef SHIM_SYS_TIMERFD_H
#define SHIM_SYS_TIMERFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC

#define TFD_TIMER_ABSTIME 1

struct itimerspec;

int timerfd_create(int, int);
int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
#if 0
int timerfd_gettime(int, struct itimerspec *);
#endif

extern int epoll_shim_close(int fd);
extern ssize_t epoll_shim_read(int fd, void *buf, size_t nbytes);
#define read epoll_shim_read
#define close epoll_shim_close

#ifdef __cplusplus
}
#endif

#endif
