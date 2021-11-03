#include "epoll_shim_ctx.h"

#include <sys/event.h>

/* For FIONBIO. */
#include <sys/filio.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
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

static errno_t
file_description_init(FileDescription *desc)
{
	errno_t ec;

	*desc = (FileDescription) {};

	if ((ec = pthread_mutex_init(&desc->mutex, NULL)) != 0) {
		return ec;
	}

	atomic_init(&desc->refcount, 1);
	return 0;
}

static errno_t
file_description_create(FileDescription **desc_out)
{
	errno_t ec;

	FileDescription *desc = malloc(sizeof(FileDescription));
	if (!desc) {
		return errno;
	}

	if ((ec = file_description_init(desc)) != 0) {
		free(desc);
		return ec;
	}

	*desc_out = desc;
	return 0;
}

static void
file_description_ref(FileDescription *desc)
{
	assert(desc->refcount > 0);

	atomic_fetch_add_explicit(&desc->refcount, 1, memory_order_relaxed);
}

static errno_t
file_description_terminate(FileDescription *desc)
{
	errno_t ec = 0;

	{
		errno_t ec_local = desc->vtable ?
			  desc->vtable->close_fun(desc) :
			  0;
		ec = ec != 0 ? ec : ec_local;
	}

	{
		errno_t ec_local = pthread_mutex_destroy(&desc->mutex);
		ec = ec != 0 ? ec : ec_local;
	}

	return ec;
}

static errno_t
file_description_destroy(FileDescription **desc)
{
	errno_t ec = file_description_terminate(*desc);
	free(*desc);
	return ec;
}

errno_t
file_description_unref(FileDescription **desc)
{
	errno_t ec = 0;

	assert((*desc)->refcount > 0);

	if (atomic_fetch_sub_explicit(&(*desc)->refcount, 1,
		memory_order_release) == 1) {
		atomic_thread_fence(memory_order_acquire);
		ec = file_description_destroy(desc);
		*desc = NULL;
	}
	return ec;
}

/**/

static void
fd_poll(void *arg, uint32_t *revents)
{
	int *fd = arg;

	FileDescription *desc = epoll_shim_ctx_find_node(&epoll_shim_ctx, *fd);
	if (!desc || !desc->vtable->poll_fun) {
		goto out;
	}

	desc->vtable->poll_fun(desc, *fd, revents);

out:
	if (desc) {
		(void)file_description_unref(&desc);
	}
}

PollableNode
fd_as_pollable_node(int *fd)
{
	static const struct pollable_node_vtable vtable = {
		.poll_fun = fd_poll,
	};
	return (PollableNode) { fd, &vtable };
}

/**/

errno_t
fd_context_default_read(FileDescription *node, int kq, /**/
    void *buf, size_t nbytes, size_t *bytes_transferred)
{
	(void)node;
	(void)kq;
	(void)buf;
	(void)nbytes;
	(void)bytes_transferred;

	return EINVAL;
}

errno_t
fd_context_default_write(FileDescription *node, int kq, /**/
    void const *buf, size_t nbytes, size_t *bytes_transferred)
{
	(void)node;
	(void)kq;
	(void)buf;
	(void)nbytes;
	(void)bytes_transferred;

	return EINVAL;
}

/**/

EpollShimCtx epoll_shim_ctx = {
	.step_detector_mutex = PTHREAD_MUTEX_INITIALIZER,
};

__attribute__((constructor)) static void
epoll_shim_ctx_initialize(void)
{
	rwlock_init(&epoll_shim_ctx.rwlock);
}

/**/

