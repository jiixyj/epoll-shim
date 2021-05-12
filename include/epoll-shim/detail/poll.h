#ifndef SHIM_SYS_SHIM_HELPERS_POLL
#define SHIM_SYS_SHIM_HELPERS_POLL

#include <poll.h>   /* IWYU pragma: keep */
#include <signal.h> /* IWYU pragma: keep */

extern int epoll_shim_poll(struct pollfd *, nfds_t, int);
extern int epoll_shim_ppoll(struct pollfd *, nfds_t, struct timespec const *,
    sigset_t const *);
#define poll(fds, nfds, timeout) epoll_shim_poll(fds, nfds, timeout)
#define ppoll(fds, nfds, timeout, newsigmask) epoll_shim_ppoll(fds, nfds, timeout, newsigmask)

#endif
