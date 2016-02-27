#include <sys/signalfd.h>

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int
signalfd(int fd, const sigset_t *sigs, int flags)
{
	if (fd != -1 || flags != SFD_NONBLOCK) {
		errno = EINVAL;
		return -1;
	}

	int kq = kqueue();
	if (kq == -1) {
		return -1;
	}

	struct kevent kevs[_SIG_MAXSIG];
	int nchanges = 0;

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(sigs, i)) {
			EV_SET(&kevs[nchanges++], i, EVFILT_SIGNAL, EV_ADD, 0,
			    0, 0);
		}
	}

	int ret = kevent(kq, kevs, nchanges, NULL, 0, NULL);
	if (ret == -1) {
		close(kq);
		return -1;
	}

	return kq;
}
