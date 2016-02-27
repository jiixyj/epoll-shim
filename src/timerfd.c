#include <sys/timerfd.h>
#undef read
#undef close

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <string.h>

static int timerfd_fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

int
timerfd_create(int clockid, int flags)
{
	if (clockid != CLOCK_MONOTONIC) {
		return EINVAL;
	}

	if (flags != (TFD_CLOEXEC | TFD_NONBLOCK)) {
		return EINVAL;
	}

	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == -1) {
			break;
		}
	}

	if (i == nitems(timerfd_fds)) {
		errno = EMFILE;
		return -1;
	}

	timerfd_fds[i] = kqueue();

	return timerfd_fds[i];
}

int
timerfd_settime(
    int fd, int flags, const struct itimerspec *new, struct itimerspec *old)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			break;
		}
	}

	if (i == nitems(timerfd_fds)) {
		errno = EINVAL;
		return -1;
	}

	if (flags != TFD_TIMER_ABSTIME) {
		errno = EINVAL;
		return -1;
	}

	if (old != NULL) {
		errno = EINVAL;
		return -1;
	}

	if (new == NULL) {
		errno = EINVAL;
		return -1;
	}

	// only allow oneshot timers for now
	if (new->it_interval.tv_sec != 0 || new->it_interval.tv_nsec != 0) {
		errno = EINVAL;
		return -1;
	}

	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
		return -1;
	}

	intptr_t millis_to_exp = 0;

	if (new->it_value.tv_sec > now.tv_sec ||
	    (new->it_value.tv_sec == now.tv_sec &&
		new->it_value.tv_nsec > now.tv_nsec)) {
		struct timespec timer_exp;
		timer_exp.tv_sec = new->it_value.tv_sec - now.tv_sec;
		timer_exp.tv_nsec = new->it_value.tv_nsec - now.tv_nsec;
		if (timer_exp.tv_nsec < 0) {
			timer_exp.tv_nsec += 1000000000;
			timer_exp.tv_sec -= 1;
		}
		millis_to_exp = timer_exp.tv_sec * 1000;
		millis_to_exp += timer_exp.tv_nsec / 1000 / 1000;
		if (timer_exp.tv_nsec % 1000000) {
			++millis_to_exp;
		}
	}

	struct kevent chlist[3];
	int nchanges = 0;

	// first, disarm the timer if one was already set
	EV_SET(&chlist[nchanges++], 0, EVFILT_TIMER, EV_ADD, 0, 0, 0);
	EV_SET(&chlist[nchanges++], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);

	if (new->it_value.tv_sec || new->it_value.tv_nsec) {
		EV_SET(&chlist[nchanges++], 0, EVFILT_TIMER,
		    EV_ADD | EV_ONESHOT, 0, millis_to_exp, NULL);
	}

	int ret = kevent(fd, chlist, nchanges, NULL, 0, NULL);
	if (ret == -1) {
		return ret;
	} else {
		return 0;
	}
}

#if 0
int timerfd_gettime(int fd, struct itimerspec *cur)
{
	return syscall(SYS_timerfd_gettime, fd, cur);
}
#endif

ssize_t
timerfd_read(int fd, void *buf, size_t nbytes)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			break;
		}
	}

	if (i == nitems(timerfd_fds)) {
		return read(fd, buf, nbytes);
	}

	if (nbytes < sizeof(uint64_t)) {
		return EINVAL;
	}

	struct timespec timeout = {0, 0};
	struct kevent kev;
	int ret = kevent(fd, NULL, 0, &kev, 1, &timeout);
	if (ret == -1) {
		return -1;
	} else if (ret == 0) {
		errno = EAGAIN;
		return -1;
	}

	if (ret != 1 || kev.data < 1) {
		errno = EIO;
		return -1;
	}

	uint64_t nr_expired = (uint64_t)kev.data;
	memcpy(buf, &nr_expired, sizeof(uint64_t));
	return sizeof(uint64_t);
}

int
timerfd_close(int fd)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			timerfd_fds[i] = -1;
			break;
		}
	}

	return close(fd);
}
