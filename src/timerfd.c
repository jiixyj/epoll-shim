#include <sys/timerfd.h>
#undef read
#undef close

#include <sys/event.h>
#include <sys/select.h>
#include <sys/stat.h>

#include <poll.h>
#include <pthread.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "epoll_shim_ctx.h"
#include "epoll_shim_export.h"

static errno_t
timerfd_ctx_read_or_block(FDContextMapNode *node, uint64_t *value)
{
	errno_t ec;
	TimerFDCtx *timerfd = &node->ctx.timerfd;

	for (;;) {
		(void)pthread_mutex_lock(&node->mutex);
		ec = timerfd_ctx_read(timerfd, value);
		bool nonblock = (node->flags & O_NONBLOCK) != 0;
		(void)pthread_mutex_unlock(&node->mutex);
		if (nonblock && ec == 0 && *value == 0) {
			ec = EAGAIN;
		}
		if (nonblock || ec != EAGAIN) {
			return ec;
		}

		struct pollfd pfd = {
			.fd = timerfd->kq,
			.events = POLLIN,
		};
		if (poll(&pfd, 1, -1) < 0) {
			return errno;
		}
	}
}

static errno_t
timerfd_read(FDContextMapNode *node, void *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	errno_t ec;

	if (nbytes < sizeof(uint64_t)) {
		return EINVAL;
	}

	uint64_t nr_expired;
	if ((ec = timerfd_ctx_read_or_block(node, &nr_expired)) != 0) {
		return ec;
	}

	if (nr_expired == 0) {
		*bytes_transferred = 0;
	} else {
		memcpy(buf, &nr_expired, sizeof(uint64_t));
		*bytes_transferred = sizeof(uint64_t);
	}

	return 0;
}

static errno_t
timerfd_close(FDContextMapNode *node)
{
	return timerfd_ctx_terminate(&node->ctx.timerfd);
}

static void
timerfd_poll(FDContextMapNode *node, uint32_t *revents)
{
	(void)pthread_mutex_lock(&node->mutex);
	timerfd_ctx_poll(&node->ctx.timerfd, revents);
	(void)pthread_mutex_unlock(&node->mutex);
}

static void
timerfd_realtime_change(FDContextMapNode *node)
{
	(void)pthread_mutex_lock(&node->mutex);
	timerfd_ctx_realtime_change(&node->ctx.timerfd);
	(void)pthread_mutex_unlock(&node->mutex);
}

static FDContextVTable const timerfd_vtable = {
	.read_fun = timerfd_read,
	.write_fun = fd_context_default_write,
	.close_fun = timerfd_close,
	.poll_fun = timerfd_poll,
	.realtime_change_fun = timerfd_realtime_change,
};

static errno_t
timerfd_create_impl(FDContextMapNode **node_out, int clockid, int flags)
{
	errno_t ec;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		return EINVAL;
	}

	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		return EINVAL;
	}

	_Static_assert(TFD_CLOEXEC == O_CLOEXEC, "");
	_Static_assert(TFD_NONBLOCK == O_NONBLOCK, "");

	FDContextMapNode *node;
	ec = epoll_shim_ctx_create_node(&epoll_shim_ctx,
	    flags & (O_CLOEXEC | O_NONBLOCK), &node);
	if (ec != 0) {
		return ec;
	}

	node->flags = flags;

	if ((ec = timerfd_ctx_init(&node->ctx.timerfd, /**/
		 node->fd, clockid)) != 0) {
		goto fail;
	}

	node->vtable = &timerfd_vtable;
	*node_out = node;
	return 0;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return ec;
}

EPOLL_SHIM_EXPORT
int
timerfd_create(int clockid, int flags)
{
	errno_t ec;
	int oe = errno;

	FDContextMapNode *node;
	ec = timerfd_create_impl(&node, clockid, flags);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return node->fd;
}

static errno_t
timerfd_settime_impl(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t ec;

	if (!new) {
		return EFAULT;
	}

	if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) {
		return EINVAL;
	}

	FDContextMapNode *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	(void)pthread_mutex_lock(&node->mutex);
	ec = timerfd_ctx_settime(&node->ctx.timerfd,
	    (flags & TFD_TIMER_ABSTIME) != 0,	    /**/
	    (flags & TFD_TIMER_CANCEL_ON_SET) != 0, /**/
	    new, old);
	(void)pthread_mutex_unlock(&node->mutex);
	if (ec != 0) {
		return ec;
	}

	return 0;
}

EPOLL_SHIM_EXPORT
int
timerfd_settime(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t ec;
	int oe = errno;

	ec = timerfd_settime_impl(fd, flags, new, old);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return 0;
}

static errno_t
timerfd_gettime_impl(int fd, struct itimerspec *cur)
{
	errno_t ec;

	FDContextMapNode *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	(void)pthread_mutex_lock(&node->mutex);
	ec = timerfd_ctx_gettime(&node->ctx.timerfd, cur);
	(void)pthread_mutex_unlock(&node->mutex);
	return ec;
}

EPOLL_SHIM_EXPORT
int
timerfd_gettime(int fd, struct itimerspec *cur)
{
	errno_t ec;
	int oe = errno;

	ec = timerfd_gettime_impl(fd, cur);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return 0;
}
