#include <sys/epoll.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "epoll_shim_ctx.h"
#include "epoll_shim_export.h"
#include "timespec_util.h"
#include "wrap.h"

void epollfd_lock(FileDescription *node);
void epollfd_unlock(FileDescription *node);
void epollfd_remove_fd(FileDescription *node, int kq, int fd);

static errno_t
epollfd_close(FileDescription *node)
{
	return epollfd_ctx_terminate(&node->ctx.epollfd);
}

static struct file_description_vtable const epollfd_vtable = {
	.read_fun = fd_context_default_read,
	.write_fun = fd_context_default_write,
	.close_fun = epollfd_close,
};

void
epollfd_lock(FileDescription *node)
{
	if (node->vtable == &epollfd_vtable) {
		(void)pthread_mutex_lock(&node->mutex);
	}
}

void
epollfd_unlock(FileDescription *node)
{
	if (node->vtable == &epollfd_vtable) {
		(void)pthread_mutex_unlock(&node->mutex);
	}
}

void
epollfd_remove_fd(FileDescription *node, int kq, int fd)
{
	if (node->vtable == &epollfd_vtable) {
		epollfd_ctx_remove_fd(&node->ctx.epollfd, kq, fd);
	}
}

static errno_t
epoll_create_impl(FDContextMapNode **node_out, int flags)
{
	errno_t ec;

	FDContextMapNode *node;
	ec = epoll_shim_ctx_create_node(&epoll_shim_ctx,
	    flags & (O_CLOEXEC | O_NONBLOCK), &node);
	if (ec != 0) {
		return ec;
	}

	FileDescription *desc = node->desc;

	desc->flags = flags & O_NONBLOCK;

	if ((ec = epollfd_ctx_init(&desc->ctx.epollfd)) != 0) {
		goto fail;
	}

	desc->vtable = &epollfd_vtable;
	epoll_shim_ctx_realize_node(&epoll_shim_ctx, node);

	*node_out = node;
	return 0;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(&node);
	return ec;
}

static int
epoll_create_common(int flags)
{
	errno_t ec;
	int oe = errno;

	FDContextMapNode *node;
	ec = epoll_create_impl(&node, flags);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return node->fd;
}

EPOLL_SHIM_EXPORT
int
epoll_create(int size)
{
	if (size <= 0) {
		errno = EINVAL;
		return -1;
	}

	return epoll_create_common(0);
}

EPOLL_SHIM_EXPORT
int
epoll_create1(int flags)
{
	if (flags & ~EPOLL_CLOEXEC) {
		errno = EINVAL;
		return -1;
	}

	_Static_assert(EPOLL_CLOEXEC == O_CLOEXEC, "");

	return epoll_create_common(flags);
}

static errno_t
epoll_ctl_impl(int fd, int op, int fd2, struct epoll_event *ev)
{
	errno_t ec;

	if (!ev && op != EPOLL_CTL_DEL) {
		return EFAULT;
	}

	FileDescription *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &epollfd_vtable) {
		struct stat sb;
		ec = (fd < 0 || fstat(fd, &sb) < 0) ? EBADF : EINVAL;
		goto out;
	}

	(void)pthread_mutex_lock(&node->mutex);
	ec = epollfd_ctx_ctl(&node->ctx.epollfd, fd, op, fd2,
	    fd_as_pollable_node, ev);
	(void)pthread_mutex_unlock(&node->mutex);

out:
	if (node) {
		(void)file_description_unref(&node);
	}
	return ec;
}

EPOLL_SHIM_EXPORT
int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	errno_t ec;
	int oe = errno;

	ec = epoll_ctl_impl(fd, op, fd2, ev);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return 0;
}

