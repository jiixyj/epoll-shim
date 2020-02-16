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

static errno_t
timerfd_read(FDContextMapNode *node, void *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	if (nbytes < sizeof(uint64_t)) {
		return EINVAL;
	}

	errno_t ec;
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

static FDContextMapNode *
timerfd_create_impl(int clockid, int flags, errno_t *ec)
{
	FDContextMapNode *node;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		*ec = EINVAL;
		return NULL;
	}

	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		*ec = EINVAL;
		return NULL;
	}

	node = epoll_shim_ctx_create_node(&epoll_shim_ctx, ec);
	if (!node) {
		return NULL;
	}

	node->flags = flags;

	if ((*ec = timerfd_ctx_init(&node->ctx.timerfd, /**/
		 node->fd, clockid)) != 0) {
		goto fail;
	}

	node->vtable = &timerfd_vtable;
	return node;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return NULL;
}

int
timerfd_create(int clockid, int flags)
{
	FDContextMapNode *node;
	errno_t ec;

	node = timerfd_create_impl(clockid, flags, &ec);
	if (!node) {
		errno = ec;
		return -1;
	}

	return node->fd;
}

static errno_t
timerfd_settime_impl(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t ec;
	FDContextMapNode *node;

	if (!new) {
		return EFAULT;
	}

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		return EINVAL;
	}

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	if ((ec = timerfd_ctx_settime(&node->ctx.timerfd,
		 (flags & TFD_TIMER_ABSTIME) ? TIMER_ABSTIME : 0, /**/
		 new, old)) != 0) {
		return ec;
	}

	return 0;
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t ec = timerfd_settime_impl(fd, flags, new, old);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}

static int
timerfd_gettime_impl(int fd, struct itimerspec *cur)
{
	FDContextMapNode *node;

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &timerfd_vtable) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb)) ? EBADF : EINVAL;
	}

	return timerfd_ctx_gettime(&node->ctx.timerfd, cur);
}

int
timerfd_gettime(int fd, struct itimerspec *cur)
{
	errno_t ec = timerfd_gettime_impl(fd, cur);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}
