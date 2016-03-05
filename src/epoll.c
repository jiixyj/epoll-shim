#include <sys/epoll.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <signal.h>

#if 0
int epoll_create(int size)
{
	return epoll_create1(0);
}
#endif

int
epoll_create1(int flags)
{
	if (flags != EPOLL_CLOEXEC) {
		errno = EINVAL;
		return -1;
	}

	return kqueue();
}

int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	struct kevent kev;
	if (op == EPOLL_CTL_ADD) {
		if (ev->events != EPOLLIN) {
			errno = EINVAL;
			return -1;
		}
		EV_SET(&kev, fd2, EVFILT_READ, EV_ADD, 0, 0, ev->data.ptr);
	} else if (op == EPOLL_CTL_DEL) {
		EV_SET(&kev, fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
	} else {
		errno = EINVAL;
		return -1;
	}

	int ret = kevent(fd, &kev, 1, NULL, 0, NULL);
	if (ret == -1) {
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
	if (cnt > 32 || to < -1) {
		errno = EINVAL;
		return -1;
	}

	struct kevent evlist[32];

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
		ev[i].events = EPOLLIN;
		ev[i].data.ptr = evlist[i].udata;
	}

	return ret;
}
