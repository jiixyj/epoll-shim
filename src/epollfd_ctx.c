#include "epollfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <assert.h>
#include <stdlib.h>

#include <poll.h>

#ifdef EVFILT_USER
#define SUPPORT_POLL_ONLY_FDS
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
registered_fds_node_feed_event(RegisteredFDsNode *fd2_node,
    EpollFDCtx *epollfd, struct kevent const *kev)
{
	assert(fd2_node->revents == 0);

	int revents = 0;

#ifdef SUPPORT_POLL_ONLY_FDS
	if (kev->filter == EVFILT_USER) {
		assert(fd2_node->node_type == NODE_TYPE_POLL);

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

	assert(kev->filter == EVFILT_READ || kev->filter == EVFILT_WRITE);
	assert((int)kev->ident == fd2_node->fd);

	if (kev->filter == EVFILT_READ) {
		revents |= EPOLLIN;

		if (kev->flags & EV_ONESHOT) {

#ifdef EV_FORCEONESHOT
			if (fd2_node->node_type == NODE_TYPE_SOCKET &&
			    fd2_node->node_data.socket.is_nycss) {
				if (is_not_yet_connected_stream_socket(
					fd2_node->fd)) {

					revents = EPOLLHUP;
					if (fd2_node->events & EPOLLOUT) {
						revents |= EPOLLOUT;
					}

					struct kevent nkev[2];
					EV_SET(&nkev[0], fd2_node->fd,
					    EVFILT_READ, EV_ADD, /**/
					    0, 0, fd2_node);
					EV_SET(&nkev[1], fd2_node->fd,
					    EVFILT_READ,
					    EV_ENABLE | EV_FORCEONESHOT, 0, 0,
					    fd2_node);

					kevent(epollfd->kq, nkev, 2, NULL, 0,
					    NULL);
				} else {
					fd2_node->node_data.socket.is_nycss =
					    false;

					struct kevent nkev[2];
					EV_SET(&nkev[0], fd2_node->fd,
					    EVFILT_READ, EV_ADD, /**/
					    0, 0, fd2_node);
					EV_SET(&nkev[1], fd2_node->fd,
					    EVFILT_READ,
					    fd2_node->events & EPOLLIN
						? EV_ENABLE
						: EV_DISABLE,
					    0, 0, fd2_node);

					kevent(epollfd->kq, nkev, 2, NULL, 0,
					    NULL);

					fd2_node->revents = 0;
					return true;
				}
			}
#endif
		}
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
				epoll_event = EPOLLERR;
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
				epoll_event = EPOLLIN;

				if (fd2_node->events & EPOLLRDHUP) {
					epoll_event |= EPOLLRDHUP;
				}
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
					if (fd2_node->events & EPOLLIN) {
						epoll_event |= EPOLLIN;
					}
					if (fd2_node->events & EPOLLRDHUP) {
						epoll_event |= EPOLLRDHUP;
					}

					epoll_event |= EPOLLHUP;
				}

				/* might as well steal flags from the
				 * poll call while we're here */

				if ((pfd.revents & POLLIN) &&
				    (fd2_node->events & EPOLLIN)) {
					epoll_event |= EPOLLIN;
				}

				if ((pfd.revents & POLLOUT) &&
				    (fd2_node->events & EPOLLOUT)) {
					epoll_event |= EPOLLOUT;
				}
			}
		} else {
			epoll_event = EPOLLHUP;
		}

		revents |= epoll_event;
	}

	assert(revents != 0);
	fd2_node->revents = (uint32_t)revents;

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

	/* Only sockets support EPOLLRDHUP. */
	if (fd2_node->node_type != NODE_TYPE_SOCKET) {
		fd2_node->events &= ~(uint32_t)EPOLLRDHUP;
	}

	int const fd2 = fd2_node->fd;
	struct kevent kev[2];

	assert(fd2 >= 0);

	if (!fd2_node->is_registered) {
		EV_SET(&kev[0], fd2, EVFILT_READ,
		    EV_ADD | (fd2_node->events & EPOLLIN ? 0 : EV_DISABLE), 0,
		    0, fd2_node);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    EV_ADD | (fd2_node->events & EPOLLOUT ? 0 : EV_DISABLE), 0,
		    0, fd2_node);
	} else {
		EV_SET(&kev[0], fd2, EVFILT_READ,
		    (fd2_node->events & EPOLLIN) ? EV_ENABLE : EV_DISABLE, 0,
		    0, fd2_node);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    (fd2_node->events & EPOLLOUT) ? EV_ENABLE : EV_DISABLE, 0,
		    0, fd2_node);
	}

	if (fd2_node->node_type != NODE_TYPE_POLL) {
		for (int i = 0; i < 2; ++i) {
			kev[i].flags |= EV_RECEIPT;
		}

		int const n = fd2_node->node_type == NODE_TYPE_KQUEUE ? 1 : 2;

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
		 kev[0].data == ENODEV && kev[1].data == ENODEV) ||
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

	/* Always have an error if EVFILT_READ registration failed. */
	if (kev[0].data != 0) {
		ec = (int)kev[0].data;
		goto out;
	}

	/*
	 * Ignore EVFILT_WRITE registration EINVAL errors (some fd
	 * types such as kqueues themselves don't support it).
	 *
	 * Note that NetBSD returns EPERM on EVFILT_WRITE registration
	 * failure for kqueues.
	 */
	if (fd2_node->node_type != NODE_TYPE_KQUEUE) {
		if (fd2_node->node_type == NODE_TYPE_FIFO &&
		    (kev[1].data == EINVAL || kev[1].data == EPERM)) {
			fd2_node->node_type = NODE_TYPE_KQUEUE;
		} else if (kev[1].data != 0) {
			ec = (int)kev[1].data;
			goto out;
		}
	}

