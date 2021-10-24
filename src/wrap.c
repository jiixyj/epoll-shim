#include "wrap.h"

#include <stdarg.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include "epoll_shim_ctx.h"
#include "epoll_shim_export.h"

static struct {
	pthread_once_t wrap_init;

	typeof(read) *real_read;
	typeof(write) *real_write;
	typeof(close) *real_close;
	typeof(poll) *real_poll;
#ifdef __NetBSD__
	typeof(pollts) *real___pollts50;
#else
	typeof(ppoll) *real_ppoll;
#endif
	typeof(fcntl) *real_fcntl;
} wrap = { .wrap_init = PTHREAD_ONCE_INIT };

static void
wrap_initialize_impl(void)
{
#define WRAP(fun)                                         \
	do {                                              \
		wrap.real_##fun = dlsym(RTLD_NEXT, #fun); \
	} while (0)

	WRAP(read);
	WRAP(write);
	WRAP(close);
	WRAP(poll);
#ifdef __NetBSD__
	WRAP(__pollts50);
#else
	WRAP(ppoll);
#endif
	WRAP(fcntl);

#undef WRAP
}

static void
wrap_initialize(void)
{
	int oe = errno;
	(void)pthread_once(&wrap.wrap_init, wrap_initialize_impl);
	errno = oe;
}

#define WRAPPERS

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
ssize_t
read(int fd, void *buf, size_t nbytes)
{
	return epoll_shim_read(fd, buf, nbytes);
}
#endif

ssize_t
real_read(int fd, void *buf, size_t nbytes)
{
	wrap_initialize();
	return wrap.real_read(fd, buf, nbytes);
}

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
ssize_t
write(int fd, void const *buf, size_t nbytes)
{
	return epoll_shim_write(fd, buf, nbytes);
}
#endif

ssize_t
real_write(int fd, void const *buf, size_t nbytes)
{
	wrap_initialize();
	return wrap.real_write(fd, buf, nbytes);
}

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
int
close(int fd)
{
	return epoll_shim_close(fd);
}
#endif

int
real_close(int fd)
{
	wrap_initialize();
	return wrap.real_close(fd);
}

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
int
poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
	return epoll_shim_poll(fds, nfds, timeout);
}
#endif

int
real_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
	wrap_initialize();
	return wrap.real_poll(fds, nfds, timeout);
}

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
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

int
real_ppoll(struct pollfd fds[], nfds_t nfds,
    struct timespec const *restrict timeout,
    sigset_t const *restrict newsigmask)
{
	wrap_initialize();
#ifdef __NetBSD__
	return wrap.real___pollts50(fds, nfds, timeout, newsigmask);
#else
	return wrap.real_ppoll(fds, nfds, timeout, newsigmask);
#endif
}

#ifdef WRAPPERS
EPOLL_SHIM_EXPORT
int
fcntl(int fd, int cmd, ...)
{
	va_list ap;

	va_start(ap, cmd);
	void *arg = va_arg(ap, void *);
	int rv = epoll_shim_fcntl(fd, cmd, arg);
	va_end(ap);

	return rv;
}
#endif

int
real_fcntl(int fd, int cmd, ...)
{
	wrap_initialize();
	va_list ap;

	va_start(ap, cmd);
	void *arg = va_arg(ap, void *);
	int rv = wrap.real_fcntl(fd, cmd, arg);
	va_end(ap);

	return rv;
}
