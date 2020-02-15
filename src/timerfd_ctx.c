#include "timerfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <assert.h>
#include <signal.h>

#include <pthread_np.h>

#ifndef timespeccmp
#define timespeccmp(tvp, uvp, cmp)                                            \
	(((tvp)->tv_sec == (uvp)->tv_sec)                                     \
		? ((tvp)->tv_nsec cmp(uvp)->tv_nsec)                          \
		: ((tvp)->tv_sec cmp(uvp)->tv_sec))
#endif

#ifndef timespecsub
#define timespecsub(tsp, usp, vsp)                                            \
	do {                                                                  \
		(vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;                \
		(vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;             \
		if ((vsp)->tv_nsec < 0) {                                     \
			(vsp)->tv_sec--;                                      \
			(vsp)->tv_nsec += 1000000000L;                        \
		}                                                             \
	} while (0)
#endif

static bool
timespec_is_valid(struct timespec const *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

static bool
itimerspec_is_valid(struct itimerspec const *its)
{
	return timespec_is_valid(&its->it_value) &&
	    timespec_is_valid(&its->it_interval);
}

static errno_t
timespecadd_safe(struct timespec const *tsp, struct timespec const *usp,
    struct timespec *vsp)
{
	assert(timespec_is_valid(tsp));
	assert(timespec_is_valid(usp));

	if (__builtin_add_overflow(tsp->tv_sec, usp->tv_sec, &vsp->tv_sec)) {
		return EINVAL;
	}
	vsp->tv_nsec = tsp->tv_nsec + usp->tv_nsec;

	if (vsp->tv_nsec >= 1000000000L) {
		if (__builtin_add_overflow(vsp->tv_sec, 1, &vsp->tv_sec)) {
			return EINVAL;
		}
		vsp->tv_nsec -= 1000000000L;
	}

	return 0;
}

static errno_t
timespecsub_safe(struct timespec const *tsp, struct timespec const *usp,
    struct timespec *vsp)
{
	assert(timespec_is_valid(tsp));
	assert(timespec_is_valid(usp));

	if (__builtin_sub_overflow(tsp->tv_sec, usp->tv_sec, &vsp->tv_sec)) {
		return EINVAL;
	}
	vsp->tv_nsec = tsp->tv_nsec - usp->tv_nsec;

	if (vsp->tv_nsec < 0) {
		if (__builtin_sub_overflow(vsp->tv_sec, 1, &vsp->tv_sec)) {
			return EINVAL;
		}
		vsp->tv_nsec += 1000000000L;
	}

	return 0;
}

static bool
timerfd_ctx_is_disarmed(TimerFDCtx const *timerfd)
{
	return /**/
	    timerfd->current_itimerspec.it_value.tv_sec == 0 &&
	    timerfd->current_itimerspec.it_value.tv_nsec == 0;
}

static bool
timerfd_ctx_is_interval_timer(TimerFDCtx const *timerfd)
{
	return /**/
	    timerfd->current_itimerspec.it_interval.tv_sec != 0 ||
	    timerfd->current_itimerspec.it_interval.tv_nsec != 0;
}

static void
timerfd_ctx_disarm(TimerFDCtx *timerfd)
{
	timerfd->current_itimerspec.it_value.tv_sec = 0;
	timerfd->current_itimerspec.it_value.tv_nsec = 0;
}

static void
timerfd_ctx_update_to_current_time(TimerFDCtx *timerfd,
    struct timespec const *current_time)
{
	if (timerfd_ctx_is_disarmed(timerfd)) {
		return;
	}

	while (timespeccmp(/**/
	    &timerfd->current_itimerspec.it_value, current_time, <=)) {
		++timerfd->nr_expirations;

		if (!timerfd_ctx_is_interval_timer(timerfd) ||
		    timespecadd_safe(/**/
			&timerfd->current_itimerspec.it_value,
			&timerfd->current_itimerspec.it_interval,
			&timerfd->current_itimerspec.it_value) != 0) {
			timerfd_ctx_disarm(timerfd);
			break;
		}
	}
}

static errno_t
timerfd_ctx_register_event(TimerFDCtx *timerfd, struct timespec const *new,
    struct timespec const *current_time)
{
	struct kevent kev[1];
	struct timespec micros_ts;

	assert(new->tv_sec != 0 || new->tv_nsec != 0);

	if (timespecsub_safe(new, current_time, &micros_ts) != 0 ||
	    micros_ts.tv_sec < 0) {
		micros_ts.tv_sec = 0;
		micros_ts.tv_nsec = 0;
	}

	int64_t micros;

	if (__builtin_mul_overflow(micros_ts.tv_sec, 1000000, &micros) ||
	    __builtin_add_overflow(micros, micros_ts.tv_nsec / 1000,
		&micros)) {
		return EINVAL;
	}

	if ((micros_ts.tv_nsec % 1000) &&
	    __builtin_add_overflow(micros, 1, &micros)) {
		return EINVAL;
	}

	EV_SET(&kev[0], 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS,
	    micros, 0);

	if (kevent(timerfd->kq, kev, nitems(kev), /**/
		NULL, 0, NULL) < 0) {
		return errno;
	}

	return 0;
}

errno_t
timerfd_ctx_init(TimerFDCtx *timerfd, int kq, int clockid)
{
	errno_t ec;

	assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);

	*timerfd = (TimerFDCtx){.kq = kq, .clockid = clockid};

	if ((ec = pthread_mutex_init(&timerfd->mutex, NULL)) != 0) {
		return ec;
	}

	return 0;
}

errno_t
timerfd_ctx_terminate(TimerFDCtx *timerfd)
{
	return pthread_mutex_destroy(&timerfd->mutex);
}

static errno_t
timerfd_ctx_settime_impl(TimerFDCtx *timerfd, int flags,
    struct itimerspec const *new, struct itimerspec *old)
{
	errno_t ec;

	if (!itimerspec_is_valid(new)) {
		return EINVAL;
	}

	assert((flags & ~(TIMER_ABSTIME)) == 0);

	struct timespec current_time;
	if (clock_gettime(timerfd->clockid, &current_time) < 0) {
		return errno;
	}

	if (old) {
		// TODO(jan): Refactor this out into timerfd_gettime.
		timerfd_ctx_update_to_current_time(timerfd, &current_time);
		*old = timerfd->current_itimerspec;
		if (!timerfd_ctx_is_disarmed(timerfd)) {
			assert(timespeccmp(&old->it_value, &current_time, >));
			timespecsub(&old->it_value, &current_time,
			    &old->it_value);
		}
	}

	if (new->it_value.tv_sec == 0 && new->it_value.tv_nsec == 0) {
		struct kevent kev[1];
		EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
		(void)kevent(timerfd->kq, kev, nitems(kev), NULL, 0, NULL);

		timerfd_ctx_disarm(timerfd);
		goto success;
	}

	struct itimerspec new_absolute;
	if (flags & TIMER_ABSTIME) {
		new_absolute = *new;
	} else {
		new_absolute = (struct itimerspec){
		    .it_interval = new->it_interval,
		    .it_value = current_time,
		};

		if ((ec = timespecadd_safe(&new_absolute.it_value,
			 &new->it_value, &new_absolute.it_value)) != 0) {
			return ec;
		}
	}

	if ((ec = timerfd_ctx_register_event(timerfd, &new_absolute.it_value,
		 &current_time)) != 0) {
		return ec;
	}

	timerfd->current_itimerspec = new_absolute;

success:
	timerfd->nr_expirations = 0;
	return 0;
}

errno_t
timerfd_ctx_settime(TimerFDCtx *timerfd, int flags,
    struct itimerspec const *new, struct itimerspec *old)
{
	errno_t ec;

	(void)pthread_mutex_lock(&timerfd->mutex);
	ec = timerfd_ctx_settime_impl(timerfd, flags, new, old);
	(void)pthread_mutex_unlock(&timerfd->mutex);

	return ec;
}

static errno_t
timerfd_ctx_read_impl(TimerFDCtx *timerfd, uint64_t *value)
{
	for (;;) {
		struct kevent kev;

		int n = kevent(timerfd->kq, NULL, 0, &kev, 1,
		    &(struct timespec){0, 0});
		if (n < 0) {
			return errno;
		}

		if (n == 0) {
			return EAGAIN;
		}

		assert(kev.filter == EVFILT_TIMER);

		struct timespec current_time;
		if (clock_gettime(timerfd->clockid, &current_time) < 0) {
			return errno;
		}

		timerfd_ctx_update_to_current_time(timerfd, &current_time);

		uint64_t nr_expirations = timerfd->nr_expirations;
		timerfd->nr_expirations = 0;

		if (!timerfd_ctx_is_disarmed(timerfd)) {
			if (timerfd_ctx_register_event(timerfd,
				&timerfd->current_itimerspec.it_value,
				&current_time) != 0) {
				timerfd_ctx_disarm(timerfd);
			}
		}

		if (nr_expirations == 0) {
			return EAGAIN;
		}

		*value = nr_expirations;
		return 0;
	}
}

errno_t
timerfd_ctx_read(TimerFDCtx *timerfd, uint64_t *value)
{
	errno_t ec;

	(void)pthread_mutex_lock(&timerfd->mutex);
	ec = timerfd_ctx_read_impl(timerfd, value);
	(void)pthread_mutex_unlock(&timerfd->mutex);

	return ec;
}
