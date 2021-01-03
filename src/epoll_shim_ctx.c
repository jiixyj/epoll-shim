#include "epoll_shim_ctx.h"

#include <sys/event.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include <poll.h>
#include <signal.h>
#include <time.h>

#include "timespec_util.h"

static void
fd_context_map_node_init(FDContextMapNode *node, int kq)
{
	node->fd = kq;
	node->vtable = NULL;
}

static FDContextMapNode *
fd_context_map_node_create(int kq, errno_t *ec)
{
	FDContextMapNode *node;

	node = malloc(sizeof(FDContextMapNode));
	if (!node) {
		*ec = errno;
		return NULL;
	}

	fd_context_map_node_init(node, kq);
	return node;
}

static errno_t
fd_context_map_node_terminate(FDContextMapNode *node, bool close_fd)
{
	errno_t ec = node->vtable ? node->vtable->close_fun(node) : 0;

	if (close_fd && close(node->fd) < 0) {
		ec = ec ? ec : errno;
	}

	return ec;
}

static void
fd_context_map_node_poll(void *arg, uint32_t *revents)
{
	FDContextMapNode *node = arg;
	node->vtable->poll_fun(node, revents);
}

PollableNode
fd_context_map_node_as_pollable_node(FDContextMapNode *node)
{
	if (!node || !node->vtable->poll_fun) {
		return (PollableNode){NULL, NULL};
	}
	static const struct pollable_node_vtable vtable = {
	    .poll_fun = fd_context_map_node_poll,
	};
	return (PollableNode){node, &vtable};
}

errno_t
fd_context_map_node_destroy(FDContextMapNode *node)
{
	errno_t ec = fd_context_map_node_terminate(node, true);
	free(node);
	return ec;
}

/**/

errno_t
fd_context_default_read(FDContextMapNode *node, /**/
    void *buf, size_t nbytes, size_t *bytes_transferred)
{
	(void)node;
	(void)buf;
	(void)nbytes;
	(void)bytes_transferred;

	return EINVAL;
}

errno_t
fd_context_default_write(FDContextMapNode *node, /**/
    void const *buf, size_t nbytes, size_t *bytes_transferred)
{
	(void)node;
	(void)buf;
	(void)nbytes;
	(void)bytes_transferred;

	return EINVAL;
}

/**/

static int
fd_context_map_node_cmp(FDContextMapNode *e1, FDContextMapNode *e2)
{
	return (e1->fd < e2->fd) ? -1 : (e1->fd > e2->fd);
}

RB_PROTOTYPE_STATIC(fd_context_map_, fd_context_map_node_, entry,
    fd_context_map_node_cmp);
RB_GENERATE_STATIC(fd_context_map_, fd_context_map_node_, entry,
    fd_context_map_node_cmp);

EpollShimCtx epoll_shim_ctx = {
    .fd_context_map = RB_INITIALIZER(&fd_context_map),
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static FDContextMapNode *
epoll_shim_ctx_create_node_impl(EpollShimCtx *epoll_shim_ctx, int kq,
    errno_t *ec)
{
	FDContextMapNode *node;

	{
		FDContextMapNode find;
		find.fd = kq;

		node = RB_FIND(fd_context_map_, /**/
		    &epoll_shim_ctx->fd_context_map, &find);
	}

	if (node) {
		/*
		 * If we get here, someone must have already closed the old fd
		 * with a normal 'close()' call, i.e. not with our
		 * 'epoll_shim_close()' wrapper. The fd inside the node
		 * refers now to the new kq we are currently creating. We
		 * must not close it, but we must clean up the old context
		 * object!
		 */
		(void)fd_context_map_node_terminate(node, false);
		fd_context_map_node_init(node, kq);
	} else {
		node = fd_context_map_node_create(kq, ec);
		if (!node) {
			return NULL;
		}

		void *colliding_node = RB_INSERT(fd_context_map_,
		    &epoll_shim_ctx->fd_context_map, node);
		(void)colliding_node;
		assert(colliding_node == NULL);
	}

	return node;
}

FDContextMapNode *
epoll_shim_ctx_create_node(EpollShimCtx *epoll_shim_ctx, errno_t *ec)
{
	FDContextMapNode *node;

	int kq = kqueue();
	if (kq < 0) {
		*ec = errno;
		return NULL;
	}

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	node = epoll_shim_ctx_create_node_impl(epoll_shim_ctx, kq, ec);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);

	if (!node) {
		close(kq);
	}

	return node;
}

static FDContextMapNode *
epoll_shim_ctx_find_node_impl(EpollShimCtx *epoll_shim_ctx, int fd)
{
	FDContextMapNode *node;

	FDContextMapNode find;
	find.fd = fd;

	node = RB_FIND(fd_context_map_, /**/
	    &epoll_shim_ctx->fd_context_map, &find);

	return node;
}

FDContextMapNode *
epoll_shim_ctx_find_node(EpollShimCtx *epoll_shim_ctx, int fd)
{
	FDContextMapNode *node;

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	node = epoll_shim_ctx_find_node_impl(epoll_shim_ctx, fd);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);

	return node;
}

FDContextMapNode *
epoll_shim_ctx_remove_node(EpollShimCtx *epoll_shim_ctx, int fd)
{
	FDContextMapNode *node;

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	node = epoll_shim_ctx_find_node_impl(epoll_shim_ctx, fd);
	if (node) {
		RB_REMOVE(fd_context_map_, /**/
		    &epoll_shim_ctx->fd_context_map, node);
	}
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);

	return node;
}

