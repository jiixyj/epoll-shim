#include <sys/timerfd.h>
#undef read
#undef close

#include <sys/event.h>
#include <sys/select.h>
#include <sys/timespec.h>

#include <pthread.h>
#include <pthread_np.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum timerfd_kind {
	TIMERFD_KIND_UNDETERMINED,
	TIMERFD_KIND_SIMPLE,
	TIMERFD_KIND_COMPLEX,
};

struct timerfd_context {
	int fd;
	int flags;
	enum timerfd_kind kind;
	union {
		struct {
			struct itimerspec current_itimerspec;
		} simple;
		struct {
			pthread_t worker;
			timer_t timer;
			uint64_t current_expirations;
		} complex;
	};
	struct timerfd_context *next;
};

static struct timerfd_context *timerfd_contexts;
pthread_mutex_t timerfd_context_mtx = PTHREAD_MUTEX_INITIALIZER;

struct timerfd_context *
get_timerfd_context(int fd, bool create_new)
{
	for (struct timerfd_context *ctx = timerfd_contexts; ctx;
	     ctx = ctx->next) {
		if (fd == ctx->fd) {
			return ctx;
		}
	}

	if (create_new) {
		struct timerfd_context *new_ctx =
		    malloc(sizeof(struct timerfd_context));
		if (!new_ctx) {
			return NULL;
		}

		*new_ctx = (struct timerfd_context){
		    .fd = -1,
		    .next = timerfd_contexts,
		};
		timerfd_contexts = new_ctx;

		return new_ctx;
	}

	return NULL;
}

static void *
worker_function(void *arg)
{
	struct timerfd_context *ctx = arg;

	uint64_t total_expirations = 0;

	siginfo_t info;
	sigset_t rt_set;
	sigset_t block_set;

	sigemptyset(&rt_set);
	sigaddset(&rt_set, SIGRTMIN);
	sigaddset(&rt_set, SIGRTMIN + 1);

	sigfillset(&block_set);

	(void)pthread_sigmask(SIG_BLOCK, &block_set, NULL);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
	    (void *)(intptr_t)pthread_getthreadid_np());
	(void)kevent(ctx->fd, &kev, 1, NULL, 0, NULL);

	for (;;) {
		if (sigwaitinfo(&rt_set, &info) != SIGRTMIN) {
			break;
		}
		total_expirations += 1 + timer_getoverrun(ctx->complex.timer);
		EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
		    (void *)(uintptr_t)total_expirations);
		(void)kevent(ctx->fd, &kev, 1, NULL, 0, NULL);
	}

	return NULL;
}

static errno_t
upgrade_to_complex_timer(struct timerfd_context *ctx, int clockid)
{
	errno_t err;

	if (ctx->kind == TIMERFD_KIND_COMPLEX) {
		return 0;
	}

	if (ctx->kind == TIMERFD_KIND_SIMPLE) {
		struct kevent kev[1];
		EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
		(void)kevent(ctx->fd, kev, nitems(kev), NULL, 0, NULL);

		ctx->kind = TIMERFD_KIND_UNDETERMINED;
	}

	assert(ctx->kind == TIMERFD_KIND_UNDETERMINED);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (kevent(ctx->fd, &kev, 1, NULL, 0, NULL) < 0) {
		assert(errno != 0);
		return errno;
	}

	if ((err = pthread_create(&ctx->complex.worker, /**/
		 NULL, worker_function, ctx)) != 0) {
		return err;
	}

	if (kevent(ctx->fd, NULL, 0, &kev, 1, NULL) < 0) {
		goto f0;
	}

	int tid = (int)(intptr_t)kev.udata;

	struct sigevent sigev = {.sigev_notify = SIGEV_THREAD_ID,
	    .sigev_signo = SIGRTMIN,
	    .sigev_notify_thread_id = tid};

	if (timer_create(clockid, &sigev, &ctx->complex.timer) < 0) {
		goto f0;
	}

	ctx->complex.current_expirations = 0;
	ctx->kind = TIMERFD_KIND_COMPLEX;
	return 0;

f0:
	assert(errno != 0);
	err = errno;
	pthread_kill(ctx->complex.worker, SIGRTMIN + 1);
	pthread_join(ctx->complex.worker, NULL);
	return err;
}

static errno_t
timerfd_create_impl(int clockid, int flags, int *fd)
{
	errno_t err;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		return EINVAL;
	}

	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		return EINVAL;
	}

	struct timerfd_context *ctx = get_timerfd_context(-1, true);
	if (!ctx) {
		return ENOMEM;
	}

	ctx->fd = kqueue();
	if (ctx->fd < 0) {
		assert(errno != 0);
		return errno;
	}

	ctx->flags = flags;
	ctx->kind = TIMERFD_KIND_UNDETERMINED;

	if (clockid == CLOCK_REALTIME) {
		if ((err = upgrade_to_complex_timer(ctx, /**/
			 CLOCK_REALTIME)) != 0) {
			goto f0;
		}
	}

	*fd = ctx->fd;
	return 0;

f0:
	close(ctx->fd);
	ctx->fd = -1;
	return err;
}

