#include "epollfd_ctx.h"

#include <sys/types.h>

#if defined(__FreeBSD__)
#include <sys/capsicum.h>
#endif
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <unistd.h>

#ifdef EVFILT_USER
#define SUPPORT_POLL_ONLY_FDS
#define SUPPORT_EPIPE_FIFOS
#endif

static RegisteredFDsNode *
registered_fds_node_create(int fd)
{
	RegisteredFDsNode *node;

	node = malloc(sizeof(*node));
	if (!node) {
		return NULL;
	}

	*node = (RegisteredFDsNode){.fd = fd};

	return node;
}

static void
registered_fds_node_destroy(RegisteredFDsNode *node)
{
	free(node);
}

typedef struct {
	int evfilt_read;
	int evfilt_write;
} NeededFilters;

static NeededFilters
get_needed_filters(RegisteredFDsNode *fd2_node)
{
	NeededFilters needed_filters;

	if (fd2_node->node_type == NODE_TYPE_FIFO) {
		if (fd2_node->node_data.fifo.readable &&
		    fd2_node->node_data.fifo.writable) {
			needed_filters.evfilt_read =
			    !!(fd2_node->events & EPOLLIN);
			needed_filters.evfilt_write =
			    !!(fd2_node->events & EPOLLOUT);

			if (fd2_node->events == 0) {
				needed_filters.evfilt_read =
				    fd2_node->eof_state ? 1 : EV_CLEAR;
			}

		} else if (fd2_node->node_data.fifo.readable) {
			needed_filters.evfilt_read =
			    !!(fd2_node->events & EPOLLIN);
			needed_filters.evfilt_write = 0;

			if (needed_filters.evfilt_read == 0) {
				needed_filters.evfilt_read =
				    fd2_node->eof_state ? 1 : EV_CLEAR;
			}
		} else if (fd2_node->node_data.fifo.writable) {
			needed_filters.evfilt_read = 0;
			needed_filters.evfilt_write =
			    !!(fd2_node->events & EPOLLOUT);

			if (needed_filters.evfilt_write == 0) {
				needed_filters.evfilt_write =
				    fd2_node->eof_state ? 1 : EV_CLEAR;
			}
		} else {
			__builtin_unreachable();
		}

		goto out;
	}

	if (fd2_node->node_type == NODE_TYPE_KQUEUE) {
		needed_filters.evfilt_read = !!(fd2_node->events & EPOLLIN);
		needed_filters.evfilt_write = 0;

		assert(fd2_node->eof_state == 0);

		if (needed_filters.evfilt_read == 0) {
			needed_filters.evfilt_read = EV_CLEAR;
		}

		goto out;
	}

	if (fd2_node->node_type == NODE_TYPE_SOCKET) {
		needed_filters.evfilt_read = !!(fd2_node->events & EPOLLIN);
		if (needed_filters.evfilt_read == 0 &&
		    (fd2_node->events & EPOLLRDHUP)) {
			needed_filters.evfilt_read =
			    (fd2_node->eof_state & EOF_STATE_READ_EOF)
			    ? 1
			    : EV_CLEAR;
		}

		needed_filters.evfilt_write = !!(fd2_node->events & EPOLLOUT);

		/* Let's use EVFILT_READ to drive the POLLHUP. */
		if (fd2_node->eof_state ==
		    (EOF_STATE_READ_EOF | EOF_STATE_WRITE_EOF)) {
			if (needed_filters.evfilt_read != 1 &&
			    needed_filters.evfilt_write != 1) {
				needed_filters.evfilt_read = 1;
			}

			if (needed_filters.evfilt_read) {
				needed_filters.evfilt_write = 0;
			} else {
				needed_filters.evfilt_read = 0;
			}
		}

		/* We need something to detect POLLHUP. */
		if (fd2_node->eof_state == 0 &&
		    needed_filters.evfilt_read == 0 &&
		    needed_filters.evfilt_write == 0) {
			needed_filters.evfilt_read = EV_CLEAR;
		}

		if (fd2_node->eof_state == EOF_STATE_READ_EOF) {
			if (needed_filters.evfilt_write == 0) {
				needed_filters.evfilt_write = EV_CLEAR;
			}
		}

		if (fd2_node->eof_state == EOF_STATE_WRITE_EOF) {
			if (needed_filters.evfilt_read == 0) {
				needed_filters.evfilt_read = EV_CLEAR;
			}
		}

		goto out;
	}

	needed_filters.evfilt_read = !!(fd2_node->events & EPOLLIN);
	needed_filters.evfilt_write = !!(fd2_node->events & EPOLLOUT);

	if (fd2_node->events == 0) {
		needed_filters.evfilt_read =
		    fd2_node->eof_state ? 1 : EV_CLEAR;
	}

out:
	if (fd2_node->is_edge_triggered) {
		if (needed_filters.evfilt_read) {
			needed_filters.evfilt_read = EV_CLEAR;
		}
		if (needed_filters.evfilt_write) {
			needed_filters.evfilt_write = EV_CLEAR;
		}
	}

	assert(needed_filters.evfilt_read || needed_filters.evfilt_write);
	assert(needed_filters.evfilt_read == 0 ||
	    needed_filters.evfilt_read == 1 ||
	    needed_filters.evfilt_read == EV_CLEAR);
	assert(needed_filters.evfilt_write == 0 ||
	    needed_filters.evfilt_write == 1 ||
	    needed_filters.evfilt_write == EV_CLEAR);

	return needed_filters;
}

