#ifndef EPOLL_SHIM_SYS_EPOLL_H_
#define EPOLL_SHIM_SYS_EPOLL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#include <stdint.h>

#include <fcntl.h>
#include <signal.h>

#define EPOLL_CLOEXEC O_CLOEXEC

enum EPOLL_EVENTS { __EPOLL_DUMMY };
#define EPOLLIN 0x001
#define EPOLLPRI 0x002
#define EPOLLOUT 0x004
#define EPOLLRDNORM 0x040
#define EPOLLNVAL 0x020
#define EPOLLRDBAND 0x080
#define EPOLLWRNORM 0x100
#define EPOLLWRBAND 0x200
#define EPOLLMSG 0x400
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDHUP @POLLRDHUP_VALUE@
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
}
#ifdef __x86_64__
__attribute__((__packed__))
#endif
;


int epoll_create(int);
int epoll_create1(int);
int epoll_ctl(int, int, int, struct epoll_event *);
int epoll_wait(int, struct epoll_event *, int, int);
int epoll_pwait(int, struct epoll_event *, int, int, sigset_t const *);


#ifndef EPOLL_SHIM_DISABLE_WRAPPER_MACROS
#include <epoll-shim/detail/common.h>
#endif


#ifdef __cplusplus
}
#endif

#endif /* sys/epoll.h */