#ifdef EV_FORCEONESHOT
	if (fd2_node->node_type == NODE_TYPE_SOCKET &&
	    is_not_yet_connected_stream_socket(fd2)) {
		EV_SET(&kev[0], fd2, EVFILT_READ, EV_ENABLE | EV_FORCEONESHOT,
		    0, 0, fd2_node);
		if (kevent(epollfd->kq, kev, 1, NULL, 0, NULL) < 0) {
			ec = errno;
			goto out;
		}

		fd2_node->node_data.socket.is_nycss = true;
	}
#endif

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
		EV_SET(&kev[0], (uintptr_t)fd2_node, EVFILT_USER,
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);

		EV_SET(&kev[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
		(void)kevent(epollfd->kq, kev, 1, NULL, 0, NULL);
	} else
#endif
	{
		struct kevent kev[2];
		EV_SET(&kev[0], fd2, EVFILT_READ, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, /**/
		    EV_DELETE | EV_RECEIPT, 0, 0, 0);
		(void)kevent(epollfd->kq, kev, 2, kev, 2, NULL);
	}

	RB_REMOVE(registered_fds_set_, &epollfd->registered_fds, fd2_node);

	registered_fds_node_destroy(fd2_node);
}

static errno_t
epollfd_ctx_add_node(EpollFDCtx *epollfd, int fd2, struct epoll_event *ev,
    struct stat const *statbuf)
{
	RegisteredFDsNode *fd2_node = registered_fds_node_create(fd2);
	if (!fd2_node) {
		return ENOMEM;
	}

	if (S_ISFIFO(statbuf->st_mode)) {
		/* May also be NODE_TYPE_KQUEUE,
		   will be checked when registering. */
		fd2_node->node_type = NODE_TYPE_FIFO;
	} else if (S_ISSOCK(statbuf->st_mode)) {
		fd2_node->node_type = NODE_TYPE_SOCKET;
	} else {
		/* May also be NODE_TYPE_POLL,
		   will be checked when registering. */
		fd2_node->node_type = NODE_TYPE_OTHER;
	}

	fd2_node->events = ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLOUT);
	fd2_node->data = ev->data;

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
		    EPOLLHUP | EPOLLERR))
		/* the user should really set one of EPOLLIN or EPOLLOUT
		 * so that EPOLLHUP and EPOLLERR work. Don't make this a
		 * hard error for now, though. */
		/* || !(ev->events & (EPOLLIN | EPOLLOUT)) */)) {
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
	struct kevent evlist[32];
	n = kevent(epollfd->kq, NULL, 0, evlist, cnt,
	    &(struct timespec){0, 0});
	if (n < 0) {
		return errno;
	}

	int j = 0;

	RegisteredFDsNode *del_list = NULL;

	for (int i = 0; i < n; ++i) {
		RegisteredFDsNode *fd2_node =
		    (RegisteredFDsNode *)evlist[i].udata;

#ifdef SUPPORT_POLL_ONLY_FDS
		if (!fd2_node) {
			assert(evlist[i].filter == EVFILT_USER);
			assert(evlist[i].udata == NULL);
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
			&evlist[i])) {
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