void
epoll_shim_ctx_remove_node_explicit(EpollShimCtx *epoll_shim_ctx,
    FDContextMapNode *node)
{
	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	RB_REMOVE(fd_context_map_, /**/
	    &epoll_shim_ctx->fd_context_map, node);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
}

/**/

int
epoll_shim_close(int fd)
{
	FDContextMapNode *node;

	node = epoll_shim_ctx_remove_node(&epoll_shim_ctx, fd);
	if (!node) {
		return close(fd);
	}

	errno_t ec = fd_context_map_node_destroy(node);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}

ssize_t
epoll_shim_read(int fd, void *buf, size_t nbytes)
{
	FDContextMapNode *node;

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node) {
		return read(fd, buf, nbytes);
	}

	if (nbytes > SSIZE_MAX) {
		errno = EINVAL;
		return -1;
	}

	size_t bytes_transferred;
	errno_t ec = node->vtable->read_fun(node, /**/
	    buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)bytes_transferred;
}

ssize_t
epoll_shim_write(int fd, void const *buf, size_t nbytes)
{
	FDContextMapNode *node;

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node) {
		return write(fd, buf, nbytes);
	}

	if (nbytes > SSIZE_MAX) {
		errno = EINVAL;
		return -1;
	}

	size_t bytes_transferred;
	errno_t ec = node->vtable->write_fun(node, /**/
	    buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	return (ssize_t)bytes_transferred;
}

int
epoll_shim_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	return epoll_shim_ppoll(fds, nfds, timeout >= 0 ? &(struct timespec) {
		.tv_sec = timeout / 1000,
		.tv_nsec = timeout % 1000 * 1000000,
	} : NULL, NULL);
}

static errno_t
epoll_shim_ppoll_deadline(struct pollfd *fds, nfds_t nfds,
    struct timespec const *deadline, sigset_t const *sigmask, int *n_out)
{
	struct timespec timeout;

retry:
	if (deadline) {
		if (deadline->tv_sec == 0 && deadline->tv_nsec == 0) {
			timeout.tv_sec = timeout.tv_nsec = 0;
		} else {
			struct timespec current_time;

			if (clock_gettime(CLOCK_MONOTONIC, /**/
				&current_time) < 0) {
				return errno;
			}

			timespecsub(deadline, &current_time, &timeout);
			if (timeout.tv_sec < 0) {
				timeout.tv_sec = 0;
				timeout.tv_nsec = 0;
			}
		}
	}

	if (fds != NULL) {
		(void)pthread_mutex_lock(&epoll_shim_ctx.mutex);
		for (nfds_t i = 0; i < nfds; ++i) {
			FDContextMapNode *node = epoll_shim_ctx_find_node_impl(
			    &epoll_shim_ctx, fds[i].fd);
			if (!node) {
				continue;
			}
			if (node->vtable->poll_fun != NULL) {
				node->vtable->poll_fun(node, NULL);
			}
		}
		(void)pthread_mutex_unlock(&epoll_shim_ctx.mutex);
	}

	int n = ppoll(fds, nfds, deadline ? &timeout : NULL, sigmask);
	if (n < 0) {
		return errno;
	}
	if (n == 0) {
		*n_out = 0;
		return 0;
	}

	(void)pthread_mutex_lock(&epoll_shim_ctx.mutex);
	for (nfds_t i = 0; i < nfds; ++i) {
		if (fds[i].revents == 0) {
			continue;
		}

		FDContextMapNode *node =
		    epoll_shim_ctx_find_node_impl(&epoll_shim_ctx, fds[i].fd);
		if (!node) {
			continue;
		}
		if (node->vtable->poll_fun != NULL) {
			uint32_t revents;
			node->vtable->poll_fun(node, &revents);
			fds[i].revents = revents;
			if (fds[i].revents == 0) {
				--n;
			}
		}
	}
	(void)pthread_mutex_unlock(&epoll_shim_ctx.mutex);

	if (n == 0) {
		goto retry;
	}

	*n_out = n;
	return 0;
}

static errno_t
epoll_shim_ppoll_impl(struct pollfd *fds, nfds_t nfds,
    struct timespec const *tmo_p, sigset_t const *sigmask, int *n)
{
	errno_t ec;

	struct timespec deadline;
	struct timespec const *deadline_p;

	if (!tmo_p) {
		deadline_p = NULL;
	} else if (tmo_p->tv_sec == 0 && tmo_p->tv_nsec == 0) {
		deadline.tv_sec = 0;
		deadline.tv_nsec = 0;
		deadline_p = &deadline;
	} else {
		if (!timespec_is_valid(tmo_p)) {
			return EINVAL;
		}

		if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) {
			return errno;
		}

		if (!timespecadd_safe(&deadline, tmo_p, &deadline)) {
			return EINVAL;
		}
		deadline_p = &deadline;
	}

	return epoll_shim_ppoll_deadline(fds, nfds, deadline_p, sigmask, n);
}

int
epoll_shim_ppoll(struct pollfd *fds, nfds_t nfds, struct timespec const *tmo_p,
    sigset_t const *sigmask)
{
	int n;
	errno_t ec = epoll_shim_ppoll_impl(fds, nfds, tmo_p, sigmask, &n);
	if (ec != 0) {
		errno = ec;
		return -1;
	}
	return n;
}
