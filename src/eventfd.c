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

#include "epoll_shim_ctx.h"

static errno_t
eventfd_ctx_read_or_block(EventFDCtx *eventfd_ctx, uint64_t *value,
    bool nonblock)
{
	for (;;) {
		errno_t ec = eventfd_ctx_read(eventfd_ctx, value);
		if (nonblock || ec != EAGAIN) {
			return (ec);
		}

		struct pollfd pfd = {.fd = eventfd_ctx->kq_, .events = POLLIN};
		if (poll(&pfd, 1, -1) < 0) {
			return (errno);
		}
	}
}

static errno_t
eventfd_helper_read(FDContextMapNode *node, void *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	if (nbytes != sizeof(uint64_t)) {
		return EINVAL;
	}

	uint64_t value;
	errno_t ec;
	if ((ec = eventfd_ctx_read_or_block(&node->ctx.eventfd, &value,
		 node->flags & EFD_NONBLOCK)) != 0) {
		return ec;
	}

	memcpy(buf, &value, sizeof(value));
	*bytes_transferred = sizeof(value);
	return 0;
}

static errno_t
eventfd_helper_write(FDContextMapNode *node, void const *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	if (nbytes != sizeof(uint64_t)) {
		return EINVAL;
	}

	uint64_t value;
	memcpy(&value, buf, sizeof(uint64_t));

	errno_t ec;
	if ((ec = eventfd_ctx_write(&node->ctx.eventfd, value)) != 0) {
		return ec;
	}

	*bytes_transferred = sizeof(value);
	return 0;
}

static errno_t
eventfd_close(FDContextMapNode *node)
{
	return eventfd_ctx_terminate(&node->ctx.eventfd);
}

static FDContextVTable const eventfd_vtable = {
    .read_fun = eventfd_helper_read,
    .write_fun = eventfd_helper_write,
    .close_fun = eventfd_close,
};

static FDContextMapNode *
eventfd_impl(unsigned int initval, int flags, errno_t *ec)
{
	FDContextMapNode *node;

	if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) {
		*ec = EINVAL;
		return NULL;
	}

	/*
	 * Don't check that EFD_CLOEXEC is set -- but our kqueue based eventfd
	 * will always be CLOEXEC.
	 */

	node = epoll_shim_ctx_create_node(&epoll_shim_ctx, ec);
	if (!node) {
		return NULL;
	}

	node->flags = flags;

	int ctx_flags = 0;
	if (flags & EFD_SEMAPHORE) {
		ctx_flags |= EVENTFD_CTX_FLAG_SEMAPHORE;
	}

	if ((*ec = eventfd_ctx_init(&node->ctx.eventfd, /**/
		 node->fd, initval, ctx_flags)) != 0) {
		goto fail;
	}

	node->vtable = &eventfd_vtable;
	return node;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return NULL;
}

int
eventfd(unsigned int initval, int flags)
{
	FDContextMapNode *node;
	errno_t ec;

	node = eventfd_impl(initval, flags, &ec);
	if (!node) {
		errno = ec;
		return -1;
	}

	return node->fd;
}

int
eventfd_read(int fd, eventfd_t *value)
{
	return (epoll_shim_read(fd, /**/
		    value, sizeof(*value)) == (ssize_t)sizeof(*value))
	    ? 0
	    : -1;
}

int
eventfd_write(int fd, eventfd_t value)
{
	return (epoll_shim_write(fd, /**/
		    &value, sizeof(value)) == (ssize_t)sizeof(value))
	    ? 0
	    : -1;
}
