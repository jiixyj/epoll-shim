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

ssize_t timerfd_read(int fd, void *buf, size_t nbytes);
#define read timerfd_read

int timerfd_close(int fd);
#define close timerfd_close

#ifdef __cplusplus
}
#endif

#endif