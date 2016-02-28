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

struct signalfd_context {
	int fd;
	int flags;
};

static struct signalfd_context signalfd_contexts[8] = {
    {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};

struct signalfd_context *
get_signalfd_context(int fd)
{
	for (unsigned i = 0; i < nitems(signalfd_contexts); ++i) {
		if (fd == signalfd_contexts[i].fd) {
			return &signalfd_contexts[i];
		}
	}
	return NULL;
}

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

	struct signalfd_context *ctx = get_signalfd_context(-1);
	if (!ctx) {
		errno = EMFILE;
		return -1;
	}

	ctx->fd = kqueue();
	if (ctx->fd == -1) {
		return -1;
	}

	ctx->flags = flags;

	struct kevent kevs[_SIG_MAXSIG];
	int n = 0;

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(sigs, i)) {
			EV_SET(&kevs[n++], i, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
		}
	}

	int ret = kevent(ctx->fd, kevs, n, NULL, 0, NULL);
	if (ret == -1) {
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	return ctx->fd;
}

ssize_t
signalfd_read(struct signalfd_context *ctx, void *buf, size_t nbytes)
{
	// TODO: fix this to read multiple signals
	if (nbytes != sizeof(struct signalfd_siginfo)) {
		errno = EINVAL;
		return -1;
	}

	struct timespec timeout = {0, 0};
	struct kevent kev;
	int ret = kevent(ctx->fd, NULL, 0, &kev, 1,
	    (ctx->flags & SFD_NONBLOCK) ? &timeout : NULL);
	if (ret == -1) {
		return -1;
	} else if (ret == 0) {
		errno = EAGAIN;
		return -1;
	}

	memset(buf, '\0', nbytes);
	struct signalfd_siginfo *sig_buf = buf;
	sig_buf->ssi_signo = (uint32_t)kev.ident;
	return (ssize_t)nbytes;
}

int
signalfd_close(struct signalfd_context *ctx)
{
	ctx->fd = -1;
	return close(ctx->fd);
}
