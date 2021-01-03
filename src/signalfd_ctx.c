#include "signalfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

static errno_t
signalfd_has_pending(SignalFDCtx const *signalfd, bool *has_pending)
{
	sigset_t pending_sigs;

	if (sigpending(&pending_sigs) < 0 ||
	    sigandset(&pending_sigs, &pending_sigs, &signalfd->sigs) < 0) {
		return errno;
	}

	*has_pending = !sigisemptyset(&pending_sigs);
	return 0;
}

static errno_t
signalfd_ctx_trigger_manually(SignalFDCtx *signalfd)
{
	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
	if (kevent(signalfd->kq, &kev, 1, NULL, 0, NULL) < 0) {
		return errno;
	}
	return 0;
}

errno_t
signalfd_ctx_init(SignalFDCtx *signalfd, int kq, const sigset_t *sigs)
{
	errno_t ec;

	assert(sigs != NULL);

	*signalfd = (SignalFDCtx){.kq = kq, .sigs = *sigs};

	if ((ec = pthread_mutex_init(&signalfd->mutex, NULL)) != 0) {
		return ec;
	}

#ifndef _SIG_MAXSIG
#define _SIG_MAXSIG (8 * sizeof(sigset_t))
#endif

	struct kevent kevs[_SIG_MAXSIG + 1];
	int n = 0;

	EV_SET(&kevs[n++], 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(&signalfd->sigs, i)) {
			EV_SET(&kevs[n++], i, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
		}
	}

	n = kevent(signalfd->kq, kevs, n, NULL, 0, NULL);
	if (n < 0) {
		ec = errno;
		goto out;
	}

	bool has_pending;
	if ((ec = signalfd_has_pending(signalfd, &has_pending)) != 0) {
		goto out;
	}
	if (has_pending) {
		if ((ec = signalfd_ctx_trigger_manually(signalfd)) != 0) {
			goto out;
		}
	}

	return 0;

out:
	signalfd_ctx_terminate(signalfd);
	return ec;
}

errno_t
signalfd_ctx_terminate(SignalFDCtx *signalfd)
{
	return pthread_mutex_destroy(&signalfd->mutex);
}

static errno_t
signalfd_ctx_read_impl(SignalFDCtx *signalfd, uint32_t *ident)
{
	/*
	 * EVFILT_SIGNAL is an "observer". It does not hook into the
	 * signal disposition mechanism. On the other hand, `signalfd` does.
	 * Therefore, to properly emulate `signalfd`, `sigtimedwait` must be
	 * called.
	 */

	int s = sigtimedwait(&signalfd->sigs, NULL, &(struct timespec){0, 0});
	if (s < 0) {
		return errno;
	}

	*ident = (uint32_t)s;
	return 0;
}

static bool
signalfd_ctx_clear_signal(SignalFDCtx *signalfd, bool was_triggered)
{
	if (was_triggered) {
		/*
		 * When there are other signals pending we can keep the kq
		 * readable and therefore don't need to clear it.
		 */
		bool has_pending;
		if (signalfd_has_pending(signalfd, &has_pending) != 0 ||
		    has_pending) {
			return true;
		}
	}

	/*
	 * Clear the kq. Signals can arrive here, leading to a race.
	 */

	{
		struct kevent kevs[32];
		int n;

		while ((n = kevent(signalfd->kq, NULL, 0, /**/
			    kevs, 32, &(struct timespec){0, 0})) > 0) {
		}
	}

	/*
	 * Because of the race, we must recheck and manually trigger if
	 * necessary.
	 */
	bool has_pending;
	if (signalfd_has_pending(signalfd, &has_pending) != 0 || has_pending) {
		(void)signalfd_ctx_trigger_manually(signalfd);
		return true;
	}
	return false;
}

errno_t
signalfd_ctx_read(SignalFDCtx *signalfd, uint32_t *ident)
{
	errno_t ec;

	(void)pthread_mutex_lock(&signalfd->mutex);
	ec = signalfd_ctx_read_impl(signalfd, ident);
	if (ec == 0 || ec == EAGAIN || ec == EWOULDBLOCK) {
		(void)signalfd_ctx_clear_signal(signalfd, false);
	}
	(void)pthread_mutex_unlock(&signalfd->mutex);

	return ec;
}

void
signalfd_ctx_poll(SignalFDCtx *signalfd, uint32_t *revents)
{
	(void)pthread_mutex_lock(&signalfd->mutex);

	bool pending = signalfd_ctx_clear_signal(signalfd, revents != NULL);
	if (revents) {
		*revents = pending ? POLLIN : 0;
	}

	(void)pthread_mutex_unlock(&signalfd->mutex);
}