static void
registered_fds_node_update_flags_from_epoll_event(RegisteredFDsNode *fd2_node,
    struct epoll_event *ev)
{
	fd2_node->events =
	    ev->events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLOUT);
	fd2_node->data = ev->data;
	fd2_node->is_edge_triggered = ev->events & EPOLLET;
	fd2_node->is_oneshot = ev->events & EPOLLONESHOT;

	if (fd2_node->is_oneshot) {
		fd2_node->is_edge_triggered = true;
	}
}

static bool
registered_fds_node_feed_event(RegisteredFDsNode *fd2_node,
    EpollFDCtx *epollfd, struct kevent const *kev)
{
	int revents = 0;

#ifdef SUPPORT_POLL_ONLY_FDS
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_POLL) {
		assert(fd2_node->revents == 0);

		struct pollfd pfd = {
		    .fd = fd2_node->fd,
		    .events = (short)fd2_node->events,
		};

		revents = poll(&pfd, 1, 0) < 0 ? EPOLLERR : pfd.revents;

		if (revents & POLLNVAL) {
			return false;
		}

		fd2_node->revents = (uint32_t)revents;
		assert(!(fd2_node->revents &
		    ~(uint32_t)(POLLIN | POLLOUT | POLLERR | POLLHUP)));
		return true;
	}
#endif

#ifdef SUPPORT_EPIPE_FIFOS
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_FIFO) {
		assert(fd2_node->revents == 0);

		assert(!fd2_node->has_evfilt_read);
		assert(!fd2_node->has_evfilt_write);
		assert(!fd2_node->has_epollpri);

		NeededFilters needed_filters = get_needed_filters(fd2_node);
		assert(needed_filters.evfilt_write);

		struct kevent nkev[1];
		EV_SET(&nkev[0], fd2_node->fd, EVFILT_WRITE,
		    EV_ADD | (needed_filters.evfilt_write & EV_CLEAR) |
			EV_RECEIPT,
		    0, 0, fd2_node);

		if (kevent(epollfd->kq, nkev, 1, nkev, 1, NULL) != 1 ||
		    nkev[0].data != 0) {
			revents = EPOLLERR | EPOLLOUT;

			if (!fd2_node->is_edge_triggered) {
				EV_SET(&nkev[0], (uintptr_t)fd2_node,
				    EVFILT_USER, //
				    0, NOTE_TRIGGER, 0, fd2_node);
				(void)kevent(epollfd->kq, nkev, 1, NULL, 0,
				    NULL);
			}

			goto out;
		} else {
			fd2_node->has_evfilt_write = true;
			return true;
		}
	}
#endif

#ifdef EVFILT_USER
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_SOCKET) {
		assert(fd2_node->events & EPOLLPRI);

		struct pollfd pfd = {
		    .fd = fd2_node->fd,
		    .events = POLLPRI,
		};

		revents = (poll(&pfd, 1, 0) == 1) && (pfd.revents & POLLPRI)
		    ? EPOLLPRI
		    : 0;

		fd2_node->got_epollpri = true;

		goto out;
	}
