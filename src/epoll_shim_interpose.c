#include <stdarg.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "epoll_shim_interpose_export.h"
#include "epoll_shim_ctx.h"

EPOLL_SHIM_INTERPOSE_EXPORT
ssize_t
read(int fd, void *buf, size_t nbytes)
{
	return epoll_shim_read(fd, buf, nbytes);
}

EPOLL_SHIM_INTERPOSE_EXPORT
ssize_t
write(int fd, void const *buf, size_t nbytes)
{
	return epoll_shim_write(fd, buf, nbytes);
}

EPOLL_SHIM_INTERPOSE_EXPORT
int
close(int fd)
{
	return epoll_shim_close(fd);
}

EPOLL_SHIM_INTERPOSE_EXPORT
int
poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
	return epoll_shim_poll(fds, nfds, timeout);
}

#if !defined(__APPLE__)
EPOLL_SHIM_INTERPOSE_EXPORT
int
#ifdef __NetBSD__
__pollts50
#else
ppoll
#endif
    (struct pollfd fds[], nfds_t nfds, struct timespec const *restrict timeout,
	sigset_t const *restrict newsigmask)
{
	return epoll_shim_ppoll(fds, nfds, timeout, newsigmask);
}
#endif

EPOLL_SHIM_INTERPOSE_EXPORT
int
fcntl(int fd, int cmd, ...)
{
	va_list ap;
	intptr_t arg;

	va_start(ap, cmd);
	arg = epoll_shim_fcntl_optional_arg(cmd, ap);
	va_end(ap);
	int rv = epoll_shim_fcntl(fd, cmd, arg);

	return rv;
}
