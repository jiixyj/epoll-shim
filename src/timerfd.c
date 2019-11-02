#include <sys/timerfd.h>
#undef read
#undef close

#include <sys/event.h>
#include <sys/select.h>

#include <poll.h>
#include <pthread.h>
#include <pthread_np.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "timerfd_ctx.h"

struct timerfd_context {
	int fd;
	int flags;
	TimerFDCtx ctx;
	struct timerfd_context *next;
};

static struct timerfd_context *timerfd_contexts;
pthread_mutex_t timerfd_context_mtx = PTHREAD_MUTEX_INITIALIZER;

struct timerfd_context *
get_timerfd_context(int fd, bool create_new)
{
	for (struct timerfd_context *ctx = timerfd_contexts; ctx;
	     ctx = ctx->next) {
		if (fd == ctx->fd) {
			return ctx;
		}
	}

	if (create_new) {
		struct timerfd_context *new_ctx =
		    malloc(sizeof(struct timerfd_context));
		if (!new_ctx) {
			return NULL;
		}

		*new_ctx = (struct timerfd_context){
		    .fd = -1,
		    .next = timerfd_contexts,
		};
		timerfd_contexts = new_ctx;

		return new_ctx;
	}

	return NULL;
}

static errno_t
timerfd_create_impl(int clockid, int flags, int *fd)
{
	errno_t err;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		return EINVAL;
	}

	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		return EINVAL;
	}

	struct timerfd_context *ctx = get_timerfd_context(-1, true);
	if (!ctx) {
		return ENOMEM;
	}

	ctx->fd = kqueue();
	if (ctx->fd < 0) {
		assert(errno != 0);
		return errno;
	}

	if ((err = timerfd_ctx_init(&ctx->ctx, ctx->fd, clockid)) != 0) {
		goto out;
	}

	ctx->flags = flags;

	*fd = ctx->fd;
	return 0;

out:
	close(ctx->fd);
	ctx->fd = -1;
	return err;
}

int
timerfd_create(int clockid, int flags)
{
	int fd;
	errno_t err;

	pthread_mutex_lock(&timerfd_context_mtx);
	err = timerfd_create_impl(clockid, flags, &fd);
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (err != 0) {
		errno = err;
		return -1;
	}

	return fd;
}

static errno_t
timerfd_settime_impl(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t err;
	struct timerfd_context *ctx;

	if (!new) {
		return EFAULT;
	}

	ctx = get_timerfd_context(fd, false);
	if (!ctx) {
		return EINVAL;
	}

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		return EINVAL;
	}

	if ((err = timerfd_ctx_settime(&ctx->ctx,
		 (flags & TFD_TIMER_ABSTIME) ? TIMER_ABSTIME : 0, /**/
		 new, old)) != 0) {
		return err;
	}

	return 0;
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t err;

	pthread_mutex_lock(&timerfd_context_mtx);
	err = timerfd_settime_impl(fd, flags, new, old);
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (err != 0) {
		errno = err;
		return -1;
	}

	return 0;
}

#if 0
int timerfd_gettime(int fd, struct itimerspec *cur)
{
	return syscall(SYS_timerfd_gettime, fd, cur);
}
#endif

static errno_t
timerfd_ctx_read_or_block(TimerFDCtx *timerfd, uint64_t *value, bool nonblock)
{
	for (;;) {
		errno_t ec = timerfd_ctx_read(timerfd, value);
		if (nonblock || ec != EAGAIN) {
			return (ec);
		}

		struct pollfd pfd = {.fd = timerfd->kq, .events = POLLIN};
		if (poll(&pfd, 1, -1) < 0) {
			return (errno);
		}
	}
}

ssize_t
timerfd_read(struct timerfd_context *ctx, void *buf, size_t nbytes)
{
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (nbytes < sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	errno_t err;
	uint64_t nr_expired;
	if ((err = timerfd_ctx_read_or_block(&ctx->ctx, &nr_expired,
		 ctx->flags & TFD_NONBLOCK)) != 0) {
		errno = err;
		return -1;
	}

	memcpy(buf, &nr_expired, sizeof(uint64_t));

	return sizeof(uint64_t);
}

int
timerfd_close(struct timerfd_context *ctx)
{
	errno_t ec = timerfd_ctx_terminate(&ctx->ctx);

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
