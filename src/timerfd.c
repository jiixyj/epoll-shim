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

struct timerfd_context {
	int fd;
	pthread_t worker;
	timer_t timer;
	int flags;
};

static struct timerfd_context timerfd_contexts[8] = {{-1, 0, 0, 0},
    {-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0},
    {-1, 0, 0, 0}, {-1, 0, 0, 0}};

struct timerfd_context *
get_timerfd_context(int fd)
{
	for (unsigned i = 0; i < nitems(timerfd_contexts); ++i) {
		if (fd == timerfd_contexts[i].fd) {
			return &timerfd_contexts[i];
		}
	}
	return NULL;
}

static void *
worker_function(void *arg)
{
	struct timerfd_context *ctx = arg;

	siginfo_t info;
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN);
	sigaddset(&set, SIGRTMIN + 1);
	(void)pthread_sigmask(SIG_BLOCK, &set, NULL);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
	    (void *)(intptr_t)pthread_getthreadid_np());
	(void)kevent(ctx->fd, &kev, 1, NULL, 0, NULL);

	for (;;) {
		if (sigwaitinfo(&set, &info) != SIGRTMIN) {
			break;
		}
		EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
		    (void *)(intptr_t)timer_getoverrun(ctx->timer));
		(void)kevent(ctx->fd, &kev, 1, NULL, 0, NULL);
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

	struct timerfd_context *ctx = get_timerfd_context(-1);
	if (!ctx) {
		errno = EMFILE;
		return -1;
	}

	ctx->fd = kqueue();
	if (ctx->fd == -1) {
		return -1;
	}

	ctx->flags = flags;

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (kevent(ctx->fd, &kev, 1, NULL, 0, NULL) == -1) {
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	if (pthread_create(&ctx->worker, NULL, worker_function, ctx) == -1) {
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	int ret = kevent(ctx->fd, NULL, 0, &kev, 1, NULL);
	if (ret == -1) {
		pthread_kill(ctx->worker, SIGRTMIN + 1);
		pthread_join(ctx->worker, NULL);
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	int tid = (int)kev.udata;

	struct sigevent sigev = {.sigev_notify = SIGEV_THREAD_ID,
	    .sigev_signo = SIGRTMIN,
	    .sigev_notify_thread_id = tid};

	if (timer_create(clockid, &sigev, &ctx->timer) == -1) {
		pthread_kill(ctx->worker, SIGRTMIN + 1);
		pthread_join(ctx->worker, NULL);
		close(ctx->fd);
		ctx->fd = -1;
		return -1;
	}

	return ctx->fd;
}

int
timerfd_settime(
    int fd, int flags, const struct itimerspec *new, struct itimerspec *old)
{
	struct timerfd_context *ctx = get_timerfd_context(fd);
	if (!ctx) {
		errno = EINVAL;
		return -1;
	}

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		errno = EINVAL;
		return -1;
	}

	return timer_settime(ctx->timer,
	    (flags & TFD_TIMER_ABSTIME) ? TIMER_ABSTIME : 0, new, old);
}

#if 0
int timerfd_gettime(int fd, struct itimerspec *cur)
{
	return syscall(SYS_timerfd_gettime, fd, cur);
}
#endif

ssize_t
timerfd_read(struct timerfd_context *ctx, void *buf, size_t nbytes)
{
	if (nbytes < sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	struct timespec timeout = {0, 0};
	struct kevent kev;
	int ret = kevent(ctx->fd, NULL, 0, &kev, 1,
	    (ctx->flags & TFD_NONBLOCK) ? &timeout : NULL);
	if (ret == -1) {
		return -1;
	} else if (ret == 0) {
		errno = EAGAIN;
		return -1;
	}

	uint64_t nr_expired = 1 + (uint64_t)kev.udata;
	memcpy(buf, &nr_expired, sizeof(uint64_t));

	return sizeof(uint64_t);
}

int
timerfd_close(struct timerfd_context *ctx)
{
	timer_delete(ctx->timer);
	pthread_kill(ctx->worker, SIGRTMIN + 1);
	pthread_join(ctx->worker, NULL);
	ctx->fd = -1;
	return close(ctx->fd);
}
