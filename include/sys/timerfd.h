#ifndef SHIM_SYS_TIMERFD_H
#define SHIM_SYS_TIMERFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <fcntl.h>

#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC

#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

struct itimerspec;

int timerfd_create(int, int);
int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
int timerfd_gettime(int, struct itimerspec *);


#ifndef SHIM_SYS_SHIM_HELPERS
#define SHIM_SYS_SHIM_HELPERS
#include <fcntl.h>  /* IWYU pragma: keep */
#include <unistd.h> /* IWYU pragma: keep */

extern int epoll_shim_close(int);
#define close epoll_shim_close

extern int epoll_shim_fcntl(int, int, ...);
#define SHIM_SYS_SHIM_HELPERS_SELECT(PREFIX, _2, _1, SUFFIX, ...) \
	PREFIX##_##SUFFIX
#define SHIM_SYS_SHIM_FCNTL_1(fd, cmd) fcntl((fd), (cmd))
#define SHIM_SYS_SHIM_FCNTL_N(fd, cmd, ...)                                \
	(((cmd) == F_SETFL) ? epoll_shim_fcntl((fd), (cmd), __VA_ARGS__) : \
				    fcntl((fd), (cmd), __VA_ARGS__))
#define fcntl(fd, ...)                                                       \
	SHIM_SYS_SHIM_HELPERS_SELECT(SHIM_SYS_SHIM_FCNTL, __VA_ARGS__, N, 1) \
	(fd, __VA_ARGS__)
#endif

#ifndef SHIM_SYS_SHIM_HELPERS_READ
#define SHIM_SYS_SHIM_HELPERS_READ
#include <unistd.h> /* IWYU pragma: keep */

extern ssize_t epoll_shim_read(int, void *, size_t);
#define read epoll_shim_read
#endif


#ifdef __cplusplus
}
#endif

#endif