errno_t
epoll_shim_ctx_create_node(EpollShimCtx *epoll_shim_ctx, int flags, /**/
    int *fd, FileDescription **node)
{
	errno_t ec = 0;

	rwlock_lock_write(&epoll_shim_ctx->rwlock);

	int kq = kqueue1(flags);
	if (kq < 0) {
		ec = errno;
		goto out_kqueue;
	}

	unsigned int open_files_length = epoll_shim_ctx->open_files_length;

	while (open_files_length <= (unsigned int)kq) {
		unsigned int space_needed = 32;
		while (space_needed <= (unsigned int)kq) {
			space_needed <<= 1;
		}

		size_t size;
		if (__builtin_mul_overflow(space_needed,
			sizeof(FileDescription *), &size)) {
			ec = ENOMEM;
			goto out;
		}

		FileDescription **new_files =
		    realloc(epoll_shim_ctx->open_files, size);
		if (!new_files) {
			ec = errno;
			goto out;
		}

		size_t old_size = open_files_length * sizeof(FileDescription *);
		memset(&new_files[open_files_length], 0, size - old_size);

		epoll_shim_ctx->open_files = new_files;
		epoll_shim_ctx->open_files_length = space_needed;
		break;
	}

	if (epoll_shim_ctx->open_files[kq] != NULL) {
		/*
		 * If we get here, someone must have already closed the old fd
		 * with a normal 'close()' call, i.e. not with our
		 * 'epoll_shim_close()' wrapper.
		 */
		(void)file_description_unref(&epoll_shim_ctx->open_files[kq]);
		epoll_shim_ctx->open_files[kq] = NULL;
	}

	ec = file_description_create(node);
	if (ec != 0) {
		goto out;
	}

	*fd = kq;

out:
	if (ec != 0) {
		real_close(kq);
	out_kqueue:
		rwlock_unlock_write(&epoll_shim_ctx->rwlock);
	}

	return ec;
}

void
epoll_shim_ctx_install_node(EpollShimCtx *epoll_shim_ctx, /**/
    int fd, FileDescription *node)
{
	assert((unsigned int)fd < epoll_shim_ctx->open_files_length);
	epoll_shim_ctx->open_files[fd] = node;
	rwlock_unlock_write(&epoll_shim_ctx->rwlock);
}

static FileDescription *
epoll_shim_ctx_find_node_impl(EpollShimCtx *epoll_shim_ctx, int fd)
{
	if (fd < 0) {
		return NULL;
	}
	return (unsigned int)fd < epoll_shim_ctx->open_files_length ?
		  epoll_shim_ctx->open_files[fd] :
		  NULL;
}

FileDescription *
epoll_shim_ctx_find_node(EpollShimCtx *epoll_shim_ctx, int fd)
{
	if (fd < 0) {
		return NULL;
	}

	FileDescription *desc;

	rwlock_lock_read(&epoll_shim_ctx->rwlock);
	desc = epoll_shim_ctx_find_node_impl(epoll_shim_ctx, fd);
	if (desc != NULL) {
		file_description_ref(desc);
	}
	rwlock_unlock_read(&epoll_shim_ctx->rwlock);

	return desc;
}

static void
epoll_shim_ctx_for_each_unlocked(EpollShimCtx *epoll_shim_ctx,
    void (*fun)(FileDescription *node, int kq, void *arg), void *arg)
{
	for (unsigned int i = 0;
	     i < epoll_shim_ctx->open_files_length && i <= INT_MAX; ++i) {
		FileDescription *desc = epoll_shim_ctx->open_files[i];
		if (!desc) {
			continue;
		}

		fun(desc, (int)i, arg);
	}
}

void epollfd_lock(FileDescription *node);
void epollfd_unlock(FileDescription *node);
void epollfd_remove_fd(FileDescription *node, int kq, int fd);
static void
remove_node_lock_epollfd(FileDescription *node, int kq, void *arg)
{
	(void)kq;
	(void)arg;

	epollfd_lock(node);
}
static void
remove_node_remove_fd_from_epollfd(FileDescription *node, int kq, void *arg)
{
	epollfd_remove_fd(node, kq, *(int *)arg);
}
static void
remove_node_unlock_epollfd(FileDescription *node, int kq, void *arg)
{
	(void)kq;
	(void)arg;

	epollfd_unlock(node);
}
static errno_t
epoll_shim_ctx_remove_node(EpollShimCtx *epoll_shim_ctx, int fd)
{
	errno_t ec = 0;
	FileDescription *node;

	rwlock_lock_write(&epoll_shim_ctx->rwlock);

	node = epoll_shim_ctx_find_node_impl(epoll_shim_ctx, fd);
	if (node) {
		epoll_shim_ctx->open_files[fd] = NULL;
		ec = file_description_unref(&node);
	}

	rwlock_downgrade(&epoll_shim_ctx->rwlock);

	epoll_shim_ctx_for_each_unlocked(epoll_shim_ctx,
	    remove_node_lock_epollfd, NULL);
	epoll_shim_ctx_for_each_unlocked(epoll_shim_ctx,
	    remove_node_remove_fd_from_epollfd, &fd);
	{
		errno_t ec_local = real_close(fd) < 0 ? errno : 0;
		ec = ec != 0 ? ec : ec_local;
	}
	epoll_shim_ctx_for_each_unlocked(epoll_shim_ctx,
	    remove_node_unlock_epollfd, NULL);

	rwlock_unlock_read(&epoll_shim_ctx->rwlock);

	return ec;
}

