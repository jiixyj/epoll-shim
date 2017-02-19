#include <sys/epoll.h>

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
epoll_create(int size __unused)
{
	fprintf(stderr,
	    "ERROR: epoll_create() is deprecated, use "
	    "epoll_create1(EPOLL_CLOEXEC).\n");
	errno = EINVAL;
	return -1;
}

int
epoll_create1(int flags)
{
	if (flags != EPOLL_CLOEXEC) {
		fprintf(stderr, "ERROR: Use epoll_create1(EPOLL_CLOEXEC).\n");
		errno = EINVAL;
		return -1;
	}
	return kqueue();
}

static int poll_fd = -1;
static int poll_epoll_fd = -1;
static void *poll_ptr;

int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	if ((!ev && op != EPOLL_CTL_DEL) ||
	    (ev &&
		(ev->events &
		    ~(EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP |
			EPOLLERR)))) {
		errno = EINVAL;
		return -1;
	}

	struct kevent kev[3];

	if (op == EPOLL_CTL_ADD) {
		/* Check if the fd already has been registered in this kqueue.
		 * See below for an explanation of this 'cookie' mechanism. */
		EV_SET(&kev[0], fd2, EVFILT_USER, 0, 0, 0, 0);
		if (!(kevent(fd, kev, 1, NULL, 0, NULL) == -1 &&
			errno == ENOENT)) {
			errno = EEXIST;
			return -1;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ,
		    EV_ADD | (ev->events & EPOLLIN ? 0 : EV_DISABLE), 0, 0,
		    ev->data.ptr);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    EV_ADD | (ev->events & EPOLLOUT ? 0 : EV_DISABLE), 0, 0,
		    ev->data.ptr);
		/* We save a 'cookie' knote inside the kq to signal if the fd
		 * has been 'registered'. We need this because there is no way
		 * to ask a kqueue if a knote has been registered without
		 * modifying the udata. */
		EV_SET(&kev[2], fd2, EVFILT_USER, EV_ADD,
		    ev->events & EPOLLRDHUP ? 1 : 0, 0, 0);
	} else if (op == EPOLL_CTL_DEL) {
		if (poll_fd == fd2 && fd == poll_epoll_fd) {
			poll_fd = -1;
			poll_epoll_fd = -1;
			poll_ptr = NULL;
			return 0;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
		EV_SET(&kev[2], fd2, EVFILT_USER, EV_DELETE, 0, 0, 0);
	} else if (op == EPOLL_CTL_MOD) {
		EV_SET(&kev[0], fd2, EVFILT_READ,
		    ev->events & EPOLLIN ? EV_ENABLE : EV_DISABLE, 0, 0,
		    ev->data.ptr);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    ev->events & EPOLLOUT ? EV_ENABLE : EV_DISABLE, 0, 0,
		    ev->data.ptr);
		/* we don't really need this, but now we have 3 kevents in all
		 * branches which is nice */
		EV_SET(&kev[2], fd2, EVFILT_USER, 0,
		    NOTE_FFCOPY | (ev->events & EPOLLRDHUP ? 1 : 0), 0, 0);
	} else {
		errno = EINVAL;
		return -1;
	}

	for (int i = 0; i < 3; ++i) {
		kev[i].flags |= EV_RECEIPT;
	}

	int ret = kevent(fd, kev, 3, kev, 3, NULL);
	if (ret == -1) {
		return -1;
	}

	if (ret != 3) {
		errno = EINVAL;
		return -1;
	}

	for (int i = 0; i < 3; ++i) {
		if (kev[i].flags != EV_ERROR) {
			errno = EINVAL;
			return -1;
		}

		if (kev[i].data == ENODEV && poll_fd == -1) {
			poll_fd = fd2;
			poll_epoll_fd = fd;
			poll_ptr = ev->data.ptr;
			return 0;
		}

		/* ignore EVFILT_WRITE registration EINVAL errors (some fd
		 * types such as kqueues themselves don't support it) */
		if (i == 1 && kev[i].data == EINVAL) {
			continue;
		}

		if (kev[i].data != 0) {
			errno = kev[i].data;
			return -1;
		}
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

static u_int
get_fflags(int kq, int fd)
{
	struct kevent kev;
	EV_SET(&kev, fd, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
	if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1) {
		return -1;
	}

	for (;;) {
		struct timespec timeout = {0, 0};

		if (kevent(kq, NULL, 0, &kev, 1, &timeout) == -1) {
			return -1;
		}

		// fprintf(stderr, "ev user: %d %d %d\n", (int)kev.filter,
		//     (int)kev.fflags, (int)kev.udata);

		if (kev.filter == EVFILT_USER) {
			u_int fflags = kev.fflags;

			EV_SET(&kev, fd, EVFILT_USER, EV_CLEAR, 0, 0, 0);
			if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1) {
				return -1;
			}

			return fflags;
		}
	}
}

int
epoll_wait(int fd, struct epoll_event *ev, int cnt, int to)
{
	if (cnt < 1 || cnt > 32) {
		errno = EINVAL;
		return -1;
	}

	if (poll_fd != -1 && fd == poll_epoll_fd) {
		struct pollfd pfds[2];
		pfds[0].fd = poll_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = fd;
		pfds[1].events = POLLIN;
		int ret = poll(pfds, 2, to);
		if (ret <= 0) {
			return ret;
		}
		if (pfds[0].revents & POLLIN) {
			ev[0].events = EPOLLIN;
			ev[0].data.ptr = poll_ptr;
			return 1;
		}
		to = 0;
	}

	struct kevent evlist[cnt];
	memset(evlist, 0, sizeof(evlist));

	struct timespec timeout = {0, 0};
	if (to > 0) {
		timeout.tv_sec = to / 1000;
		timeout.tv_nsec = (to % 1000) * 1000 * 1000;
	}

	struct timespec *ptimeout = NULL;
	if (to >= 0) {
		ptimeout = &timeout;
	}

	int ret = kevent(fd, NULL, 0, evlist, cnt, ptimeout);
	if (ret == -1) {
		return ret;
	}

	for (int i = 0; i < ret; ++i) {
		int events = 0;
		if (evlist[i].filter == EVFILT_READ) {
			events |= EPOLLIN;
		} else if (evlist[i].filter == EVFILT_WRITE) {
			events |= EPOLLOUT;
		}
		if (evlist[i].flags & EV_ERROR) {
			events |= EPOLLERR;
		}
		if (evlist[i].flags & EV_EOF) {
			int epoll_event = EPOLLHUP;

			struct stat statbuf;
			if (fstat(evlist[i].ident, &statbuf) == -1) {
				return -1;
			}

			/* do some special EPOLLRDHUP handling for sockets */
			if ((statbuf.st_mode & S_IFSOCK) &&
			    evlist[i].filter == EVFILT_READ) {
				/* if we are reading, we just know for sure
				 * that we can't receive any more, so use
				 * EPOLLRDHUP per default */
				epoll_event = get_fflags(fd, evlist[i].ident)
				    ? EPOLLRDHUP
				    : 0;

				/* only set EPOLLHUP if the stat says that
				 * writing is also impossible */
				if (!(statbuf.st_mode &
					(S_IWUSR | S_IWGRP | S_IWOTH))) {
					epoll_event |= EPOLLHUP;
				}
			}

			events |= epoll_event;
		}
		ev[i].events = events;
		ev[i].data.ptr = evlist[i].udata;
	}
	return ret;
}
