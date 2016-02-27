#include <sys/param.h>
#include <sys/types.h>

#include <unistd.h>

extern int timerfd_fds[8];
extern ssize_t timerfd_read(int fd, void *buf, size_t nbytes);
extern int timerfd_close(int fd);

extern int signalfd_fds[8];
extern ssize_t signalfd_read(int fd, void *buf, size_t nbytes);
extern int signalfd_close(int fd);

int
epoll_shim_close(int fd)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			return timerfd_close(fd);
		}
	}

	for (i = 0; i < nitems(signalfd_fds); ++i) {
		if (signalfd_fds[i] == fd) {
			return signalfd_close(fd);
		}
	}

	return close(fd);
}

ssize_t
epoll_shim_read(int fd, void *buf, size_t nbytes)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			return timerfd_read(fd, buf, nbytes);
		}
	}

	for (i = 0; i < nitems(signalfd_fds); ++i) {
		if (signalfd_fds[i] == fd) {
			return signalfd_read(fd, buf, nbytes);
		}
	}

	return read(fd, buf, nbytes);
}
