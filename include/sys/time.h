#ifndef EPOLL_SHIM_SYS_TIME_H_
#define EPOLL_SHIM_SYS_TIME_H_

#ifdef __cplusplus
extern "C" {
#endif

#include_next <sys/time.h>

#if __APPLE__
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
#endif // __APPLE__

#ifdef __cplusplus
}
#endif

#endif // EPOLL_SHIM_SYS_TIME_H_
