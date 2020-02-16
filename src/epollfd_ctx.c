#include "epollfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <assert.h>
#include <stdlib.h>

#include <poll.h>

RegisteredFDsNode *
registered_fds_node_create(int fd, struct epoll_event *ev)
{
	RegisteredFDsNode *node;

	node = malloc(sizeof(*node));
	if (!node) {
		return NULL;
	}

	node->fd = fd;
	node->flags = 0;
	node->eof_state = 0;
	node->data = ev->data;

	return node;
}

void
registered_fds_node_destroy(RegisteredFDsNode *node)
{
	free(node);
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

	epollfd->pfds[0].fd = -1;
	epollfd->pfds[1] = (struct pollfd){.fd = kq, .events = POLLIN};

	if ((ec = pthread_mutex_init(&epollfd->mutex, NULL)) != 0) {
		return ec;
	}

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

#define KQUEUE_STATE_EPOLLIN 0x2u
#define KQUEUE_STATE_EPOLLOUT 0x4u
#define KQUEUE_STATE_EPOLLRDHUP 0x8u
#define KQUEUE_STATE_NYCSS 0x10u
#define KQUEUE_STATE_ISFIFO 0x20u
#define KQUEUE_STATE_ISSOCK 0x40u

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

static errno_t
epollfd_ctx_ctl_impl(EpollFDCtx *epollfd, int op, int fd2,
    struct epoll_event *ev)
{
	errno_t ec = 0;
	struct kevent kev[2];

	assert(op == EPOLL_CTL_DEL || ev != NULL);

	if (epollfd->kq == fd2) {
		return EINVAL;
	}

	if (ev &&
	    ((ev->events &
		~(uint32_t)(EPOLLIN | EPOLLOUT | EPOLLHUP /**/
		    | EPOLLRDHUP | EPOLLERR))
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
		ec = errno;

		/* If the fstat fails for any reason we must clear
		 * internal state to avoid EEXIST errors in future
		 * calls to epoll_ctl. */
		if (fd2_node) {
			op = EPOLL_CTL_DEL;
			goto out;
		}

		return ec;
	}

	if (op != EPOLL_CTL_ADD && /**/
	    op != EPOLL_CTL_DEL && /**/
	    op != EPOLL_CTL_MOD) {
		return EINVAL;
	}

	uint16_t new_flags;

	if (op == EPOLL_CTL_ADD) {
		if (fd2_node) {
			return EEXIST;
		}

		new_flags = 0;

		if (S_ISFIFO(statbuf.st_mode)) {
			new_flags |= KQUEUE_STATE_ISFIFO;
		} else if (S_ISSOCK(statbuf.st_mode)) {
			new_flags |= KQUEUE_STATE_ISSOCK;
		}

		fd2_node = registered_fds_node_create(fd2, ev);
		if (!fd2_node) {
			return ENOMEM;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ,
		    EV_ADD | (ev->events & EPOLLIN ? 0 : EV_DISABLE), 0, 0,
		    fd2_node);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    EV_ADD | (ev->events & EPOLLOUT ? 0 : EV_DISABLE), 0, 0,
		    fd2_node);

#define SET_FLAG(flag)                                                        \
	do {                                                                  \
		if (ev->events & (flag)) {                                    \
			new_flags |= KQUEUE_STATE_##flag;                     \
		}                                                             \
	} while (0)

		SET_FLAG(EPOLLIN);
		SET_FLAG(EPOLLOUT);
		SET_FLAG(EPOLLRDHUP);

#undef SET_FLAG

	} else if (op == EPOLL_CTL_DEL) {
		if (!fd2_node) {
			return ENOENT;
		}

		if (fd2 >= 0 && fd2 == epollfd->pfds[0].fd) {
			goto out;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, EV_DELETE, 0, 0, 0);

	} else if (op == EPOLL_CTL_MOD) {
		if (!fd2_node) {
			return ENOENT;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ,
		    (ev->events & EPOLLIN) ? EV_ENABLE : EV_DISABLE, 0, 0,
		    fd2_node);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    (ev->events & EPOLLOUT) ? EV_ENABLE : EV_DISABLE, 0, 0,
		    fd2_node);

		new_flags = fd2_node->flags;

#define SET_FLAG(flag)                                                        \
	do {                                                                  \
		if (ev->events & (flag)) {                                    \
			new_flags |= KQUEUE_STATE_##flag;                     \
		} else {                                                      \
			new_flags &= ~KQUEUE_STATE_##flag;                    \
		}                                                             \
	} while (0)

		SET_FLAG(EPOLLIN);
		SET_FLAG(EPOLLOUT);
		SET_FLAG(EPOLLRDHUP);

#undef SET_FLAG

	} else {
		__builtin_unreachable();
	}

	for (int i = 0; i < 2; ++i) {
		kev[i].flags |= EV_RECEIPT;
	}

	int ret = kevent(epollfd->kq, kev, 2, kev, 2, NULL);
	if (ret < 0) {
		ec = errno;
		goto out;
	}

	if (ret != 2) {
		ec = EINVAL;
		goto out;
	}

	for (int i = 0; i < 2; ++i) {
		if (!(kev[i].flags & EV_ERROR)) {
			ec = EINVAL;
			goto out;
		}

		/* Check for fds that only support poll. */
		if (kev[i].data == ENODEV && op != EPOLL_CTL_DEL && fd2 >= 0 &&
		    !(ev->events & ~(uint32_t)(EPOLLIN | EPOLLOUT)) &&
		    (epollfd->pfds[0].fd < 0 || epollfd->pfds[0].fd == fd2)) {
			epollfd->pfds[0] = (struct pollfd){
			    .fd = fd2,
			    .events = ((ev->events & EPOLLIN) ? POLLIN : 0) |
				((ev->events & EPOLLOUT) ? POLLOUT : 0),
			};
			epollfd->pollfd_data = ev->data;

			goto out;
		}

		/*
		 * Ignore EVFILT_WRITE registration EINVAL errors (some fd
		 * types such as kqueues themselves don't support it).
		 * Also ignore ENOENT -- this happens when trying to remove a
		 * previously added fd where the EVFILT_WRITE registration
		 * failed.
		 *
		 * Note that NetBSD returns EPERM on EVFILT_WRITE registration
		 * failure.
		 */
		if (i == 1 &&
		    (kev[i].data == EINVAL || kev[i].data == EPERM ||
			kev[i].data == ENOENT)) {
			continue;
		}

		if (kev[i].data != 0) {
			ec = (int)kev[i].data;
			goto out;
		}
	}

#ifdef EV_FORCEONESHOT
	if (op != EPOLL_CTL_DEL && is_not_yet_connected_stream_socket(fd2)) {
		EV_SET(&kev[0], fd2, EVFILT_READ, EV_ENABLE | EV_FORCEONESHOT,
		    0, 0, fd2_node);
		if (kevent(epollfd->kq, kev, 1, NULL, 0, NULL) < 0) {
			ec = errno;
			goto out;
		}

		new_flags |= KQUEUE_STATE_NYCSS;
	}
#endif

out:
	if (op == EPOLL_CTL_ADD) {
		if (ec == 0) {
			fd2_node->flags = new_flags;

			if (RB_INSERT(registered_fds_set_,
				&epollfd->registered_fds, fd2_node)) {
				assert(0);
			}
		} else {
			registered_fds_node_destroy(fd2_node);
		}

	} else if (op == EPOLL_CTL_DEL) {
		RB_REMOVE(registered_fds_set_, /**/
		    &epollfd->registered_fds, fd2_node);
		registered_fds_node_destroy(fd2_node);

		if (fd2 >= 0 && fd2 == epollfd->pfds[0].fd) {
			epollfd->pfds[0].fd = -1;
		}

	} else if (op == EPOLL_CTL_MOD) {
		if (ec == 0) {
			fd2_node->flags = new_flags;
			fd2_node->data = ev->data;
		} else if (ec == ENOENT || ec == EBADF) {
			RB_REMOVE(registered_fds_set_, /**/
			    &epollfd->registered_fds, fd2_node);
			registered_fds_node_destroy(fd2_node);
		}

	} else {
		__builtin_unreachable();
	}

	return ec;
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

#define SUPPORTED_POLLFLAGS (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)

static uint32_t
poll_to_epoll(int flags)
{
	uint32_t epoll_flags = 0;

	if (flags & POLLIN) {
		epoll_flags |= EPOLLIN;
	}
	if (flags & POLLOUT) {
		epoll_flags |= EPOLLOUT;
	}
	if (flags & POLLERR) {
		epoll_flags |= EPOLLERR;
	}
	if (flags & POLLHUP) {
		epoll_flags |= EPOLLHUP;
	}
	if (flags & POLLNVAL) {
		epoll_flags |= EPOLLNVAL;
	}

	return epoll_flags;
}

static errno_t
epollfd_ctx_wait_impl(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt)
{
	if (cnt < 1 || cnt > 32) {
		return EINVAL;
	}

	int ret = poll(epollfd->pfds, 2, 0);
	if (ret < 0) {
		return errno;
	}
	if (ret == 0) {
		*actual_cnt = 0;
		return 0;
	}

	if (epollfd->pfds[0].revents & SUPPORTED_POLLFLAGS) {
		ev[0].events = poll_to_epoll(epollfd->pfds[0].revents);
		ev[0].data = epollfd->pollfd_data;
		*actual_cnt = 1;
		return 0;
	}

again:;
	struct kevent evlist[32];
	ret = kevent(epollfd->kq, NULL, 0, evlist, cnt,
	    &(struct timespec){0, 0});
	if (ret < 0) {
		return errno;
	}

	int j = 0;

	for (int i = 0; i < ret; ++i) {
		RegisteredFDsNode *fd2_node =
		    (RegisteredFDsNode *)evlist[i].udata;
		int events = 0;

		if (evlist[i].filter == EVFILT_READ) {
			events |= EPOLLIN;
			if (evlist[i].flags & EV_ONESHOT) {
				uint16_t flags = fd2_node->flags;

#ifdef EV_FORCEONESHOT
				if (flags & KQUEUE_STATE_NYCSS) {
					if (is_not_yet_connected_stream_socket(
						(int)evlist[i].ident)) {

						events = EPOLLHUP;
						if (flags &
						    KQUEUE_STATE_EPOLLOUT) {
							events |= EPOLLOUT;
						}

						struct kevent nkev[2];
						EV_SET(&nkev[0],
						    evlist[i].ident,
						    EVFILT_READ, EV_ADD, /**/
						    0, 0, fd2_node);
						EV_SET(&nkev[1],
						    evlist[i].ident,
						    EVFILT_READ,
						    EV_ENABLE |
							EV_FORCEONESHOT,
						    0, 0, fd2_node);

						kevent(epollfd->kq, nkev, 2,
						    NULL, 0, NULL);
					} else {
						flags &= ~KQUEUE_STATE_NYCSS;

						struct kevent nkev[2];
						EV_SET(&nkev[0],
						    evlist[i].ident,
						    EVFILT_READ, EV_ADD, /**/
						    0, 0, fd2_node);
						EV_SET(&nkev[1],
						    evlist[i].ident,
						    EVFILT_READ,
						    flags & KQUEUE_STATE_EPOLLIN
							? EV_ENABLE
							: EV_DISABLE,
						    0, 0, fd2_node);

						kevent(epollfd->kq, nkev, 2,
						    NULL, 0, NULL);

						fd2_node->flags = flags;

						continue;
					}
				}
#endif
			}
		} else if (evlist[i].filter == EVFILT_WRITE) {
			events |= EPOLLOUT;
		}

		if (evlist[i].filter == EVFILT_READ) {
			if (evlist[i].flags & EV_EOF) {
				fd2_node->eof_state |= EOF_STATE_READ_EOF;
			} else {
				fd2_node->eof_state &= ~EOF_STATE_READ_EOF;
			}
		} else if (evlist[i].filter == EVFILT_WRITE) {
			if (evlist[i].flags & EV_EOF) {
				fd2_node->eof_state |= EOF_STATE_WRITE_EOF;
			} else {
				fd2_node->eof_state &= ~EOF_STATE_WRITE_EOF;
			}
		}

		if (evlist[i].flags & EV_ERROR) {
			events |= EPOLLERR;
		}

		if (evlist[i].flags & EV_EOF) {
			if (evlist[i].fflags) {
				events |= EPOLLERR;
			}

			uint16_t flags = fd2_node->flags;

			int epoll_event;

			if (flags & KQUEUE_STATE_ISFIFO) {
				if (evlist[i].filter == EVFILT_READ) {
					epoll_event = EPOLLHUP;
					if (evlist[i].data == 0) {
						events &= ~EPOLLIN;
					}
				} else if (evlist[i].filter == EVFILT_WRITE) {
					epoll_event = EPOLLERR;
				} else {
					/* should not happen */
					assert(0);
					return EIO;
				}
			} else if (flags & KQUEUE_STATE_ISSOCK) {
				if (evlist[i].filter == EVFILT_READ) {
					/* do some special EPOLLRDHUP handling
					 * for sockets */

					/* if we are reading, we just know for
					 * sure that we can't receive any more,
					 * so use EPOLLIN/EPOLLRDHUP per
					 * default */
					epoll_event = EPOLLIN;

					if (flags & KQUEUE_STATE_EPOLLRDHUP) {
						epoll_event |= EPOLLRDHUP;
					}
				} else if (evlist[i].filter == EVFILT_WRITE) {
					epoll_event = EPOLLOUT;
				} else {
					/* should not happen */
					assert(0);
					return EIO;
				}

				struct pollfd pfd = {
				    .fd = (int)evlist[i].ident,
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
						if (flags &
						    KQUEUE_STATE_EPOLLIN) {
							epoll_event |= EPOLLIN;
						}
						if (flags &
						    KQUEUE_STATE_EPOLLRDHUP) {
							epoll_event |=
							    EPOLLRDHUP;
						}

						epoll_event |= EPOLLHUP;
					}

					/* might as well steal flags from the
					 * poll call while we're here */

					if ((pfd.revents & POLLIN) &&
					    (flags & KQUEUE_STATE_EPOLLIN)) {
						epoll_event |= EPOLLIN;
					}

					if ((pfd.revents & POLLOUT) &&
					    (flags & KQUEUE_STATE_EPOLLOUT)) {
						epoll_event |= EPOLLOUT;
					}
				}
			} else {
				epoll_event = EPOLLHUP;
			}

			events |= epoll_event;
		}
		ev[j].events = (uint32_t)events;
		ev[j].data = fd2_node->data;
		++j;
	}

	if (ret && j == 0) {
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
