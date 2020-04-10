#include "epollfd_ctx.h"

#include <sys/types.h>

#if defined(__FreeBSD__)
#include <sys/capsicum.h>
#endif
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>

#ifdef EVFILT_USER
#define SUPPORT_POLL_ONLY_FDS
#define SUPPORT_NYCSS
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

static int
is_not_yet_connected_stream_socket(int s)
{
	{
		int val;
		socklen_t length = sizeof(int);

		if (getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, /**/
			&val, &length) == 0 &&
		    val) {
			return 0;
		}
	}

	{
		int type;
		socklen_t length = sizeof(int);

		if (getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
		    (type == SOCK_STREAM || type == SOCK_SEQPACKET)) {
			struct sockaddr name;
			socklen_t namelen = 0;
			if (getpeername(s, &name, &namelen) < 0 &&
			    errno == ENOTCONN) {
				return 1;
			}
		}
	}

	return 0;
}

static bool
needs_evfilt_read(RegisteredFDsNode *fd2_node, bool *clear)
{
	if (fd2_node->node_type == NODE_TYPE_FIFO) {
		if (!fd2_node->node_data.fifo.readable) {
			return false;
		}

		if (!fd2_node->node_data.fifo.writable) {
			*clear = !(fd2_node->events & EPOLLIN);
			return true;
		}
	}

	if (fd2_node->node_type == NODE_TYPE_KQUEUE) {
		*clear = !(fd2_node->events & EPOLLIN);
		return true;
	}

	if (fd2_node->events == 0 ||
	    ((fd2_node->events & (EPOLLIN | EPOLLRDHUP)) == EPOLLRDHUP)) {
		*clear = true;
		return true;
	}

	*clear = false;
	return fd2_node->events & (EPOLLIN | EPOLLRDHUP);
}

static bool
needs_evfilt_write(RegisteredFDsNode *fd2_node, bool *clear)
{
	if (fd2_node->node_type == NODE_TYPE_FIFO) {
		if (!fd2_node->node_data.fifo.writable) {
			return false;
		}

		if (!fd2_node->node_data.fifo.readable) {
			*clear = !(fd2_node->events & EPOLLOUT);
			return true;
		}
	}

	if (fd2_node->node_type == NODE_TYPE_KQUEUE) {
		return false;
	}

	*clear = false;
	return fd2_node->events & (EPOLLOUT);
}

static bool
registered_fds_node_feed_event(RegisteredFDsNode *fd2_node,
    EpollFDCtx *epollfd, struct kevent const *kev)
{
	assert(fd2_node->revents == 0);

	int revents = 0;

#ifdef SUPPORT_POLL_ONLY_FDS
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_POLL) {
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

#ifdef SUPPORT_NYCSS
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_SOCKET) {
		assert(fd2_node->node_data.socket.is_nycss);

		if (is_not_yet_connected_stream_socket(fd2_node->fd)) {
			revents = EPOLLHUP | EPOLLOUT;

			struct kevent nkev[1];
			EV_SET(&nkev[0], (uintptr_t)fd2_node, EVFILT_USER, //
			    0, NOTE_TRIGGER, 0, fd2_node);
			(void)kevent(epollfd->kq, nkev, 1, NULL, 0, NULL);

			goto out;
		} else {
			fd2_node->node_data.socket.is_nycss = false;
			fd2_node->revents = 0;
			return true;
		}
	}
#endif

#ifdef SUPPORT_EPIPE_FIFOS
	if (kev->filter == EVFILT_USER &&
	    fd2_node->node_type == NODE_TYPE_FIFO) {

		bool clear;
		if (!needs_evfilt_write(fd2_node, &clear)) {
			assert(0);
			__builtin_unreachable();
		}

		struct kevent nkev[1];
		EV_SET(&nkev[0], fd2_node->fd, EVFILT_WRITE,
		    EV_ADD | (clear ? EV_CLEAR : 0) | EV_DISPATCH, 0, 0,
		    fd2_node);

		if (kevent(epollfd->kq, nkev, 1, nkev, 1, NULL) != 1 ||
		    nkev[0].data == EPIPE) {
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
			return true;
		}
	}
