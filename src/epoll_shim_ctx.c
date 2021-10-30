#include "epoll_shim_ctx.h"

#include <sys/event.h>

/* For FIONBIO. */
#include <sys/filio.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "epoll_shim_export.h"
#include "timespec_util.h"
#include "wrap.h"

static void
fd_context_map_node_install_kq(FDContextMapNode *node, int kq)
{
	node->fd = kq;
	node->desc.vtable = NULL;
}

static errno_t
fd_context_map_node_init(FDContextMapNode *node, int kq)
{
	errno_t ec;

	if ((ec = pthread_mutex_init(&node->desc.mutex, NULL)) != 0) {
		return ec;
	}
	fd_context_map_node_install_kq(node, kq);

	return 0;
}

static errno_t
fd_context_map_node_create(FDContextMapNode **node_out, int kq)
{
	errno_t ec;

	FDContextMapNode *node = malloc(sizeof(FDContextMapNode));
	if (!node) {
		return errno;
	}

	if ((ec = fd_context_map_node_init(node, kq)) != 0) {
		free(node);
		return ec;
	}

	*node_out = node;
	return 0;
}

static errno_t
fd_context_map_node_close_ctx(FileDescription *node)
{
	return node->vtable ? node->vtable->close_fun(node) : 0;
}

static errno_t
fd_context_map_node_terminate(FDContextMapNode *node)
{
	errno_t ec = fd_context_map_node_close_ctx(&node->desc);

	errno_t ec_local = pthread_mutex_destroy(&node->desc.mutex);
	ec = ec != 0 ? ec : ec_local;

	if (real_close(node->fd) < 0) {
		ec = ec ? ec : errno;
	}

	return ec;
}

static void
fd_context_map_node_poll(void *arg, uint32_t *revents)
{
	FileDescription *node = arg;
	node->vtable->poll_fun(node, revents);
}

PollableNode
fd_context_map_node_as_pollable_node(FileDescription *node)
{
	if (!node || !node->vtable->poll_fun) {
		return (PollableNode) { NULL, NULL };
	}
	static const struct pollable_node_vtable vtable = {
		.poll_fun = fd_context_map_node_poll,
	};
	return (PollableNode) { node, &vtable };
}

errno_t
fd_context_map_node_destroy(FDContextMapNode *node)
{
	errno_t ec = fd_context_map_node_terminate(node);
	free(node);
	return ec;
}

/**/

errno_t
fd_context_default_read(FileDescription *node, /**/
    void *buf, size_t nbytes, size_t *bytes_transferred)
{
	(void)node;
	(void)buf;
	(void)nbytes;
	(void)bytes_transferred;

	return EINVAL;
}

errno_t
fd_context_default_write(FileDescription *node, /**/
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

static errno_t
epoll_shim_ctx_create_node_impl(EpollShimCtx *epoll_shim_ctx, int kq,
    FDContextMapNode **node_out)
{
	errno_t ec;

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
		(void)fd_context_map_node_close_ctx(&node->desc);
		fd_context_map_node_install_kq(node, kq);
	} else {
		ec = fd_context_map_node_create(&node, kq);
		if (ec != 0) {
			return ec;
		}

		void *colliding_node = RB_INSERT(fd_context_map_,
		    &epoll_shim_ctx->fd_context_map, node);
		(void)colliding_node;
		assert(colliding_node == NULL);
	}

	*node_out = node;
	return 0;
}

