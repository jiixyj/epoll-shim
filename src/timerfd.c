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
timerfd_ctx_read_or_block(TimerFDCtx *timerfd, uint64_t *value, bool nonblock)
{
	errno_t ec;

	for (;;) {
		ec = timerfd_ctx_read(timerfd, value);
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
	if ((ec = timerfd_ctx_read_or_block(&node->ctx.timerfd, &nr_expired,
		 node->flags & TFD_NONBLOCK)) != 0) {
		return ec;
	}

	memcpy(buf, &nr_expired, sizeof(uint64_t));

	*bytes_transferred = sizeof(uint64_t);
	return 0;
}

static errno_t
timerfd_close(FDContextMapNode *node)
{
	return timerfd_ctx_terminate(&node->ctx.timerfd);
}

static FDContextVTable const timerfd_vtable = {
	.read_fun = timerfd_read,
	.write_fun = fd_context_default_write,
	.close_fun = timerfd_close,
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

	FDContextMapNode *node;
	ec = epoll_shim_ctx_create_node(&epoll_shim_ctx,
	    (flags & TFD_CLOEXEC) != 0, &node);
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

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		return EINVAL;
	}

	FDContextMapNode *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	if ((ec = timerfd_ctx_settime(&node->ctx.timerfd,
		 !!(flags & TFD_TIMER_ABSTIME), /**/
		 new, old)) != 0) {
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

static int
timerfd_gettime_impl(int fd, struct itimerspec *cur)
{
	FDContextMapNode *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	return timerfd_ctx_gettime(&node->ctx.timerfd, cur);
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