static errno_t
epollfd_ctx_wait_or_block(FileDescription *node, int kq, /**/
    struct epoll_event *ev, int cnt, int *actual_cnt,
    struct timespec const *deadline, struct timespec *timeout,
    sigset_t const *sigs)
{
	errno_t ec;

	EpollFDCtx *epollfd = &node->ctx.epollfd;

	for (;;) {
		(void)pthread_mutex_lock(&node->mutex);
		ec = epollfd_ctx_wait(epollfd, kq, ev, cnt, actual_cnt);
		(void)pthread_mutex_unlock(&node->mutex);
		if (ec != 0) {
			return ec;
		}

		if (*actual_cnt ||
		    (timeout && timeout->tv_sec == 0 &&
			timeout->tv_nsec == 0)) {
			return 0;
		}

		(void)pthread_mutex_lock(&node->mutex);

		nfds_t nfds = (nfds_t)(1 + epollfd->poll_fds_size);

		size_t size;
		if (__builtin_mul_overflow(nfds, sizeof(struct pollfd),
			&size)) {
			ec = ENOMEM;
			(void)pthread_mutex_unlock(&node->mutex);
			return ec;
		}

		struct pollfd *pfds = malloc(size);
		if (!pfds) {
			ec = errno;
			(void)pthread_mutex_unlock(&node->mutex);
			return ec;
		}

		epollfd_ctx_fill_pollfds(epollfd, kq, pfds);

		(void)pthread_mutex_lock(&epollfd->nr_polling_threads_mutex);
		++epollfd->nr_polling_threads;
		(void)pthread_mutex_unlock(&epollfd->nr_polling_threads_mutex);

		(void)pthread_mutex_unlock(&node->mutex);

		/*
		 * This surfaced a race condition when
		 * registering/unregistering poll-only fds. The tests should
		 * still succeed if this is enabled.
		 */
#if 0
		usleep(500000);
#endif

		int n = real_ppoll(pfds, nfds, timeout, sigs);
		if (n < 0) {
			ec = errno;
		}

		free(pfds);

		(void)pthread_mutex_lock(&epollfd->nr_polling_threads_mutex);
		--epollfd->nr_polling_threads;
		if (epollfd->nr_polling_threads == 0) {
			(void)pthread_cond_signal(
			    &epollfd->nr_polling_threads_cond);
		}
		(void)pthread_mutex_unlock(&epollfd->nr_polling_threads_mutex);

		if (n < 0) {
			return ec;
		}

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
	}
}

static errno_t
timeout_to_deadline(struct timespec *deadline, struct timespec *timeout, int to)
{
	assert(to >= 0);

	if (to == 0) {
		*deadline = *timeout = (struct timespec) { 0, 0 };
	} else if (to > 0) {
		if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) {
			return errno;
		}
		*timeout = (struct timespec) {
			.tv_sec = to / 1000,
			.tv_nsec = (to % 1000) * 1000000L,
		};
		if (!timespecadd_safe(deadline, timeout, deadline)) {
			return EINVAL;
		}
	}

	return 0;
}

static errno_t
epoll_pwait_impl(int fd, struct epoll_event *ev, int cnt, int to,
    sigset_t const *sigs, int *actual_cnt)
{
	errno_t ec;

	if (cnt < 1 || cnt > (int)(INT_MAX / sizeof(struct epoll_event))) {
		return EINVAL;
	}

	FileDescription *node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &epollfd_vtable) {
		struct stat sb;
		ec = (fd < 0 || fstat(fd, &sb) < 0) ? EBADF : EINVAL;
		goto out;
	}

	struct timespec deadline;
	struct timespec timeout;
	if (to >= 0 &&
	    (ec = timeout_to_deadline(&deadline, &timeout, to)) != 0) {
		goto out;
	}

	ec = epollfd_ctx_wait_or_block(node, fd, ev, cnt, actual_cnt, /**/
	    (to >= 0) ? &deadline : NULL,			      /**/
	    (to >= 0) ? &timeout : NULL,			      /**/
	    sigs);

out:
	if (node) {
		(void)file_description_unref(&node);
	}
	return ec;
}

EPOLL_SHIM_EXPORT
int
epoll_pwait(int fd, struct epoll_event *ev, int cnt, int to,
    sigset_t const *sigs)
{
	errno_t ec;
	int oe = errno;

	int actual_cnt;
	ec = epoll_pwait_impl(fd, ev, cnt, to, sigs, &actual_cnt);
	if (ec != 0) {
		errno = ec;
		return -1;
	}

	errno = oe;
	return actual_cnt;
}

EPOLL_SHIM_EXPORT
int
epoll_wait(int fd, struct epoll_event *ev, int cnt, int to)
{
	return epoll_pwait(fd, ev, cnt, to, NULL);
}