#endif

	assert(kev->filter == EVFILT_READ || kev->filter == EVFILT_WRITE);
	assert((int)kev->ident == fd2_node->fd);

	if (kev->filter == EVFILT_READ) {
		revents |= EPOLLIN;
	} else if (kev->filter == EVFILT_WRITE) {
		revents |= EPOLLOUT;
	}

	if (fd2_node->node_type == NODE_TYPE_SOCKET) {
		if (kev->filter == EVFILT_READ) {
			if (kev->flags & EV_EOF) {
				fd2_node->eof_state |= EOF_STATE_READ_EOF;
			} else {
				fd2_node->eof_state &= ~EOF_STATE_READ_EOF;
			}
		} else if (kev->filter == EVFILT_WRITE) {
			if (kev->flags & EV_EOF) {
				fd2_node->eof_state |= EOF_STATE_WRITE_EOF;
			} else {
				fd2_node->eof_state &= ~EOF_STATE_WRITE_EOF;
			}
		}
	} else {
		if (kev->filter == EVFILT_READ) {
			if (kev->flags & EV_EOF) {
				fd2_node->eof_state =
				    EOF_STATE_READ_EOF | EOF_STATE_WRITE_EOF;
			} else {
				fd2_node->eof_state = 0;
			}
		} else if (kev->filter == EVFILT_WRITE) {
			if (kev->flags & EV_EOF) {
				fd2_node->eof_state =
				    EOF_STATE_READ_EOF | EOF_STATE_WRITE_EOF;
			} else {
				fd2_node->eof_state = 0;
			}
		}
	}

	if (kev->flags & EV_ERROR) {
		revents |= EPOLLERR;
	}

	if (kev->flags & EV_EOF) {
		if (kev->fflags) {
			revents |= EPOLLERR;
		}
	}

	if (fd2_node->eof_state) {
		int epoll_event;

		if (fd2_node->node_type == NODE_TYPE_FIFO) {
			if (kev->filter == EVFILT_READ) {
				epoll_event = EPOLLHUP;
				if (kev->data == 0) {
					revents &= ~EPOLLIN;
				}
			} else if (kev->filter == EVFILT_WRITE) {
				if (fd2_node->has_evfilt_read) {
					assert(
					    fd2_node->node_data.fifo.readable);
					assert(
					    fd2_node->node_data.fifo.writable);

					/*
					 * Any non-zero revents must have come
					 * from the EVFILT_READ filter. It
					 * could either be "POLLIN",
					 * "POLLIN | POLLHUP" or "POLLHUP", so
					 * we know if there is data to read.
					 * But we also know that the FIFO is
					 * done, so set POLLHUP because it
					 * would be set anyway.
					 *
					 * If revents is zero, not setting it
					 * will simply ignore this EVFILT_WRITE
					 * and wait for the next EVFILT_READ
					 * (which will be EOF).
					 */

					if (fd2_node->revents != 0) {
						fd2_node->revents |= POLLHUP;
					}
					return true;
				}

				epoll_event = EPOLLERR;
				if (kev->data < PIPE_BUF) {
					revents &= ~EPOLLOUT;
				}
			} else {
				__builtin_unreachable();
			}
		} else if (fd2_node->node_type == NODE_TYPE_SOCKET) {
			epoll_event = 0;

			if (fd2_node->eof_state & EOF_STATE_READ_EOF) {
				epoll_event |= EPOLLIN | EPOLLRDHUP;
			}

			if (fd2_node->eof_state & EOF_STATE_WRITE_EOF) {
				epoll_event |= EPOLLOUT;
			}

			if (fd2_node->eof_state ==
			    (EOF_STATE_READ_EOF | EOF_STATE_WRITE_EOF)) {
				epoll_event |= EPOLLHUP;
			}
		} else {
			epoll_event = EPOLLHUP;
		}

		revents |= epoll_event;
	}

out:
	fd2_node->revents |= (uint32_t)revents;
	fd2_node->revents &= (fd2_node->events | EPOLLHUP | EPOLLERR);

	if (fd2_node->revents) {
		if (kev->filter == EVFILT_READ) {
			fd2_node->got_evfilt_read = true;
		} else if (kev->filter == EVFILT_WRITE) {
			fd2_node->got_evfilt_write = true;
		}
	}

	return true;
}

static void
registered_fds_node_register_for_completion(int *kq,
    RegisteredFDsNode *fd2_node)
{
	struct kevent kev[2];
	int n = 0;

	if (fd2_node->has_evfilt_read && !fd2_node->got_evfilt_read) {
		EV_SET(&kev[n++], fd2_node->fd, EVFILT_READ,
		    EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, fd2_node);
	}
	if (fd2_node->has_evfilt_write && !fd2_node->got_evfilt_write) {
		EV_SET(&kev[n++], fd2_node->fd, EVFILT_WRITE,
		    EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, fd2_node);
	}
#ifdef EVFILT_USER
	if (fd2_node->has_epollpri && !fd2_node->got_epollpri) {
		struct kevent epollpri_event;

		EV_SET(&epollpri_event, fd2_node->fd, EVFILT_USER, 0, 0, 0,
		    fd2_node);

		if (!registered_fds_node_feed_event(fd2_node, NULL,
			&epollpri_event)) {
			assert(0);
		}
	}
#endif

	if (n == 0) {
		return;
	}

	if (*kq < 0) {
		*kq = kqueue();
	}

	if (*kq >= 0) {
		(void)kevent(*kq, kev, n, kev, n, NULL);
	}
}

static void
registered_fds_node_complete(int kq)
{
	if (kq < 0) {
		return;
	}

	struct kevent kevs[32];
	int n;

	while ((n = kevent(kq, /**/
		    NULL, 0, kevs, 32, &(struct timespec){0, 0})) > 0) {
		for (int i = 0; i < n; ++i) {
			RegisteredFDsNode *fd2_node =
			    (RegisteredFDsNode *)kevs[i].udata;

			if (!registered_fds_node_feed_event(fd2_node, NULL,
				&kevs[i])) {
				assert(0);
			}
		}
	}

	(void)close(kq);
}

