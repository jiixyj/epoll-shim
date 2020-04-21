#include "signalfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>

#include <assert.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>

errno_t
signalfd_ctx_init(SignalFDCtx *signalfd, int kq, const sigset_t *sigs)
{
	*signalfd = (SignalFDCtx){.kq = kq};

#ifndef _SIG_MAXSIG
#define _SIG_MAXSIG (8 * sizeof(sigset_t))
#endif

	struct kevent kevs[_SIG_MAXSIG];
	int n = 0;

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(sigs, i)) {
			EV_SET(&kevs[n++], i, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
		}
	}

	n = kevent(signalfd->kq, kevs, n, NULL, 0, NULL);
	if (n < 0) {
		return errno;
	}

	return 0;
}

errno_t
signalfd_ctx_terminate(SignalFDCtx *signalfd)
{
	(void)signalfd;

	return (0);
}

errno_t
signalfd_ctx_read(SignalFDCtx *signalfd, uint32_t *ident)
{
	struct kevent kev;

	int n = kevent(signalfd->kq, NULL, 0, /**/
	    &kev, 1, &(struct timespec){0, 0});
	if (n < 0) {
		return errno;
	}

	if (n == 0) {
		return EAGAIN;
	}

	*ident = (uint32_t)kev.ident;
	return 0;
}
