#ifndef SHIM_SYS_SHIM_HELPERS_POLL
#define SHIM_SYS_SHIM_HELPERS_POLL

#include <poll.h> /* IWYU pragma: keep */

extern int epoll_shim_poll(struct pollfd *, nfds_t, int);
extern int epoll_shim_ppoll(struct pollfd *, nfds_t, struct timespec const *,
    sigset_t const *);
#define poll epoll_shim_poll
#define ppoll epoll_shim_ppoll

#endif