static int
fd_cmp(RegisteredFDsNode *e1, RegisteredFDsNode *e2)
{
	return (e1->fd < e2->fd) ? -1 : (e1->fd > e2->fd);
}

RB_GENERATE_STATIC(registered_fds_set_, registered_fds_node_, entry, fd_cmp)

errno_t
epollfd_ctx_init(EpollFDCtx *epollfd, int kq)
{
	errno_t ec;

	*epollfd = (EpollFDCtx){
	    .kq = kq,
	    .registered_fds = RB_INITIALIZER(&registered_fds),
	};

	TAILQ_INIT(&epollfd->poll_fds);

	if ((ec = pthread_mutex_init(&epollfd->mutex, NULL)) != 0) {
		return ec;
	}

	if ((ec = pthread_mutex_init(&epollfd->nr_polling_threads_mutex,
		 NULL)) != 0) {
		pthread_mutex_destroy(&epollfd->mutex);
		return ec;
	}

	if ((ec = pthread_cond_init(&epollfd->nr_polling_threads_cond,
		 NULL)) != 0) {
		pthread_mutex_destroy(&epollfd->nr_polling_threads_mutex);
		pthread_mutex_destroy(&epollfd->mutex);
		return ec;
	}

#ifdef SUPPORT_POLL_ONLY_FDS
	struct kevent kev[1];
	EV_SET(&kev[0], 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);
#endif

	return 0;
}

errno_t
epollfd_ctx_terminate(EpollFDCtx *epollfd)
{
	errno_t ec = 0;
	errno_t ec_local;

	ec_local = pthread_cond_destroy(&epollfd->nr_polling_threads_cond);
	ec = ec ? ec : ec_local;
	ec_local = pthread_mutex_destroy(&epollfd->nr_polling_threads_mutex);
	ec = ec ? ec : ec_local;
	ec_local = pthread_mutex_destroy(&epollfd->mutex);
	ec = ec ? ec : ec_local;

	RegisteredFDsNode *np;
	RegisteredFDsNode *np_temp;
	RB_FOREACH_SAFE(np, registered_fds_set_, &epollfd->registered_fds,
	    np_temp)
	{
		RB_REMOVE(registered_fds_set_, &epollfd->registered_fds, np);
		registered_fds_node_destroy(np);
	}

	free(epollfd->kevs);
	free(epollfd->pfds);

	return ec;
}

static errno_t
epollfd_ctx_make_kevs_space(EpollFDCtx *epollfd, size_t cnt)
{
	assert(cnt > 0);

	if (cnt <= epollfd->kevs_length) {
		return 0;
	}

	size_t size;
	if (__builtin_mul_overflow(cnt, sizeof(struct kevent), &size)) {
		return ENOMEM;
	}

	struct kevent *new_kevs = realloc(epollfd->kevs, size);
	if (!new_kevs) {
		return errno;
	}

	epollfd->kevs = new_kevs;
	epollfd->kevs_length = cnt;

	return 0;
}

static errno_t
epollfd_ctx_make_pfds_space(EpollFDCtx *epollfd)
{
	size_t cnt = 1 + epollfd->poll_fds_size;

	if (cnt <= epollfd->pfds_length) {
		return 0;
	}

	size_t size;
	if (__builtin_mul_overflow(cnt, sizeof(struct pollfd), &size)) {
		return ENOMEM;
	}

	struct pollfd *new_pfds = realloc(epollfd->pfds, size);
	if (!new_pfds) {
		return errno;
	}

	epollfd->pfds = new_pfds;
	epollfd->pfds_length = cnt;

	return 0;
}

#ifdef EVFILT_USER
static void
epollfd_ctx__trigger_repoll(EpollFDCtx *epollfd)
{
	(void)pthread_mutex_lock(&epollfd->nr_polling_threads_mutex);
	unsigned long nr_polling_threads = epollfd->nr_polling_threads;
	(void)pthread_mutex_unlock(&epollfd->nr_polling_threads_mutex);

	if (nr_polling_threads == 0) {
		return;
	}

	struct kevent kev[1];
	EV_SET(&kev[0], 0, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, 0);
	(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);

	(void)pthread_mutex_lock(&epollfd->nr_polling_threads_mutex);
	while (epollfd->nr_polling_threads != 0) {
		pthread_cond_wait(&epollfd->nr_polling_threads_cond,
		    &epollfd->nr_polling_threads_mutex);
	}
	(void)pthread_mutex_unlock(&epollfd->nr_polling_threads_mutex);
}
#endif

