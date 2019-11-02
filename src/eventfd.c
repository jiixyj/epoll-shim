#include <sys/eventfd.h>
#undef read
#undef write
#undef close

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eventfd_ctx.h"

struct eventfd_context {
	int fd;
	int flags;
	EventFDCtx ctx;
	struct eventfd_context *next;
};

static struct eventfd_context *eventfd_contexts;
pthread_mutex_t eventfd_context_mtx = PTHREAD_MUTEX_INITIALIZER;

struct eventfd_context *
get_eventfd_context(int fd, bool create_new)
{
	for (struct eventfd_context *ctx = eventfd_contexts; ctx;
	     ctx = ctx->next) {
		if (fd == ctx->fd) {
			return ctx;
		}
	}

	if (create_new) {
		struct eventfd_context *new_ctx =
		    malloc(sizeof(struct eventfd_context));
		if (!new_ctx) {
			return NULL;
		}
		new_ctx->fd = -1;
		new_ctx->next = eventfd_contexts;
		eventfd_contexts = new_ctx;
		return new_ctx;
	}

	return NULL;
}

static int
eventfd_impl(unsigned int initval, int flags)
{
	if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) {
		errno = EINVAL;
		return -1;
	}

	/*
	 * Don't check that EFD_CLOEXEC is set -- but our kqueue based eventfd
	 * will always be CLOEXEC.
	 */

	struct eventfd_context *ctx = get_eventfd_context(-1, true);
	if (!ctx) {
		errno = EMFILE;
		return -1;
	}

	ctx->fd = kqueue();
	if (ctx->fd < 0) {
		return -1;
	}

	int ctx_flags = 0;
	if (flags & EFD_SEMAPHORE) {
		ctx_flags |= EVENTFD_CTX_FLAG_SEMAPHORE;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_init(&ctx->ctx, /**/
		 ctx->fd, initval, ctx_flags)) != 0) {
		errno = ec;
		return -1;
	}

	ctx->flags = flags;

	return ctx->fd;
}

int
eventfd(unsigned int initval, int flags)
{
	pthread_mutex_lock(&eventfd_context_mtx);
	int ret = eventfd_impl(initval, flags);
	pthread_mutex_unlock(&eventfd_context_mtx);
	return ret;
}

static errno_t
eventfd_ctx_read_or_block(EventFDCtx *eventfd_ctx, int kq, uint64_t *value,
    bool nonblock)
{
	for (;;) {
		errno_t ec = eventfd_ctx_read(eventfd_ctx, kq, value);
		if (nonblock || ec != EAGAIN) {
			return (ec);
		}

		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		if (poll(&pfd, 1, -1) < 0) {
			return (errno);
		}
	}
}

ssize_t
eventfd_helper_read(struct eventfd_context *ctx, void *buf, size_t nbytes)
{
	int fd = ctx->fd;
	EventFDCtx *efd_ctx = &ctx->ctx;
	int flags = ctx->flags;
	pthread_mutex_unlock(&eventfd_context_mtx);

	if (nbytes != sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_read_or_block(efd_ctx, fd, buf,
		 flags & EFD_NONBLOCK)) != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)nbytes;
}

ssize_t
eventfd_helper_write(struct eventfd_context *ctx, void const *buf,
    size_t nbytes)
{
	int fd = ctx->fd;
	EventFDCtx *efd_ctx = &ctx->ctx;
	pthread_mutex_unlock(&eventfd_context_mtx);

	if (nbytes != sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	uint64_t value;
	memcpy(&value, buf, sizeof(uint64_t));

	errno_t ec;
	if ((ec = eventfd_ctx_write(efd_ctx, fd, value)) != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)nbytes;
}

int
eventfd_close(struct eventfd_context *ctx)
{
	errno_t ec = eventfd_ctx_terminate(&ctx->ctx);

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

int
eventfd_read(int fd, eventfd_t *value)
{
	struct eventfd_context *ctx;

	(void)pthread_mutex_lock(&eventfd_context_mtx);
	ctx = get_eventfd_context(fd, false);
	(void)pthread_mutex_unlock(&eventfd_context_mtx);

	if (!ctx) {
		errno = EBADF;
		return -1;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_read_or_block(&ctx->ctx, fd, value,
		 ctx->flags & EFD_NONBLOCK)) != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}

int
eventfd_write(int fd, eventfd_t value)
{
	struct eventfd_context *ctx;

	(void)pthread_mutex_lock(&eventfd_context_mtx);
	ctx = get_eventfd_context(fd, false);
	(void)pthread_mutex_unlock(&eventfd_context_mtx);

	if (!ctx) {
		errno = EBADF;
		return -1;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_write(&ctx->ctx, fd, value)) != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}
