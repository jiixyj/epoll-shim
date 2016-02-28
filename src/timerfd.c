#include <sys/timerfd.h>
#undef read
#undef close

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <pthread.h>
#include <pthread_np.h>

#include <errno.h>
#include <signal.h>
#include <string.h>

int timerfd_fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static pthread_t timerfd_workers[8];
static timer_t timerfd_timers[8];
static int timerfd_clockid[8];
static int timerfd_flags[8];

static void
timer_notify_func(union sigval arg)
{
	int kq = arg.sival_int;

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
	(void)kevent(kq, &kev, 1, NULL, 0, NULL);
}

static void *
worker_function(void *arg)
{
	int i = (int)(intptr_t)arg;

	siginfo_t info;
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN);
	sigaddset(&set, SIGRTMIN + 1);
	(void)pthread_sigmask(SIG_BLOCK, &set, NULL);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
	    (void *)(intptr_t)pthread_getthreadid_np());
	(void)kevent(timerfd_fds[i], &kev, 1, NULL, 0, NULL);

	for (;;) {
		if (sigwaitinfo(&set, &info) != SIGRTMIN) {
			break;
		}
		EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
		    (void *)(intptr_t)timer_getoverrun(timerfd_timers[i]));
		(void)kevent(timerfd_fds[i], &kev, 1, NULL, 0, NULL);
	}

	return NULL;
}

int
timerfd_create(int clockid, int flags)
{
	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		return EINVAL;
	}

	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
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
	if (timerfd_fds[i] == -1) {
		return -1;
	}

	timerfd_flags[i] = flags;

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (kevent(timerfd_fds[i], &kev, 1, NULL, 0, NULL) == -1) {
		close(timerfd_fds[i]);
		timerfd_fds[i] = -1;
		return -1;
	}

	if (pthread_create(&timerfd_workers[i], NULL, worker_function,
		(void *)(intptr_t)i) == -1) {
		close(timerfd_fds[i]);
		timerfd_fds[i] = -1;
		return -1;
	}

	int ret = kevent(timerfd_fds[i], NULL, 0, &kev, 1, NULL);
	if (ret == -1) {
		pthread_kill(timerfd_workers[i], SIGRTMIN + 1);
		pthread_join(timerfd_workers[i], NULL);
		close(timerfd_fds[i]);
		timerfd_fds[i] = -1;
		return -1;
	}

	int tid = (int)kev.udata;

	struct sigevent sigev = {.sigev_notify = SIGEV_THREAD_ID,
	    .sigev_signo = SIGRTMIN,
	    .sigev_notify_thread_id = tid};

	if (timer_create(clockid, &sigev, &timerfd_timers[i]) == -1) {
		pthread_kill(timerfd_workers[i], SIGRTMIN + 1);
		pthread_join(timerfd_workers[i], NULL);
		close(timerfd_fds[i]);
		timerfd_fds[i] = -1;
		return -1;
	}

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

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		errno = EINVAL;
		return -1;
	}

	return timer_settime(timerfd_timers[i],
	    (flags & TFD_TIMER_ABSTIME) ? TIMER_ABSTIME : 0, new, old);
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
		errno = EINVAL;
		return -1;
	}

	struct timespec timeout = {0, 0};
	struct kevent kev;
	int ret = kevent(fd, NULL, 0, &kev, 1,
	    (timerfd_flags[i] & TFD_NONBLOCK) ? &timeout : NULL);
	if (ret == -1) {
		return -1;
	} else if (ret == 0) {
		errno = EAGAIN;
		return -1;
	}

	uint64_t nr_expired = 1 + (int)kev.udata;
	memcpy(buf, &nr_expired, sizeof(uint64_t));

	return sizeof(uint64_t);
}

int
timerfd_close(int fd)
{
	unsigned i;
	for (i = 0; i < nitems(timerfd_fds); ++i) {
		if (timerfd_fds[i] == fd) {
			timer_delete(timerfd_timers[i]);
			pthread_kill(timerfd_workers[i], SIGRTMIN + 1);
			pthread_join(timerfd_workers[i], NULL);
			timerfd_fds[i] = -1;
			break;
		}
	}

	return close(fd);
}