static void
epollfd_ctx__remove_node_from_kq(EpollFDCtx *epollfd,
    RegisteredFDsNode *fd2_node)
{
#ifdef EVFILT_USER
	if (fd2_node->is_on_pollfd_list) {
		TAILQ_REMOVE(&epollfd->poll_fds, fd2_node, pollfd_list_entry);
		fd2_node->is_on_pollfd_list = false;
		assert(epollfd->poll_fds_size != 0);
		--epollfd->poll_fds_size;

		epollfd_ctx__trigger_repoll(epollfd);
	}

	if (fd2_node->node_type == NODE_TYPE_POLL) {
		struct kevent kev[1];

		EV_SET(&kev[0], (uintptr_t)fd2_node, EVFILT_USER, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);
	} else
#endif
	{
		struct kevent kev[3];
		int fd2 = fd2_node->fd;

		EV_SET(&kev[0], fd2, EVFILT_READ, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#ifdef EVFILT_USER
		EV_SET(&kev[2], (uintptr_t)fd2_node, EVFILT_USER, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#endif
		(void)kevent(epollfd->kq, kev, 3, kev, 3, NULL);

		fd2_node->has_evfilt_read = false;
		fd2_node->has_evfilt_write = false;
		fd2_node->has_epollpri = false;
	}
}

static errno_t
epollfd_ctx__register_events(EpollFDCtx *epollfd, RegisteredFDsNode *fd2_node)
{
	errno_t ec = 0;

	/* Only sockets support EPOLLRDHUP and EPOLLPRI. */
	if (fd2_node->node_type != NODE_TYPE_SOCKET) {
		fd2_node->events &= ~(uint32_t)EPOLLRDHUP;
		fd2_node->events &= ~(uint32_t)EPOLLPRI;
	}

	int const fd2 = fd2_node->fd;
	struct kevent kev[4] = {
	    {.data = 0},
	    {.data = 0},
	    {.data = 0},
	    {.data = 0},
	};

	assert(fd2 >= 0);

	int evfilt_read_index = -1;
	int evfilt_write_index = -1;

	if (fd2_node->node_type != NODE_TYPE_POLL) {
		if (fd2_node->is_registered) {
			epollfd_ctx__remove_node_from_kq(epollfd, fd2_node);
		}

		int n = 0;

		assert(!fd2_node->has_evfilt_read);
		assert(!fd2_node->has_evfilt_write);
		assert(!fd2_node->has_epollpri);

		NeededFilters needed_filters = get_needed_filters(fd2_node);

		if (needed_filters.evfilt_read) {
			fd2_node->has_evfilt_read = true;
			evfilt_read_index = n;
			EV_SET(&kev[n++], fd2, EVFILT_READ,
			    EV_ADD | (needed_filters.evfilt_read & EV_CLEAR),
			    0, 0, fd2_node);
		}
		if (needed_filters.evfilt_write) {
			fd2_node->has_evfilt_write = true;
			evfilt_write_index = n;
			EV_SET(&kev[n++], fd2, EVFILT_WRITE,
			    EV_ADD | (needed_filters.evfilt_write & EV_CLEAR),
			    0, 0, fd2_node);
		}

		assert(n != 0);

#ifdef EVFILT_USER
		if (fd2_node->events & EPOLLPRI) {
			fd2_node->has_epollpri = true;
			EV_SET(&kev[n++], (uintptr_t)fd2_node, EVFILT_USER,
			    EV_ADD | EV_CLEAR, 0, 0, fd2_node);

			if (!fd2_node->is_on_pollfd_list) {
				TAILQ_INSERT_TAIL(&epollfd->poll_fds, fd2_node,
				    pollfd_list_entry);
				fd2_node->is_on_pollfd_list = true;
				++epollfd->poll_fds_size;

				epollfd_ctx__trigger_repoll(epollfd);
			}
		}
#endif

		for (int i = 0; i < n; ++i) {
			kev[i].flags |= EV_RECEIPT;
		}

		int ret = kevent(epollfd->kq, kev, n, kev, n, NULL);
		if (ret < 0) {
			ec = errno;
			goto out;
		}

		if (ret != n) {
			ec = EINVAL;
			goto out;
		}

		for (int i = 0; i < n; ++i) {
			if (!(kev[i].flags & EV_ERROR)) {
				ec = EINVAL;
				goto out;
			}
		}
	}

#ifdef SUPPORT_POLL_ONLY_FDS
	/* Check for fds that only support poll. */
	if (((fd2_node->node_type == NODE_TYPE_OTHER &&
		 (kev[0].data == ENODEV || kev[1].data == ENODEV)) ||
		fd2_node->node_type == NODE_TYPE_POLL)) {

		assert((fd2_node->events & /**/
			   ~(uint32_t)(EPOLLIN | EPOLLOUT)) == 0);
		assert(fd2_node->is_registered ||
		    fd2_node->node_type == NODE_TYPE_OTHER);

		fd2_node->has_evfilt_read = false;
		fd2_node->has_evfilt_write = false;
		fd2_node->has_epollpri = false;

		EV_SET(&kev[0], (uintptr_t)fd2_node, EVFILT_USER,
		    EV_ADD | EV_CLEAR, 0, 0, fd2_node);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);

		fd2_node->node_type = NODE_TYPE_POLL;

		if (!fd2_node->is_on_pollfd_list) {
			TAILQ_INSERT_TAIL(&epollfd->poll_fds, fd2_node,
			    pollfd_list_entry);
			fd2_node->is_on_pollfd_list = true;
			++epollfd->poll_fds_size;
		}

		epollfd_ctx__trigger_repoll(epollfd);

		goto out;
	}

	if (fd2_node->node_type == NODE_TYPE_POLL) {
		ec = EINVAL;
		goto out;
	}
#endif

	for (int i = 0; i < 4; ++i) {
		if (kev[i].data != 0) {
#ifdef SUPPORT_EPIPE_FIFOS
			if (kev[i].data == EPIPE && i == evfilt_write_index &&
			    fd2_node->node_type == NODE_TYPE_FIFO) {
				fd2_node->eof_state |= EOF_STATE_WRITE_EOF;
				fd2_node->has_evfilt_write = false;

				if (evfilt_read_index < 0) {
					struct kevent nkev[2];
					EV_SET(&nkev[0], (uintptr_t)fd2_node,
					    EVFILT_USER, EV_ADD | EV_CLEAR, 0,
					    0, fd2_node);
					EV_SET(&nkev[1], (uintptr_t)fd2_node,
					    EVFILT_USER, 0, NOTE_TRIGGER, 0,
					    fd2_node);
					(void)kevent(epollfd->kq, nkev, 2,
					    NULL, 0, NULL);
				}
			} else {
				ec = (int)kev[i].data;
				goto out;
			}
#else
			ec = (int)kev[i].data;
			goto out;
#endif
		}
	}

	ec = 0;

out:
	return ec;
}

static void
epollfd_ctx_remove_node(EpollFDCtx *epollfd, RegisteredFDsNode *fd2_node)
{
	epollfd_ctx__remove_node_from_kq(epollfd, fd2_node);

	RB_REMOVE(registered_fds_set_, &epollfd->registered_fds, fd2_node);
	assert(epollfd->registered_fds_size > 0);
	--epollfd->registered_fds_size;

	registered_fds_node_destroy(fd2_node);
}

#if defined(__FreeBSD__)
static void
modify_fifo_rights_from_capabilities(RegisteredFDsNode *fd2_node)
{
	assert(fd2_node->node_data.fifo.readable);
	assert(fd2_node->node_data.fifo.writable);

	cap_rights_t rights;
	memset(&rights, 0, sizeof(rights));

	if (cap_rights_get(fd2_node->fd, &rights) == 0) {
		cap_rights_t test_rights;

		cap_rights_init(&test_rights, CAP_READ);
		bool has_read_rights =
		    cap_rights_contains(&rights, &test_rights);

		cap_rights_init(&test_rights, CAP_WRITE);
		bool has_write_rights =
		    cap_rights_contains(&rights, &test_rights);

		if (has_read_rights != has_write_rights) {
			fd2_node->node_data.fifo.readable = has_read_rights;
			fd2_node->node_data.fifo.writable = has_write_rights;
		}
	}
}
#endif

static errno_t
epollfd_ctx_add_node(EpollFDCtx *epollfd, int fd2, struct epoll_event *ev,
    struct stat const *statbuf)
{
	RegisteredFDsNode *fd2_node = registered_fds_node_create(fd2);
	if (!fd2_node) {
		return ENOMEM;
	}

	if (S_ISFIFO(statbuf->st_mode)) {
		int tmp;

		if (ioctl(fd2_node->fd, FIONREAD, &tmp) < 0 &&
		    errno == ENOTTY) {
			fd2_node->node_type = NODE_TYPE_KQUEUE;
		} else {
			fd2_node->node_type = NODE_TYPE_FIFO;

			int fl = fcntl(fd2, F_GETFL, 0);
			if (fl < 0) {
				errno_t ec = errno;
				registered_fds_node_destroy(fd2_node);
				return ec;
			}

			fl &= O_ACCMODE;

			if (fl == O_RDWR) {
				fd2_node->node_data.fifo.readable = true;
				fd2_node->node_data.fifo.writable = true;
#if defined(__FreeBSD__)
				modify_fifo_rights_from_capabilities(fd2_node);
#endif
			} else if (fl == O_WRONLY) {
				fd2_node->node_data.fifo.writable = true;
			} else if (fl == O_RDONLY) {
				fd2_node->node_data.fifo.readable = true;
			} else {
				registered_fds_node_destroy(fd2_node);
				return EINVAL;
			}
		}
	} else if (S_ISSOCK(statbuf->st_mode)) {
		fd2_node->node_type = NODE_TYPE_SOCKET;
	} else {
		/* May also be NODE_TYPE_POLL,
		   will be checked when registering. */
		fd2_node->node_type = NODE_TYPE_OTHER;
	}

	registered_fds_node_update_flags_from_epoll_event(fd2_node, ev);

	if (RB_INSERT(registered_fds_set_, &epollfd->registered_fds,
		fd2_node)) {
		assert(0);
	}
	++epollfd->registered_fds_size;

	errno_t ec = epollfd_ctx__register_events(epollfd, fd2_node);
	if (ec != 0) {
		epollfd_ctx_remove_node(epollfd, fd2_node);
		return ec;
	}

	fd2_node->is_registered = true;

	return 0;
}

static errno_t
epollfd_ctx_modify_node(EpollFDCtx *epollfd, RegisteredFDsNode *fd2_node,
    struct epoll_event *ev)
{
	registered_fds_node_update_flags_from_epoll_event(fd2_node, ev);

	assert(fd2_node->is_registered);

	errno_t ec = epollfd_ctx__register_events(epollfd, fd2_node);
	if (ec != 0) {
		epollfd_ctx_remove_node(epollfd, fd2_node);
		return ec;
	}

	return 0;
}

static errno_t
epollfd_ctx_ctl_impl(EpollFDCtx *epollfd, int op, int fd2,
    struct epoll_event *ev)
{
	assert(op == EPOLL_CTL_DEL || ev != NULL);

	if (epollfd->kq == fd2) {
		return EINVAL;
	}

	if (op != EPOLL_CTL_DEL &&
	    ((ev->events &
		~(uint32_t)(EPOLLIN | EPOLLOUT | EPOLLRDHUP | /**/
		    EPOLLPRI |		  /* unsupported by kqueue! */
		    EPOLLHUP | EPOLLERR | /**/
		    EPOLLET | EPOLLONESHOT)))) {
		return EINVAL;
	}

	RegisteredFDsNode *fd2_node;
	{
		RegisteredFDsNode find;
		find.fd = fd2;

		fd2_node = RB_FIND(registered_fds_set_, /**/
		    &epollfd->registered_fds, &find);
	}

	struct stat statbuf;
	if (fstat(fd2, &statbuf) < 0) {
		errno_t ec = errno;

		/* If the fstat fails for any reason we must clear
		 * internal state to avoid EEXIST errors in future
		 * calls to epoll_ctl. */
		if (fd2_node) {
			epollfd_ctx_remove_node(epollfd, fd2_node);
		}

		return ec;
	}

	errno_t ec;

	if (op == EPOLL_CTL_ADD) {
		ec = fd2_node
		    ? EEXIST
		    : epollfd_ctx_add_node(epollfd, fd2, ev, &statbuf);
	} else if (op == EPOLL_CTL_DEL) {
		ec = !fd2_node
		    ? ENOENT
		    : (epollfd_ctx_remove_node(epollfd, fd2_node), 0);
	} else if (op == EPOLL_CTL_MOD) {
		ec = !fd2_node
		    ? ENOENT
		    : epollfd_ctx_modify_node(epollfd, fd2_node, ev);
	} else {
		ec = EINVAL;
	}

	return ec;
}

void
epollfd_ctx_fill_pollfds(EpollFDCtx *epollfd, struct pollfd *pfds)
{
	pfds[0] = (struct pollfd){.fd = epollfd->kq, .events = POLLIN};

	RegisteredFDsNode *poll_node;
	size_t i = 1;
	TAILQ_FOREACH(poll_node, &epollfd->poll_fds, pollfd_list_entry)
	{
		pfds[i++] = (struct pollfd){
		    .fd = poll_node->fd,
		    .events = poll_node->node_type == NODE_TYPE_POLL
			? (short)poll_node->events
			: POLLPRI,
		};
	}
}

errno_t
epollfd_ctx_ctl(EpollFDCtx *epollfd, int op, int fd2, struct epoll_event *ev)
{
	errno_t ec;

	(void)pthread_mutex_lock(&epollfd->mutex);
	ec = epollfd_ctx_ctl_impl(epollfd, op, fd2, ev);
	(void)pthread_mutex_unlock(&epollfd->mutex);

	return ec;
}

static errno_t
epollfd_ctx_wait_impl(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt)
{
	errno_t ec;

	if (cnt < 1) {
		return EINVAL;
	}

	ec = epollfd_ctx_make_pfds_space(epollfd);
	if (ec != 0) {
		return ec;
	}

	epollfd_ctx_fill_pollfds(epollfd, epollfd->pfds);

	int n = poll(epollfd->pfds, (nfds_t)(1 + epollfd->poll_fds_size), 0);
	if (n < 0) {
		return errno;
	}
	if (n == 0) {
		*actual_cnt = 0;
		return 0;
	}

#ifdef SUPPORT_POLL_ONLY_FDS
	{
		RegisteredFDsNode *poll_node, *tmp_poll_node;
		size_t i = 1;
		TAILQ_FOREACH_SAFE(poll_node, &epollfd->poll_fds,
		    pollfd_list_entry, tmp_poll_node)
		{
			struct pollfd *pfd = &epollfd->pfds[i++];

			if (pfd->revents & POLLNVAL) {
				epollfd_ctx_remove_node(epollfd, poll_node);
			} else if (pfd->revents) {
				struct kevent kevs[1];
				EV_SET(&kevs[0], (uintptr_t)poll_node,
				    EVFILT_USER, 0, NOTE_TRIGGER, 0,
				    poll_node);
				(void)kevent(epollfd->kq, kevs, 1, NULL, 0,
				    NULL);
			}
		}
	}
#endif

again:;

	/*
	 * Each registered fd can produce a maximum of 3 kevents. If the
	 * provided space in 'ev' is large enough to hold results for all
	 * registered fds, provide enough space for the kevent call as well.
	 * Add some wiggle room for the 'poll only fd' notification mechanism.
	 */
	if ((size_t)cnt >= epollfd->registered_fds_size) {
		if (__builtin_add_overflow(cnt, 1, &cnt)) {
			return ENOMEM;
		}
		if (__builtin_mul_overflow(cnt, 3, &cnt)) {
			return ENOMEM;
		}
	}

	ec = epollfd_ctx_make_kevs_space(epollfd, (size_t)cnt);
	if (ec != 0) {
		return ec;
	}

	struct kevent *kevs = epollfd->kevs;
	assert(kevs != NULL);

	n = kevent(epollfd->kq, NULL, 0, kevs, cnt, &(struct timespec){0, 0});
	if (n < 0) {
		return errno;
	}

	int j = 0;

	RegisteredFDsNode *del_list = NULL;

	for (int i = 0; i < n; ++i) {
		RegisteredFDsNode *fd2_node =
		    (RegisteredFDsNode *)kevs[i].udata;

#ifdef SUPPORT_POLL_ONLY_FDS
		if (!fd2_node) {
			assert(kevs[i].filter == EVFILT_USER);
			assert(kevs[i].udata == NULL);
			continue;
		}
#endif

		for (RegisteredFDsNode *it = del_list; it != NULL;
		     it = it->del_list) {
			if (it == fd2_node) {
				continue;
			}
		}

		uint32_t old_revents = fd2_node->revents;
		NeededFilters old_needed_filters =
		    get_needed_filters(fd2_node);

		if (!registered_fds_node_feed_event(fd2_node, epollfd,
			&kevs[i])) {
			fd2_node->del_list = del_list;
			del_list = fd2_node;
		} else {
			if (fd2_node->node_type != NODE_TYPE_POLL &&
			    !(fd2_node->is_edge_triggered &&
				fd2_node->eof_state ==
				    (EOF_STATE_READ_EOF |
					EOF_STATE_WRITE_EOF) &&
				fd2_node->node_type != NODE_TYPE_FIFO)) {

				NeededFilters needed_filters =
				    get_needed_filters(fd2_node);

				if (old_needed_filters.evfilt_read !=
					needed_filters.evfilt_read ||
				    old_needed_filters.evfilt_write !=
					needed_filters.evfilt_write) {

					if (epollfd_ctx__register_events(
						epollfd, fd2_node) != 0) {
						epollfd_ctx__remove_node_from_kq(
						    epollfd, fd2_node);
					}
				}
			}

			if (fd2_node->revents && !old_revents) {
				ev[j++].data.ptr = fd2_node;
			}
		}
	}

	/* This assumes that those nodes are never put into the 'ev' list. */
	for (RegisteredFDsNode *it = del_list; it != NULL;) {
		RegisteredFDsNode *next_it = it->del_list;
		epollfd_ctx_remove_node(epollfd, it);
		it = next_it;
	}

	{
		int completion_kq = -1;

		for (int i = 0; i < j; ++i) {
			RegisteredFDsNode *fd2_node =
			    (RegisteredFDsNode *)ev[i].data.ptr;

			if (n == cnt || fd2_node->is_edge_triggered) {
				registered_fds_node_register_for_completion(
				    &completion_kq, fd2_node);
			}
		}

		registered_fds_node_complete(completion_kq);
	}

	for (int i = 0; i < j; ++i) {
		RegisteredFDsNode *fd2_node =
		    (RegisteredFDsNode *)ev[i].data.ptr;

		ev[i].events = fd2_node->revents;
		ev[i].data = fd2_node->data;

		fd2_node->revents = 0;
		fd2_node->got_evfilt_read = false;
		fd2_node->got_evfilt_write = false;
		fd2_node->got_epollpri = false;

		if (fd2_node->is_oneshot) {
			epollfd_ctx__remove_node_from_kq(epollfd, fd2_node);
		}
	}

	if (n && j == 0) {
		goto again;
	}

	*actual_cnt = j;
	return 0;
}

errno_t
epollfd_ctx_wait(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt)
{
	errno_t ec;

	(void)pthread_mutex_lock(&epollfd->mutex);
	ec = epollfd_ctx_wait_impl(epollfd, ev, cnt, actual_cnt);
	(void)pthread_mutex_unlock(&epollfd->mutex);

	return ec;
}
