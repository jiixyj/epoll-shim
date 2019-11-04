#include <sys/epoll.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>

#include "epoll_shim_ctx.h"

static errno_t
epollfd_close(FDContextMapNode *node)
{
	return epollfd_ctx_terminate(&node->ctx.epollfd);
}

static FDContextVTable const epollfd_vtable = {
    .read_fun = fd_context_default_read,
    .write_fun = fd_context_default_write,
    .close_fun = epollfd_close,
};

static FDContextMapNode *
epoll_create_impl(errno_t *ec)
{
	FDContextMapNode *node;

	node = epoll_shim_ctx_create_node(&epoll_shim_ctx, ec);
	if (!node) {
		return NULL;
	}

	node->flags = 0;

	if ((*ec = epollfd_ctx_init(&node->ctx.epollfd, /**/
		 node->fd)) != 0) {
		goto fail;
	}

	node->vtable = &epollfd_vtable;
	return node;

fail:
	epoll_shim_ctx_remove_node_explicit(&epoll_shim_ctx, node);
	(void)fd_context_map_node_destroy(node);
	return NULL;
}

static int
epoll_create_common(void)
{
	FDContextMapNode *node;
	errno_t ec;

	node = epoll_create_impl(&ec);
	if (!node) {
		errno = ec;
		return -1;
	}

	return node->fd;
}

int
epoll_create(int size)
{
	if (size <= 0) {
		errno = EINVAL;
		return -1;
	}

	return epoll_create_common();
}

int
epoll_create1(int flags)
{
	if (flags & ~EPOLL_CLOEXEC) {
		errno = EINVAL;
		return -1;
	}

	return epoll_create_common();
}

int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	errno_t ec;
	FDContextMapNode *node;

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &epollfd_vtable) {
		errno = EINVAL;
		return -1;
	}

	if ((ec = epollfd_ctx_ctl(&node->ctx.epollfd, op, fd2, ev)) != 0) {
		errno = ec;
		return -1;
	}

	return 0;
}

#if 0
int
epoll_pwait(
    int fd, struct epoll_event *ev, int cnt, int to, const sigset_t *sigs)
{
	int r = __syscall(SYS_epoll_pwait, fd, ev, cnt, to, sigs, _NSIG / 8);
#ifdef SYS_epoll_wait
	if (r == -ENOSYS && !sigs)
		r = __syscall(SYS_epoll_wait, fd, ev, cnt, to);
#endif
	return __syscall_ret(r);
}
#endif

static errno_t
epollfd_ctx_wait_or_block(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt, int to)
{
	struct timespec deadline;

	if (to > 0) {
		if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) {
			return errno;
		}

		if (__builtin_add_overflow(deadline.tv_sec, to / 1000 + 1,
			&deadline.tv_sec)) {
			return EINVAL;
		}
		deadline.tv_sec -= 1;

		deadline.tv_nsec += (to % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_nsec -= 1000000000;
			deadline.tv_sec += 1;
		}
	}

	for (;;) {
		errno_t ec = epollfd_ctx_wait(epollfd, ev, cnt, actual_cnt);
		if (ec || *actual_cnt || to == 0) {
			return ec;
		}

		struct timespec current_time;
		struct timespec timeout;

		if (to > 0) {
			if (clock_gettime(CLOCK_MONOTONIC, /**/
				&current_time) < 0) {
				return errno;
			}

			timespecsub(&deadline, &current_time, &timeout);
			if (timeout.tv_sec < 0) {
				timeout.tv_sec = 0;
				timeout.tv_nsec = 0;
			}

			to = (int)((timeout.tv_sec * 1000) +
			    (timeout.tv_nsec / 1000000) +
			    !!(timeout.tv_nsec % 1000000));

			if (to == 0) {
				continue;
			}
		}

		assert(to != 0);

		/*
		 * We should add a notification mechanism when a new poll-only
		 * fd gets registered when this thread sleeps...
		 */
		struct pollfd pfds[2];
		(void)pthread_mutex_lock(&epollfd->mutex);
		pfds[0] = epollfd->pfds[0];
		pfds[1] = epollfd->pfds[1];
		(void)pthread_mutex_unlock(&epollfd->mutex);

		if (poll(pfds, 2, MAX(to, -1)) < 0) {
			return errno;
		}
	}
}

int
epoll_wait(int fd, struct epoll_event *ev, int cnt, int to)
{
	errno_t ec;
	FDContextMapNode *node;

	node = epoll_shim_ctx_find_node(&epoll_shim_ctx, fd);
	if (!node || node->vtable != &epollfd_vtable) {
		errno = EINVAL;
		return -1;
	}

	int actual_cnt;
	if ((ec = epollfd_ctx_wait_or_block(&node->ctx.epollfd, ev, cnt,
		 &actual_cnt, to)) != 0) {
		errno = ec;
		return -1;
	}

	return actual_cnt;
}