#endif

	assert(kev->filter == EVFILT_READ || kev->filter == EVFILT_WRITE);
	assert((int)kev->ident == fd2_node->fd);

	if (kev->filter == EVFILT_READ) {
		revents |= EPOLLIN;
	} else if (kev->filter == EVFILT_WRITE) {
		revents |= EPOLLOUT;
	}

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

	if (kev->flags & EV_ERROR) {
		revents |= EPOLLERR;
	}

	if (kev->flags & EV_EOF) {
		if (kev->fflags) {
			revents |= EPOLLERR;
		}

		int epoll_event;

		if (fd2_node->node_type == NODE_TYPE_FIFO) {
			if (kev->filter == EVFILT_READ) {
				epoll_event = EPOLLHUP;
				if (kev->data == 0) {
					revents &= ~EPOLLIN;
				}
			} else if (kev->filter == EVFILT_WRITE) {
				if (fd2_node->events &
					(EPOLLIN | EPOLLRDHUP) &&
				    fd2_node->node_data.fifo.readable &&
				    fd2_node->node_data.fifo.writable) {
					/* Leave revents untouched. */
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
			if (kev->filter == EVFILT_READ) {
				/* do some special EPOLLRDHUP handling
				 * for sockets */

				/* if we are reading, we just know for
				 * sure that we can't receive any more,
				 * so use EPOLLIN/EPOLLRDHUP per
				 * default */
				epoll_event = EPOLLIN | EPOLLRDHUP;
			} else if (kev->filter == EVFILT_WRITE) {
				epoll_event = EPOLLOUT;
			} else {
				__builtin_unreachable();
			}

			struct pollfd pfd = {
			    .fd = fd2_node->fd,
			    .events = POLLIN | POLLOUT | POLLHUP,
			};

			if (poll(&pfd, 1, 0) == 1) {
				if ((pfd.revents & POLLHUP) ||
				    (fd2_node->eof_state ==
					(EOF_STATE_READ_EOF |
					    EOF_STATE_WRITE_EOF))) {
					/*
					 * We need to set these flags
					 * so that readers still have a
					 * chance to read the last data
					 * from the socket. This is
					 * very important to preserve
					 * Linux poll/epoll semantics
					 * when coming from an
					 * EVFILT_WRITE event.
					 */
					epoll_event |= EPOLLIN;
					epoll_event |= EPOLLRDHUP;
					epoll_event |= EPOLLHUP;
				}

				/* might as well steal flags from the
				 * poll call while we're here */
				epoll_event |=
				    (pfd.revents & (POLLIN | POLLOUT));
			}
		} else {
			epoll_event = EPOLLHUP;
		}

		revents |= epoll_event;
	}

out:
	fd2_node->revents = (uint32_t)revents;
	fd2_node->revents &= (fd2_node->events | EPOLLHUP | EPOLLERR);

	return true;
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

	if ((ec = pthread_mutex_init(&epollfd->mutex, NULL)) != 0) {
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
	errno_t ec_local = 0;

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

	return ec;
}

static errno_t
epollfd_ctx__register_events(EpollFDCtx *epollfd, RegisteredFDsNode *fd2_node)
{
	errno_t ec = 0;

	/* For now, support edge triggering only for FIFOs/pipes. */
	if (fd2_node->is_edge_triggered &&
	    fd2_node->node_type != NODE_TYPE_FIFO) {
		return EINVAL;
	}

	/* Only sockets support EPOLLRDHUP. */
	if (fd2_node->node_type != NODE_TYPE_SOCKET) {
		fd2_node->events &= ~(uint32_t)EPOLLRDHUP;
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
			EV_SET(&kev[0], fd2, EVFILT_READ, /**/
			    EV_DELETE | EV_RECEIPT, 0, 0, 0);
			EV_SET(&kev[1], fd2, EVFILT_WRITE, /**/
			    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#ifdef SUPPORT_NYCSS
			EV_SET(&kev[2], (uintptr_t)fd2_node, EVFILT_USER, /**/
			    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#endif
			(void)kevent(epollfd->kq, kev, 3, kev, 3, NULL);
			kev[0].data = kev[1].data = kev[2].data = 0;
		}

		int n = 0;
		bool clear;

		if (needs_evfilt_read(fd2_node, &clear)) {
			evfilt_read_index = n;
			clear = clear || fd2_node->is_edge_triggered;
			EV_SET(&kev[n++], fd2, EVFILT_READ,
			    EV_ADD | (clear ? EV_CLEAR : 0), 0, 0, fd2_node);
		}
		if (needs_evfilt_write(fd2_node, &clear)) {
			evfilt_write_index = n;
			clear = clear || fd2_node->is_edge_triggered;
			EV_SET(&kev[n++], fd2, EVFILT_WRITE,
			    EV_ADD | (clear ? EV_CLEAR : 0), 0, 0, fd2_node);
		}
#ifdef SUPPORT_NYCSS
		if (fd2_node->node_type == NODE_TYPE_SOCKET &&
		    is_not_yet_connected_stream_socket(fd2)) {
			EV_SET(&kev[n++], (uintptr_t)fd2_node, EVFILT_USER,
			    EV_ADD | EV_CLEAR, 0, 0, fd2_node);
			EV_SET(&kev[n++], (uintptr_t)fd2_node, EVFILT_USER, //
			    0, NOTE_TRIGGER, 0, fd2_node);

			fd2_node->node_data.socket.is_nycss = true;
		}
#endif

		assert(n != 0);

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
		fd2_node->node_type == NODE_TYPE_POLL) &&
	    (fd2_node->events & ~(uint32_t)(EPOLLIN | EPOLLOUT)) == 0 &&
	    (epollfd->poll_node == NULL || epollfd->poll_node->fd == fd2)) {

		assert(fd2_node->is_registered ||
		    fd2_node->node_type == NODE_TYPE_OTHER);

		if (!fd2_node->is_registered) {
			EV_SET(&kev[0], (uintptr_t)fd2_node, EVFILT_USER,
			    EV_ADD | EV_CLEAR, 0, 0, fd2_node);
			if (kevent(epollfd->kq, kev, 1, NULL, 0, NULL) < 0) {
				ec = errno;
				goto out;
			}
		}

		EV_SET(&kev[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);

		fd2_node->node_type = NODE_TYPE_POLL;
		epollfd->poll_node = fd2_node;
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
	int const fd2 = fd2_node->fd;

#ifdef SUPPORT_POLL_ONLY_FDS
	if (fd2_node->node_type == NODE_TYPE_POLL) {
		assert(epollfd->poll_node == fd2_node);
		epollfd->poll_node = NULL;

		struct kevent kev[1];
		EV_SET(&kev[0], (uintptr_t)fd2_node, EVFILT_USER, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);

		EV_SET(&kev[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);
	} else
#endif
	{
		struct kevent kev[3];
		EV_SET(&kev[0], fd2, EVFILT_READ, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#ifdef SUPPORT_NYCSS
		EV_SET(&kev[2], (uintptr_t)fd2_node, EVFILT_USER, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
#endif
		(void)kevent(epollfd->kq, kev, 3, kev, 3, NULL);
	}

	RB_REMOVE(registered_fds_set_, &epollfd->registered_fds, fd2_node);

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

	fd2_node->events = ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLOUT);
	fd2_node->data = ev->data;
	fd2_node->is_edge_triggered = ev->events & EPOLLET;

	if (RB_INSERT(registered_fds_set_, &epollfd->registered_fds,
		fd2_node)) {
		assert(0);
	}

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
	fd2_node->events = ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLOUT);
	fd2_node->data = ev->data;
	fd2_node->is_edge_triggered = ev->events & EPOLLET;

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
		    EPOLLHUP | EPOLLERR |		      /**/
		    EPOLLET)))) {
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
epollfd_ctx_fill_pollfds(EpollFDCtx *epollfd, struct pollfd pfds[2])
{
	if (epollfd->poll_node) {
		pfds[0] = (struct pollfd){
		    .fd = epollfd->poll_node->fd,
		    .events = (short)epollfd->poll_node->events,
		};
	} else {
		pfds[0] = (struct pollfd){.fd = -1};
	}
	pfds[1] = (struct pollfd){.fd = epollfd->kq, .events = POLLIN};
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
	if (cnt < 1 || cnt > 32) {
		return EINVAL;
	}

	struct pollfd pfds[2];
	epollfd_ctx_fill_pollfds(epollfd, pfds);

	int n = poll(pfds, 2, 0);
	if (n < 0) {
		return errno;
	}
	if (n == 0) {
		*actual_cnt = 0;
		return 0;
	}

#ifdef SUPPORT_POLL_ONLY_FDS
	if (pfds[0].revents & POLLNVAL) {
		epollfd_ctx_remove_node(epollfd, epollfd->poll_node);
	} else {
		uint32_t revents = (uint32_t)(pfds[0].revents);
		if (revents) {
			struct kevent kevs[1];
			EV_SET(&kevs[0], (uintptr_t)epollfd->poll_node,
			    EVFILT_USER, 0, NOTE_TRIGGER, 0,
			    epollfd->poll_node);
			(void)kevent(epollfd->kq, kevs, 1, NULL, 0, NULL);
		}
	}
#endif

again:;
	struct kevent kevs[32];
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

		if (!registered_fds_node_feed_event(fd2_node, epollfd,
			&kevs[i])) {
			fd2_node->del_list = del_list;
			del_list = fd2_node;
		} else {
			if (fd2_node->revents) {
				ev[j].events = fd2_node->revents;
				ev[j].data = fd2_node->data;
				++j;

				fd2_node->revents = 0;
			}
		}
	}

	for (RegisteredFDsNode *it = del_list; it != NULL;) {
		RegisteredFDsNode *next_it = it->del_list;
		epollfd_ctx_remove_node(epollfd, it);
		it = next_it;
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
