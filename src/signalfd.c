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

#include "epoll_shim_ctx.h"

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

static errno_t
signalfd_read(FDContextMapNode *node, void *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	// TODO(jan): fix this to read multiple signals
	if (nbytes != sizeof(struct signalfd_siginfo)) {
		return EINVAL;
	}

	uint32_t signo;
	errno_t ec;
	if ((ec = signalfd_ctx_read_or_block(&node->ctx.signalfd, &signo,
		 node->flags & SFD_NONBLOCK)) != 0) {
		return ec;
	}

	struct signalfd_siginfo siginfo = {.ssi_signo = signo};
	memcpy(buf, &siginfo, sizeof(siginfo));

	*bytes_transferred = sizeof(siginfo);
	return 0;
}

static errno_t
signalfd_close(FDContextMapNode *node)
{
	return signalfd_ctx_terminate(&node->ctx.signalfd);
}

static FDContextVTable const signalfd_vtable = {
    .read_fun = signalfd_read,
    .write_fun = fd_context_default_write,
    .close_fun = signalfd_close,
};

static FDContextMapNode *
signalfd_impl(int fd, const sigset_t *sigs, int flags, errno_t *ec)
{
	FDContextMapNode *node;

	if (fd != -1) {
		*ec = EINVAL;
		return NULL;
	}

	if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) {
		*ec = EINVAL;
		return NULL;
	}

	node = epoll_shim_ctx_create_node(&epoll_shim_ctx, ec);
	if (!node) {
		return NULL;
	}

	node->flags = flags;

	if ((*ec = signalfd_ctx_init(&node->ctx.signalfd, /**/
		 node->fd, sigs)) != 0) {
		goto fail;
	}

	node->vtable = &signalfd_vtable;
	return node;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return NULL;
}

int
signalfd(int fd, const sigset_t *sigs, int flags)
{
	FDContextMapNode *node;
	errno_t ec;

	node = signalfd_impl(fd, sigs, flags, &ec);
	if (!node) {
		errno = ec;
		return -1;
	}

	return node->fd;
}
