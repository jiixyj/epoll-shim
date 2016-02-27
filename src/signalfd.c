#include <sys/signalfd.h>
#undef read
#undef close

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int signalfd_fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int signalfd_flags[8];

int
signalfd(int fd, const sigset_t *sigs, int flags)
{
	if (fd != -1) {
		errno = EINVAL;
		return -1;
	}

	if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) {
		errno = EINVAL;
		return -1;
	}

	unsigned i;
	for (i = 0; i < nitems(signalfd_fds); ++i) {
		if (signalfd_fds[i] == -1) {
			break;
		}
	}

	if (i == nitems(signalfd_fds)) {
		errno = EMFILE;
		return -1;
	}

	signalfd_fds[i] = kqueue();
	if (signalfd_fds[i] == -1) {
		return -1;
	}

	signalfd_flags[i] = flags;

	struct kevent kevs[_SIG_MAXSIG];
	int nchanges = 0;

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(sigs, i)) {
			EV_SET(&kevs[nchanges++], i, EVFILT_SIGNAL, EV_ADD, 0,
			    0, 0);
		}
	}

	int ret = kevent(signalfd_fds[i], kevs, nchanges, NULL, 0, NULL);
	if (ret == -1) {
		close(signalfd_fds[i]);
		signalfd_fds[i] = -1;
		return -1;
	}

	return signalfd_fds[i];
}

ssize_t
signalfd_read(int fd, void *buf, size_t nbytes)
{
	unsigned i;
	for (i = 0; i < nitems(signalfd_fds); ++i) {
		if (signalfd_fds[i] == fd) {
			break;
		}
	}

	if (i == nitems(signalfd_fds)) {
		return read(fd, buf, nbytes);
	}

	// TODO: fix this to read multiple signals
	if (nbytes != sizeof(struct signalfd_siginfo)) {
		errno = EINVAL;
		return -1;
	}

	struct timespec timeout = {0, 0};
	struct kevent kev;
	int ret = kevent(fd, NULL, 0, &kev, 1,
	    (signalfd_flags[i] & SFD_NONBLOCK) ? &timeout : NULL);
	if (ret == -1) {
		return -1;
	} else if (ret == 0) {
		errno = EAGAIN;
		return -1;
	}

	memset(buf, '\0', nbytes);
	struct signalfd_siginfo *sig_buf = buf;
	sig_buf->ssi_signo = kev.ident;
	return nbytes;
}

int
signalfd_close(int fd)
{
	unsigned i;
	for (i = 0; i < nitems(signalfd_fds); ++i) {
		if (signalfd_fds[i] == fd) {
			signalfd_fds[i] = -1;
			break;
		}
	}

	return close(fd);
}