errno_t
epoll_shim_ctx_create_node(EpollShimCtx *epoll_shim_ctx, int flags,
    FDContextMapNode **node)
{
	errno_t ec;

	int kq = kqueue1(flags);
	if (kq < 0) {
		return errno;
	}

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	ec = epoll_shim_ctx_create_node_impl(epoll_shim_ctx, kq, node);

	if (ec != 0) {
		(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
		real_close(kq);
	}

	return ec;
}

void
epoll_shim_ctx_realize_node(EpollShimCtx *epoll_shim_ctx,
    FDContextMapNode *node)
{
	(void)node;

	assert(pthread_mutex_trylock(&epoll_shim_ctx->mutex) == EBUSY);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
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

FileDescription *
epoll_shim_ctx_find_node(EpollShimCtx *epoll_shim_ctx, int fd)
{
	FDContextMapNode *node;

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	node = epoll_shim_ctx_find_node_impl(epoll_shim_ctx, fd);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);

	return node != NULL ? &node->desc : NULL;
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
	assert(pthread_mutex_trylock(&epoll_shim_ctx->mutex) == EBUSY);
	RB_REMOVE(fd_context_map_, /**/
	    &epoll_shim_ctx->fd_context_map, node);
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
}

static void
epoll_shim_ctx_for_each_unlocked(EpollShimCtx *epoll_shim_ctx,
    void (*fun)(FileDescription *node))
{
	assert(pthread_mutex_trylock(&epoll_shim_ctx->mutex) == EBUSY);

	FDContextMapNode *node;
	RB_FOREACH (node, fd_context_map_, &epoll_shim_ctx->fd_context_map) {
		fun(&node->desc);
	}
}

#ifndef HAVE_TIMERFD
static void
trigger_realtime_change_notification(FileDescription *node)
{
	if (node->vtable->realtime_change_fun != NULL) {
		node->vtable->realtime_change_fun(node);
	}
}

struct realtime_step_detection_args {
	EpollShimCtx *epoll_shim_ctx;
	uint64_t generation;
	struct timespec monotonic_offset;
};

static void *
realtime_step_detection(void *arg)
{
	struct realtime_step_detection_args *args = arg;
	EpollShimCtx *const epoll_shim_ctx = args->epoll_shim_ctx;
	uint64_t const generation = args->generation;
	struct timespec monotonic_offset = args->monotonic_offset;
	free(args);

	for (;;) {
		(void)nanosleep(&(struct timespec) { .tv_sec = 1 }, NULL);

		struct timespec new_monotonic_offset;
		if (timerfd_ctx_get_monotonic_offset(/**/
			&new_monotonic_offset) != 0) {
			/*
			 * realtime timer step detection is best effort,
			 * so bail out.
			 */
			break;
		}

		(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
		if (epoll_shim_ctx->realtime_step_detector_generation !=
		    generation) {
			(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
			break;
		}
		if (new_monotonic_offset.tv_sec != monotonic_offset.tv_sec ||
		    new_monotonic_offset.tv_nsec != monotonic_offset.tv_nsec) {
			monotonic_offset = new_monotonic_offset;
			epoll_shim_ctx_for_each_unlocked(epoll_shim_ctx,
			    trigger_realtime_change_notification);
		}
		(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
	}

	return NULL;
}

static errno_t
epoll_shim_ctx_start_realtime_step_detection(EpollShimCtx *epoll_shim_ctx)
{
	errno_t ec;

	struct timespec monotonic_offset;
	if ((ec = timerfd_ctx_get_monotonic_offset(&monotonic_offset)) != 0) {
		return ec;
	}

	sigset_t set;
	if (sigfillset(&set) < 0) {
		return errno;
	}

	sigset_t oldset;
	if ((ec = pthread_sigmask(SIG_BLOCK, &set, &oldset)) != 0) {
		return ec;
	}

	struct realtime_step_detection_args *args = malloc(
	    sizeof(struct realtime_step_detection_args));
	if (args == NULL) {
		goto out;
	}
	*args = (struct realtime_step_detection_args) {
		.epoll_shim_ctx = epoll_shim_ctx,
		.generation = epoll_shim_ctx->realtime_step_detector_generation,
		.monotonic_offset = monotonic_offset,
	};

	pthread_t realtime_step_detector;
	if ((ec = pthread_create(&realtime_step_detector, NULL,
		 realtime_step_detection, args)) != 0) {
		free(args);
		goto out;
	}

	(void)pthread_detach(realtime_step_detector);

out:
	(void)pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	return ec;
}

void
epoll_shim_ctx_update_realtime_change_monitoring(EpollShimCtx *epoll_shim_ctx,
    int change)
{
	if (change == 0) {
		return;
	}

	(void)pthread_mutex_lock(&epoll_shim_ctx->mutex);
	uint64_t old_nr_fds = epoll_shim_ctx->nr_fds_for_realtime_step_detector;
	if (change < 0) {
		assert(old_nr_fds >= (uint64_t)-change);

		epoll_shim_ctx->nr_fds_for_realtime_step_detector -=
		    (uint64_t)-change;

		if (epoll_shim_ctx->nr_fds_for_realtime_step_detector == 0) {
			++epoll_shim_ctx->realtime_step_detector_generation;
		}
	} else {
		epoll_shim_ctx->nr_fds_for_realtime_step_detector += /**/
		    (uint64_t)change;

		if (old_nr_fds == 0) {
			/* best effort */
			(void)epoll_shim_ctx_start_realtime_step_detection(
			    epoll_shim_ctx);
		}
	}
	(void)pthread_mutex_unlock(&epoll_shim_ctx->mutex);
}
#endif

/**/

EPOLL_SHIM_EXPORT
int
epoll_shim_close(int fd)
{
	errno_t ec;
	int oe = errno;

	FDContextMapNode *node = /**/
	    epoll_shim_ctx_remove_node(&epoll_shim_ctx, fd);
	if (!node) {
		errno = oe;
		return real_close(fd);
	}

	ec = fd_context_map_node_destroy(node);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return 0;
}

EPOLL_SHIM_EXPORT
ssize_t
epoll_shim_read(int fd, void *buf, size_t nbytes)
{
	errno_t ec;
	int oe = errno;

	FileDescription *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node) {
		errno = oe;
		return real_read(fd, buf, nbytes);
	}

	if (nbytes > SSIZE_MAX) {
		errno = EINVAL;
		return -1;
	}

	size_t bytes_transferred;
	ec = node->vtable->read_fun(node, buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return (ssize_t)bytes_transferred;
}

EPOLL_SHIM_EXPORT
ssize_t
epoll_shim_write(int fd, void const *buf, size_t nbytes)
{
	errno_t ec;
	int oe = errno;

	FileDescription *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node) {
		errno = oe;
		return real_write(fd, buf, nbytes);
	}

	if (nbytes > SSIZE_MAX) {
		errno = EINVAL;
		return -1;
	}

	size_t bytes_transferred;
	ec = node->vtable->write_fun(node, buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return (ssize_t)bytes_transferred;
}

EPOLL_SHIM_EXPORT
int
epoll_shim_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	return epoll_shim_ppoll(fds, nfds,
	    timeout >= 0 ?
		      &(struct timespec) {
		    .tv_sec = timeout / 1000,
		    .tv_nsec = timeout % 1000 * 1000000,
		} :
		      NULL,
	    NULL);
}

static errno_t
epoll_shim_ppoll_deadline(struct pollfd *fds, nfds_t nfds,
    struct timespec const *deadline, struct timespec *timeout,
    sigset_t const *sigmask, int *n_out)
{

retry:;
	if (fds != NULL) {
		(void)pthread_mutex_lock(&epoll_shim_ctx.mutex);
		for (nfds_t i = 0; i < nfds; ++i) {
			FDContextMapNode *node = epoll_shim_ctx_find_node_impl(
			    &epoll_shim_ctx, fds[i].fd);
			if (!node) {
				continue;
			}
			if (node->desc.vtable->poll_fun != NULL) {
				node->desc.vtable->poll_fun(&node->desc, NULL);
			}
		}
		(void)pthread_mutex_unlock(&epoll_shim_ctx.mutex);
	}

	int n = real_ppoll(fds, nfds, timeout, sigmask);
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
		if (node->desc.vtable->poll_fun != NULL) {
			uint32_t revents;
			node->desc.vtable->poll_fun(&node->desc, &revents);
			fds[i].revents = (short)revents;
			if (fds[i].revents == 0) {
				--n;
			}
		}
	}
	(void)pthread_mutex_unlock(&epoll_shim_ctx.mutex);

	if (n == 0 &&
	    !(timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0)) {
		if (timeout) {
			struct timespec current_time;

			if (clock_gettime(CLOCK_MONOTONIC, /**/
				&current_time) < 0) {
				return errno;
			}

			timespecsub(deadline, &current_time, timeout);
			if (timeout->tv_sec < 0) {
				timeout->tv_sec = 0;
				timeout->tv_nsec = 0;
			}
		}
		goto retry;
	}

	*n_out = n;
	return 0;
}

static errno_t
epoll_shim_ppoll_impl(struct pollfd *fds, nfds_t nfds,
    struct timespec const *tmo_p, sigset_t const *sigmask, int *n)
{
	struct timespec deadline;
	struct timespec timeout;

	if (tmo_p) {
		if (tmo_p->tv_sec == 0 && tmo_p->tv_nsec == 0) {
			deadline = timeout = (struct timespec) { 0, 0 };
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

			memcpy(&timeout, tmo_p, sizeof(struct timespec));
		}
	}

	return epoll_shim_ppoll_deadline(fds, nfds, /**/
	    tmo_p ? &deadline : NULL,		    /**/
	    tmo_p ? &timeout : NULL,		    /**/
	    sigmask, n);
}

EPOLL_SHIM_EXPORT
int
epoll_shim_ppoll(struct pollfd *fds, nfds_t nfds, struct timespec const *tmo_p,
    sigset_t const *sigmask)
{
	errno_t ec;
	int oe = errno;

	int n;
	ec = epoll_shim_ppoll_impl(fds, nfds, tmo_p, sigmask, &n);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return n;
}

EPOLL_SHIM_EXPORT
int
epoll_shim_fcntl(int fd, int cmd, ...)
{
	errno_t ec;
	int oe = errno;

	va_list ap;

	if (cmd != F_SETFL) {
		va_start(ap, cmd);
		void *arg = va_arg(ap, void *);
		va_end(ap);

		errno = oe;
		return real_fcntl(fd, cmd, arg);
	}

	int arg;

	va_start(ap, cmd);
	arg = va_arg(ap, int);
	va_end(ap);

	FileDescription *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node) {
		errno = oe;
		return real_fcntl(fd, F_SETFL, arg);
	}

	(void)pthread_mutex_lock(&node->mutex);
	{
		int opt = (arg & O_NONBLOCK) ? 1 : 0;
		ec = ioctl(fd, FIONBIO, &opt) < 0 ? errno : 0;
		ec = (ec == ENOTTY) ? 0 : ec;

		if (ec == 0) {
			node->flags = arg & O_NONBLOCK;
		}
	}
	(void)pthread_mutex_unlock(&node->mutex);

	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return 0;
}
