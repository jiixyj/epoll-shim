#include <sys/eventfd.h>
#undef read
#undef close

#include <sys/types.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eventfd_ctx.h"

struct eventfd_context {
	int fd;
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

	/* Only EFD_CLOEXEC and EFD_NONBLOCK eventfds are supported for now. */
	if ((flags & (EFD_CLOEXEC | EFD_NONBLOCK)) !=
	    (EFD_CLOEXEC | EFD_NONBLOCK)) {
		errno = EINVAL;
		return -1;
	}

	struct eventfd_context *ctx = get_eventfd_context(-1, true);
	if (!ctx) {
		errno = EMFILE;
		return -1;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_init(&ctx->ctx, initval,
		 flags & EFD_SEMAPHORE)) != 0) {
		errno = ec;
		return -1;
	}

	ctx->fd = eventfd_ctx_fd(&ctx->ctx);

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

ssize_t
eventfd_helper_read(struct eventfd_context *ctx, void *buf, size_t nbytes)
{
	EventFDCtx *efd_ctx = &ctx->ctx;
	pthread_mutex_unlock(&eventfd_context_mtx);

	if (nbytes != sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	errno_t ec;
	if ((ec = eventfd_ctx_read(efd_ctx, buf)) != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)nbytes;
}

ssize_t
eventfd_helper_write(struct eventfd_context *ctx, void const *buf,
    size_t nbytes)
{
	EventFDCtx *efd_ctx = &ctx->ctx;
	pthread_mutex_unlock(&eventfd_context_mtx);

	if (nbytes != sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	uint64_t value;
	memcpy(&value, buf, sizeof(uint64_t));

	errno_t ec;
	if ((ec = eventfd_ctx_write(efd_ctx, value)) != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)nbytes;
}

int
eventfd_close(struct eventfd_context *ctx)
{
	ctx->fd = -1;

	errno_t ec;
	if ((ec = eventfd_ctx_terminate(&ctx->ctx)) != 0) {
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
	if ((ec = eventfd_ctx_read(&ctx->ctx, value)) != 0) {
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
	if ((ec = eventfd_ctx_write(&ctx->ctx, value)) != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}