int
timerfd_create(int clockid, int flags)
{
	int fd;
	errno_t err;

	pthread_mutex_lock(&timerfd_context_mtx);
	err = timerfd_create_impl(clockid, flags, &fd);
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (err != 0) {
		errno = err;
		return -1;
	}

	return fd;
}

static errno_t
timerfd_settime_impl(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t err;
	struct timerfd_context *ctx;

	if (!new) {
		return EFAULT;
	}

	ctx = get_timerfd_context(fd, false);
	if (!ctx) {
		return EINVAL;
	}

	if (flags & ~(TFD_TIMER_ABSTIME)) {
		return EINVAL;
	}

	if ((flags & TFD_TIMER_ABSTIME) ||
	    ((new->it_interval.tv_sec != 0 || new->it_interval.tv_nsec != 0) &&
		(new->it_interval.tv_sec != new->it_value.tv_sec ||
		    new->it_interval.tv_nsec != new->it_value.tv_nsec))) {
		if ((err = upgrade_to_complex_timer(ctx, /**/
			 CLOCK_MONOTONIC)) != 0) {
			return err;
		}
	}

	if (ctx->kind == TIMERFD_KIND_COMPLEX) {
		if (timer_settime(ctx->complex.timer,
			(flags & TFD_TIMER_ABSTIME) ? TIMER_ABSTIME : 0, /**/
			new, old) < 0) {
			return errno;
		}
	} else {
		struct kevent kev[1];
		int oneshot_flag;
		int64_t micros;

		if (old) {
			*old = ctx->simple.current_itimerspec;
		}

		if (new->it_value.tv_sec == 0 && new->it_value.tv_nsec == 0) {
			struct kevent kev[1];
			EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
			(void)kevent(ctx->fd, kev, nitems(kev), NULL, 0, NULL);
		} else {
			if (__builtin_mul_overflow(new->it_value.tv_sec,
				1000000, &micros) ||
			    __builtin_add_overflow(micros,
				new->it_value.tv_nsec / 1000, &micros)) {
				return EOVERFLOW;
			}

			if ((new->it_value.tv_nsec % 1000) &&
			    __builtin_add_overflow(micros, 1, &micros)) {
				return EOVERFLOW;
			}

			if (new->it_interval.tv_sec == 0 &&
			    new->it_interval.tv_nsec == 0) {
				oneshot_flag = EV_ONESHOT;
			} else {
				oneshot_flag = 0;
			}

			EV_SET(&kev[0], 0, EVFILT_TIMER, EV_ADD | oneshot_flag,
			    NOTE_USECONDS, micros, 0);

			if (kevent(ctx->fd, kev, nitems(kev), /**/
				NULL, 0, NULL) < 0) {
				return errno;
			}
		}

		ctx->simple.current_itimerspec = *new;
		ctx->kind = TIMERFD_KIND_SIMPLE;
	}

	return 0;
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new,
    struct itimerspec *old)
{
	errno_t err;

	pthread_mutex_lock(&timerfd_context_mtx);
	err = timerfd_settime_impl(fd, flags, new, old);
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (err != 0) {
		errno = err;
		return -1;
	}

	return 0;
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
	int fd = ctx->fd;
	int flags = ctx->flags;
	enum timerfd_kind kind = ctx->kind;
	pthread_mutex_unlock(&timerfd_context_mtx);

	if (nbytes < sizeof(uint64_t)) {
		errno = EINVAL;
		return -1;
	}

	uint64_t nr_expired;
	for (;;) {
		struct timespec timeout = {0, 0};
		struct kevent kev;
		int ret = kevent(fd, NULL, 0, &kev, 1,
		    (flags & TFD_NONBLOCK) ? &timeout : NULL);
		if (ret < 0) {
			return -1;
		}

		if (ret == 0) {
			errno = EAGAIN;
			return -1;
		}

		if (kind == TIMERFD_KIND_COMPLEX) {
			uint64_t expired_new = (uint64_t)kev.udata;

			/* TODO(jan): Should replace this with a
			 * per-timerfd_context mutex. */
			pthread_mutex_lock(&timerfd_context_mtx);
			if (expired_new > ctx->complex.current_expirations) {
				nr_expired = expired_new -
				    ctx->complex.current_expirations;
				ctx->complex.current_expirations = expired_new;
			} else {
				nr_expired = 0;
			}
			pthread_mutex_unlock(&timerfd_context_mtx);
		} else {
			nr_expired = kev.data;
		}

		if (nr_expired != 0) {
			break;
		}
	}

	memcpy(buf, &nr_expired, sizeof(uint64_t));

	return sizeof(uint64_t);
}

int
timerfd_close(struct timerfd_context *ctx)
{
	if (ctx->kind == TIMERFD_KIND_COMPLEX) {
		timer_delete(ctx->complex.timer);
		pthread_kill(ctx->complex.worker, SIGRTMIN + 1);
		pthread_join(ctx->complex.worker, NULL);
	}
	int ret = close(ctx->fd);
	ctx->fd = -1;
	return ret;
}