void
epoll_shim_ctx_drop_node(EpollShimCtx *epoll_shim_ctx, /**/
    int fd, FileDescription *node)
{
	(void)file_description_unref(&node);
	(void)real_close(fd);
	rwlock_unlock_write(&epoll_shim_ctx->rwlock);
}

#ifndef HAVE_TIMERFD
static void
trigger_realtime_change_notification(FileDescription *node, int kq, void *arg)
{
	(void)arg;
	if (node->vtable->realtime_change_fun != NULL) {
		node->vtable->realtime_change_fun(node, kq);
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

		(void)pthread_mutex_lock(&epoll_shim_ctx->step_detector_mutex);
		bool do_break =
		    epoll_shim_ctx->realtime_step_detector_generation !=
		    generation;
		(void)pthread_mutex_unlock(
		    &epoll_shim_ctx->step_detector_mutex);

		if (do_break) {
			break;
		}

		if (new_monotonic_offset.tv_sec != monotonic_offset.tv_sec ||
		    new_monotonic_offset.tv_nsec != monotonic_offset.tv_nsec) {
			monotonic_offset = new_monotonic_offset;

			rwlock_lock_read(&epoll_shim_ctx->rwlock);
			epoll_shim_ctx_for_each_unlocked(epoll_shim_ctx,
			    trigger_realtime_change_notification, NULL);
			rwlock_unlock_read(&epoll_shim_ctx->rwlock);
		}
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

	(void)pthread_mutex_lock(&epoll_shim_ctx->step_detector_mutex);
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
	(void)pthread_mutex_unlock(&epoll_shim_ctx->step_detector_mutex);
}
#endif

/**/

EPOLL_SHIM_EXPORT
int
epoll_shim_close(int fd)
{
	errno_t ec;
	int oe = errno;

	ec = epoll_shim_ctx_remove_node(&epoll_shim_ctx, fd);
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

	ssize_t rc = -1;

	if (nbytes > SSIZE_MAX) {
		ec = EINVAL;
		goto out;
	}

	size_t bytes_transferred;
	ec = node->vtable->read_fun(node, fd, buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		goto out;
	}

	ec = oe;
	rc = (ssize_t)bytes_transferred;

out:
	(void)file_description_unref(&node);
	errno = ec;
	return rc;
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

	ssize_t rc = -1;

	if (nbytes > SSIZE_MAX) {
		ec = EINVAL;
		goto out;
	}

	size_t bytes_transferred;
	ec = node->vtable->write_fun(node, fd, buf, nbytes, &bytes_transferred);
	if (ec != 0) {
		goto out;
	}

	ec = oe;
	rc = (ssize_t)bytes_transferred;

out:
	(void)file_description_unref(&node);
	errno = ec;
	return rc;
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
		rwlock_lock_read(&epoll_shim_ctx.rwlock);
		for (nfds_t i = 0; i < nfds; ++i) {
			FileDescription *node = epoll_shim_ctx_find_node_impl(
			    &epoll_shim_ctx, fds[i].fd);
			if (!node) {
				continue;
			}
			if (node->vtable->poll_fun != NULL) {
				node->vtable->poll_fun(node, fds[i].fd, NULL);
			}
		}
		rwlock_unlock_read(&epoll_shim_ctx.rwlock);
	}

	int n = real_ppoll(fds, nfds, timeout, sigmask);
	if (n < 0) {
		return errno;
	}
	if (n == 0) {
		*n_out = 0;
		return 0;
	}

	rwlock_lock_read(&epoll_shim_ctx.rwlock);
	for (nfds_t i = 0; i < nfds; ++i) {
		if (fds[i].revents == 0) {
			continue;
		}

		FileDescription *node =
		    epoll_shim_ctx_find_node_impl(&epoll_shim_ctx, fds[i].fd);
		if (!node) {
			continue;
		}
		if (node->vtable->poll_fun != NULL) {
			uint32_t revents;
			node->vtable->poll_fun(node, fds[i].fd, &revents);
			fds[i].revents = (short)revents;
			if (fds[i].revents == 0) {
				--n;
			}
		}
	}
	rwlock_unlock_read(&epoll_shim_ctx.rwlock);

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

	int rc;

	if (ec != 0) {
		rc = -1;
		goto out;
	}

	ec = oe;
	rc = 0;

out:
	(void)file_description_unref(&node);
	errno = ec;
	return rc;
}
