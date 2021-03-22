#include "compat_kqueue1.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/time.h>

#include <errno.h>

#include <fcntl.h>
#include <unistd.h>

static errno_t
compat_kqueue1_impl(int *fd_out, int flags)
{
	errno_t ec;

	if (flags & ~(O_CLOEXEC)) {
		return EINVAL;
	}

	int fd = kqueue();
	if (fd < 0) {
		return errno;
	}

	{
		int r;

		if (flags & O_CLOEXEC) {
			if ((r = fcntl(fd, F_GETFD, 0)) < 0 ||
			    fcntl(fd, F_SETFD, r | FD_CLOEXEC) < 0) {
				ec = errno;
				goto out;
			}
		}
	}

	*fd_out = fd;
	return 0;

out:
	(void)close(fd);
	return ec;
}

int
compat_kqueue1(int flags)
{
	int fd;
	errno_t ec = compat_kqueue1_impl(&fd, flags);
	if (ec != 0) {
		errno = ec;
		return -1;
	}
	return fd;
}
