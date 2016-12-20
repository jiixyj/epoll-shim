#include <sys/epoll.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/queue.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#if 1
int epoll_create(int size)
{
	fprintf(stderr, "ERROR: epoll_create() is deprecated, use epoll_create1(EPOLL_CLOEXEC).\n");
	exit(-1);
	/* return epoll_create1(0); */
}
#endif

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

static int
epoll_kevent_set(int fd, uintptr_t ident, short filter, u_short flags,
				 u_int fflags, intptr_t data, void *udata)
{
	int ret = 0;
	struct kevent kev;
	EV_SET(&kev, ident, filter, flags, fflags, data, udata);
	ret = kevent(fd, &kev, 1, NULL, 0, NULL);
	return ret;
}


static int
epoll_ctl_add(int fd, int fd2, struct epoll_event *ev)
{
	struct kevent kev;
	int ret = 0;
	if(ev->events & EPOLLIN) {
		ret = epoll_kevent_set(fd, fd2, EVFILT_READ, EV_ADD, 0, 0, ev->data.ptr);
	}
	if(ret < 0)
		return ret;
	if(ev->events & EPOLLOUT) {
		ret = epoll_kevent_set(fd, fd2, EVFILT_WRITE, EV_ADD, 0, 0, ev->data.ptr);
	}
	return ret;
}

static int
epoll_ctl_del(int fd, int fd2, struct epoll_event *ev)
{
	int ret = 0;
	// This will fail (return -1) if we try to delete a non-existing
	// event. This can happen so ignore return value for now.
	ret = epoll_kevent_set(fd, fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
	ret = epoll_kevent_set(fd, fd2, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
	// TODO: Check if event exist before attemt to delete. Is there an
	// API for that or do we need to keep track of <ident,filter> events
	// we added to the queue?
	return 0;
}

int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	int ret = 0;
	if (op == EPOLL_CTL_ADD) {
		ret = epoll_ctl_add(fd, fd2, ev);
	} else if (op == EPOLL_CTL_DEL) {
		if (poll_fd == fd2 && fd == poll_epoll_fd) {
			poll_fd = -1;
			poll_epoll_fd = -1;
			poll_ptr = NULL;
			return 0;
		} else {
			ret = epoll_ctl_del(fd, fd2, ev);
		}
	} else if (op == EPOLL_CTL_MOD) {
		if(ev->events & EPOLLIN && ev->events & EPOLLOUT) {
			// Adding both EVFILT_READ and EVFILT_WRITE
			// Existing events will be modified.
			ret = epoll_ctl_add(fd, fd2, ev);
		} else if(ev->events & EPOLLOUT == 0) {
			// Is it OK to assume this?
			ret = epoll_kevent_set(fd, fd2, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
			// Returns -1 if event does not exist so ignore return value for now.
			ret = 0;
		} else if(ev->events & EPOLLIN == 0) {
			// Is it OK to assume this?
			ret = epoll_kevent_set(fd, fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
			// Returns -1 if event does not exist so ignore return value for now.
			ret = 0;
		}
	} else {
		errno = EINVAL;
		return -1;
	}
	if (ret == -1) {
		if (errno == ENODEV && poll_fd == -1) {
			poll_fd = fd2;
			poll_epoll_fd = fd;
			poll_ptr = ev->data.ptr;
			return 0;
		}
		return ret;
	} else {
		return 0;
	}
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

int
epoll_wait(int fd, struct epoll_event *ev, int cnt, int to)
{
	if (cnt < 1 || cnt > 32 || to < -1) {
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

	int ret = kevent(fd, NULL, 0, evlist, cnt, to == -1 ? NULL : &timeout);
	if (ret == -1) {
		return ret;
	}

	for (int i = 0; i < ret; ++i) {
		int events = 0;
		if(evlist[i].filter == EVFILT_READ) {
			events |= EPOLLIN;
		} else if(evlist[i].filter == EVFILT_WRITE) {
			events |= EPOLLOUT;
		}
		if(evlist[i].flags & EV_ERROR) {
			events |= EPOLLERR;
		}
		if(evlist[i].flags & EV_EOF) {
			events |= EPOLLHUP;
		}
		ev[i].events = events;
		ev[i].data.ptr = evlist[i].udata;
	}
	return ret;
}
