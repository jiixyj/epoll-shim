#include <sys/signalfd.h>
#undef read
#undef close

#include <sys/types.h>

#include <sys/event.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "signalfd_ctx.h"

struct signalfd_context {
	int fd;
	int flags;
	SignalFDCtx ctx;
	struct signalfd_context *next;
};

static struct signalfd_context *signalfd_contexts;
pthread_mutex_t signalfd_context_mtx = PTHREAD_MUTEX_INITIALIZER;

struct signalfd_context *
get_signalfd_context(int fd, bool create_new)
{
	for (struct signalfd_context *ctx = signalfd_contexts; ctx;
	     ctx = ctx->next) {
		if (fd == ctx->fd) {
			return ctx;
		}
	}

	if (create_new) {
		struct signalfd_context *new_ctx =
		    calloc(1, sizeof(struct signalfd_context));
		if (!new_ctx) {
			return NULL;
		}
		new_ctx->fd = -1;
		new_ctx->next = signalfd_contexts;
		signalfd_contexts = new_ctx;
		return new_ctx;
	}

	return NULL;
}

static int
signalfd_impl(int fd, const sigset_t *sigs, int flags)
{
	if (fd != -1) {
		errno = EINVAL;
		return -1;
	}

	if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) {
		errno = EINVAL;
		return -1;
	}

	struct signalfd_context *ctx = get_signalfd_context(-1, true);
	if (!ctx) {
		errno = EMFILE;
		return -1;
	}

	ctx->fd = kqueue();
	if (ctx->fd < 0) {
		return -1;
	}

	ctx->flags = flags;

	errno_t err;

	if ((err = signalfd_ctx_init(&ctx->ctx, ctx->fd, sigs)) != 0) {
		(void)close(ctx->fd);
		ctx->fd = -1;
		errno = err;
		return -1;
	}

	return ctx->fd;
}

int
signalfd(int fd, const sigset_t *sigs, int flags)
{
	pthread_mutex_lock(&signalfd_context_mtx);
	int ret = signalfd_impl(fd, sigs, flags);
	pthread_mutex_unlock(&signalfd_context_mtx);
	return ret;
}

static errno_t
signalfd_ctx_read_or_block(SignalFDCtx *signalfd_ctx, uint32_t *value,
    bool nonblock)
{
	for (;;) {
		errno_t ec = signalfd_ctx_read(signalfd_ctx, value);
		if (nonblock || ec != EAGAIN) {
			return (ec);
		}

		struct pollfd pfd = {.fd = signalfd_ctx->kq, .events = POLLIN};
		if (poll(&pfd, 1, -1) < 0) {
			return (errno);
		}
	}
}

ssize_t
signalfd_read(struct signalfd_context *ctx, void *buf, size_t nbytes)
{
	int fd = ctx->fd;
	int flags = ctx->flags;
	pthread_mutex_unlock(&signalfd_context_mtx);

	// TODO(jan): fix this to read multiple signals
	if (nbytes != sizeof(struct signalfd_siginfo)) {
		errno = EINVAL;
		return -1;
	}

	uint32_t signo;
	errno_t err;

	if ((err = signalfd_ctx_read_or_block(&ctx->ctx, &signo,
		 flags & SFD_NONBLOCK)) != 0) {
		errno = err;
		return -1;
	}

	struct signalfd_siginfo siginfo = {.ssi_signo = signo};
	memcpy(buf, &siginfo, sizeof(siginfo));
	return (ssize_t)sizeof(siginfo);
}

int
signalfd_close(struct signalfd_context *ctx)
{
	errno_t ec = signalfd_ctx_terminate(&ctx->ctx);

	if (close(ctx->fd) < 0) {
		ec = ec ? ec : errno;
	}
	ctx->fd = -1;

	if (ec) {
		errno = ec;
		return -1;
	}

	return 0;
}
