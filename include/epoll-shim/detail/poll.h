#ifndef EPOLL_SHIM_DETAIL_POLL_H_
#define EPOLL_SHIM_DETAIL_POLL_H_

#include <poll.h>
#include <signal.h>

extern int epoll_shim_poll(struct pollfd *, nfds_t, int);
#define poll(...) epoll_shim_poll(__VA_ARGS__)

extern int epoll_shim_ppoll(struct pollfd *, nfds_t, struct timespec const *,
    sigset_t const *);
#define ppoll(...) epoll_shim_ppoll(__VA_ARGS__)

#endif
