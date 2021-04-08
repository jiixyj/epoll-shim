#include <sys/signalfd.h>
#undef read
#undef close
#undef poll
#undef ppoll

#include <sys/types.h>

#include <sys/event.h>
#include <sys/stat.h>

#include <assert.h>
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
#include "epoll_shim_export.h"

static errno_t
signalfd_ctx_read_or_block(FDContextMapNode *node, SignalFDCtxSiginfo *siginfo,
    bool force_nonblock)
{
	errno_t ec;
	SignalFDCtx *signalfd_ctx = &node->ctx.signalfd;

	for (;;) {
		(void)pthread_mutex_lock(&node->mutex);
		ec = signalfd_ctx_read(signalfd_ctx, siginfo);
		bool nonblock = force_nonblock ||
		    (node->flags & O_NONBLOCK) != 0;
		(void)pthread_mutex_unlock(&node->mutex);
		if (nonblock || (ec != EAGAIN && ec != EWOULDBLOCK)) {
			return ec;
		}

		struct pollfd pfd = {
			.fd = signalfd_ctx->kq,
			.events = POLLIN,
		};
		if (poll(&pfd, 1, -1) < 0) {
			return errno;
		}
	}
}

static errno_t
signalfd_read(FDContextMapNode *node, void *buf, size_t nbytes,
    size_t *bytes_transferred)
{
	errno_t ec;

	if (nbytes < sizeof(struct signalfd_siginfo)) {
		return EINVAL;
	}

	bool force_nonblock = false;
	size_t bytes_transferred_local = 0;

	while (nbytes >= sizeof(struct signalfd_siginfo)) {
		_Static_assert(sizeof(struct signalfd_siginfo) ==
			sizeof(SignalFDCtxSiginfo),
		    "");

		SignalFDCtxSiginfo siginfo;
		memset(&siginfo, 0, sizeof(siginfo));

		if ((ec = signalfd_ctx_read_or_block(node, &siginfo,
			 force_nonblock)) != 0) {
			break;
		}

		memcpy(buf, &siginfo, sizeof(siginfo));
		bytes_transferred_local += sizeof(siginfo);

		force_nonblock = true;
		nbytes -= sizeof(siginfo);
		buf = ((unsigned char *)buf) + sizeof(siginfo);
	}

	if (bytes_transferred_local > 0) {
		ec = 0;
	}

	*bytes_transferred = bytes_transferred_local;
	return ec;
}

static errno_t
signalfd_close(FDContextMapNode *node)
{
	return signalfd_ctx_terminate(&node->ctx.signalfd);
}

static void
signalfd_poll(FDContextMapNode *node, uint32_t *revents)
{
	(void)pthread_mutex_lock(&node->mutex);
	signalfd_ctx_poll(&node->ctx.signalfd, revents);
	(void)pthread_mutex_unlock(&node->mutex);
}

static FDContextVTable const signalfd_vtable = {
	.read_fun = signalfd_read,
	.write_fun = fd_context_default_write,
	.close_fun = signalfd_close,
	.poll_fun = signalfd_poll,
};

static errno_t
signalfd_impl(FDContextMapNode **node_out, int fd, const sigset_t *sigs,
    int flags)
{
	errno_t ec;

	if (sigs == NULL || (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC))) {
		return EINVAL;
	}

	if (fd != -1) {
		struct stat sb;
		return (fd < 0 || fstat(fd, &sb) < 0) ? EBADF : EINVAL;
	}

	_Static_assert(SFD_CLOEXEC == O_CLOEXEC, "");
	_Static_assert(SFD_NONBLOCK == O_NONBLOCK, "");

	FDContextMapNode *node;
	ec = epoll_shim_ctx_create_node(&epoll_shim_ctx,
	    flags & (O_CLOEXEC | O_NONBLOCK), &node);
	if (ec != 0) {
		return ec;
	}

	node->flags = flags & O_NONBLOCK;

	if ((ec = signalfd_ctx_init(&node->ctx.signalfd, /**/
		 node->fd, sigs)) != 0) {
		goto fail;
	}

	node->vtable = &signalfd_vtable;
	(void)pthread_mutex_unlock(&epoll_shim_ctx.mutex);

	*node_out = node;
	return 0;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return ec;
}

EPOLL_SHIM_EXPORT
int
signalfd(int fd, const sigset_t *sigs, int flags)
{
	errno_t ec;
	int oe = errno;

	FDContextMapNode *node;
	ec = signalfd_impl(&node, fd, sigs, flags);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return node->fd;
}
