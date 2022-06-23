#ifndef EPOLL_SHIM_SYS_TIMERFD_H_
#define EPOLL_SHIM_SYS_TIMERFD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <fcntl.h>
#include <time.h>

#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC

#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

#ifdef __APPLE__
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
#else
struct itimerspec;
#endif

int timerfd_create(int, int);
int timerfd_settime(int, int, struct itimerspec const *, struct itimerspec *);
int timerfd_gettime(int, struct itimerspec *);


#ifndef EPOLL_SHIM_DISABLE_WRAPPER_MACROS
#include <epoll-shim/detail/common.h>
#include <epoll-shim/detail/poll.h>
#include <epoll-shim/detail/read.h>
#endif


#ifdef __cplusplus
}
#endif

#endif
