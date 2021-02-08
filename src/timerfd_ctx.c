#include "timerfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>

#include "timespec_util.h"

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

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

static errno_t
ts_to_nanos(struct timespec const *ts, int64_t *ts_nanos_out)
{
	int64_t ts_nanos;

	if (__builtin_mul_overflow(ts->tv_sec, 1000000000, &ts_nanos) ||
	    __builtin_add_overflow(ts_nanos, ts->tv_nsec, &ts_nanos)) {
		return EOVERFLOW;
	}

	*ts_nanos_out = ts_nanos;
	return 0;
}

static struct timespec
nanos_to_ts(int64_t ts_nanos)
{
	return (struct timespec){
	    .tv_sec = ts_nanos / 1000000000,
	    .tv_nsec = ts_nanos % 1000000000,
	};
}

static void
timerfd_ctx_update_to_current_time(TimerFDCtx *timerfd,
    struct timespec const *current_time)
{
	if (timerfd_ctx_is_disarmed(timerfd)) {
		return;
	}

	if (timerfd_ctx_is_interval_timer(timerfd)) {
		struct timespec diff_time;

		if (!timespecsub_safe(current_time,
			&timerfd->current_itimerspec.it_value, &diff_time)) {
			goto disarm;
		}

		if (diff_time.tv_sec >= 0) {
			int64_t diff_nanos;
			if (ts_to_nanos(&diff_time, &diff_nanos)) {
				goto disarm;
			}

			int64_t interval_nanos;
			if (ts_to_nanos(
				&timerfd->current_itimerspec.it_interval,
				&interval_nanos)) {
				goto disarm;
			}

			int64_t expirations = diff_nanos / interval_nanos;
			if (expirations == INT64_MAX) {
				goto disarm;
			}
			++expirations;

			int64_t nanos_to_add;
			if (__builtin_mul_overflow(expirations, interval_nanos,
				&nanos_to_add)) {
				goto disarm;
			}

			struct timespec next_ts = nanos_to_ts(nanos_to_add);
			if (!timespecadd_safe(&next_ts,
				&timerfd->current_itimerspec.it_value,
				&next_ts)) {
				goto disarm;
			}

			assert(expirations >= 0);

			timerfd->nr_expirations += (uint64_t)expirations;
			timerfd->current_itimerspec.it_value = next_ts;
		}
	} else {
		if (timespeccmp(current_time,
			&timerfd->current_itimerspec.it_value, >=)) {
			++timerfd->nr_expirations;
			goto disarm;
		}
	}

	assert(timespeccmp(current_time, /**/
	    &timerfd->current_itimerspec.it_value, <));

	return;

disarm:
	timerfd_ctx_disarm(timerfd);
}

#if defined(__NetBSD__)

/* On NetBSD, EVFILT_TIMER sometimes returns early. */
#define QUIRKY_EVFILT_TIMER

static bool
round_up_millis(int64_t millis, int64_t *result)
{

	long ticks = CLK_TCK;
	if (ticks <= 0) {
		return false;
	}
	uint32_t ms_per_tick = (uint32_t)(1000 / ticks + !!(1000 % ticks));

	uint32_t ms = (uint32_t)millis;

	/* We need to round up ms to a multiple of the ms per tick. */
	uint32_t fixed_ms = ms / ms_per_tick * ms_per_tick;
	if (fixed_ms < ms) {
		fixed_ms += ms_per_tick;
	}

	/* Then add one tick so that we never sleep shorter than requested. */
	fixed_ms += ms_per_tick;

	/* Add one second for large timeout values (see mstohz in NetBSD for
	 * the reason). */
	if (fixed_ms >= 0x20000 && (fixed_ms % 1000) != 0) {
		fixed_ms += 1000;
	}

	*result = fixed_ms;
	return true;
}
#endif

static errno_t
timerfd_ctx_register_event(TimerFDCtx *timerfd, struct timespec const *new,
    struct timespec const *current_time)
{
	struct kevent kev[1];
	struct timespec diff_time;

	assert(new->tv_sec != 0 || new->tv_nsec != 0);

	if (!timespecsub_safe(new, current_time, &diff_time) ||
	    diff_time.tv_sec < 0) {
		diff_time.tv_sec = 0;
		diff_time.tv_nsec = 0;
	}

	/* Let's hope nobody needs timeouts larger than 10 years. */
	if (diff_time.tv_sec >= 315360000) {
		return 0;
	}

	/* There are EVFILT_TIMER implementations that return EINVAL on a
	 * timeout value of 0, for example NetBSD. */
	bool handle_einval_on_zero_value = false;

#ifdef NOTE_USECONDS
	int64_t micros = (int64_t)diff_time.tv_sec * 1000000 +
	    diff_time.tv_nsec / 1000;

	if ((diff_time.tv_nsec % 1000) != 0) {
		++micros;
	}

	/* If there is NOTE_USECONDS support we assume timeout values of 0 are
	 * valid. For FreeBSD this is the case. */

	/* The data field is only 32 bit wide on FreeBSD 11 i386. If this
	 * would overflow, try again with milliseconds. */
	if (__builtin_add_overflow(micros, 0, &kev[0].data)) {
		goto try_with_millis;
	}
	EV_SET(&kev[0], 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT, /**/
	    NOTE_USECONDS, micros, 0);

	goto set_timer;

try_with_millis:;
#endif

#ifdef QUIRKY_EVFILT_TIMER
	/* Let's hope 49 days are enough. */
	if (diff_time.tv_sec >= 4233600) {
		return 0;
	}
#endif

	int64_t millis = (int64_t)diff_time.tv_sec * 1000 +
	    diff_time.tv_nsec / 1000000;

	if ((diff_time.tv_nsec % 1000000) != 0) {
		++millis;
	}

#ifdef QUIRKY_EVFILT_TIMER
	if (millis != 0 && !round_up_millis(millis, &millis)) {
		return 0;
	}
#endif

	if (millis == 0) {
		handle_einval_on_zero_value = true;
	}

	if (__builtin_add_overflow(millis, 0, &kev[0].data)) {
		return 0;
	}
	EV_SET(&kev[0], 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT, /**/
	    0, millis, 0);

set_timer:;

#if !defined(__FreeBSD__)
	{
		/*
		 * On some BSD's, EVFILT_TIMER ignores timer resets using
		 * EV_ADD, so we have to do it manually.
		 */

		struct kevent kev_delete;
		EV_SET(&kev_delete, 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);

		int oe = errno;
		(void)kevent(timerfd->kq, &kev_delete, 1, NULL, 0, NULL);
		errno = oe;
	}
#endif

reset_timer:;

	int oe = errno;
	if (kevent(timerfd->kq, kev, nitems(kev), /**/
		NULL, 0, NULL) < 0) {
		if (handle_einval_on_zero_value && errno == EINVAL) {
			kev[0].data = 1;
			handle_einval_on_zero_value = false;
			errno = oe;
			goto reset_timer;
		}

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

static void
timerfd_ctx_gettime_impl(TimerFDCtx *timerfd, struct itimerspec *cur,
    struct timespec const *current_time)
{
	timerfd_ctx_update_to_current_time(timerfd, current_time);
	*cur = timerfd->current_itimerspec;
	if (!timerfd_ctx_is_disarmed(timerfd)) {
		assert(timespeccmp(current_time, &cur->it_value, <));
		timespecsub(&cur->it_value, current_time, &cur->it_value);
	}
}

static errno_t
timerfd_ctx_settime_impl(TimerFDCtx *timerfd, bool is_abstime,
    struct itimerspec const *new, struct itimerspec *old)
{
	errno_t ec;

	if (!itimerspec_is_valid(new)) {
		return EINVAL;
	}

	struct timespec current_time;
	if (clock_gettime(timerfd->clockid, &current_time) < 0) {
		return errno;
	}

	if (old) {
		timerfd_ctx_gettime_impl(timerfd, old, &current_time);
	}

	if (new->it_value.tv_sec == 0 && new->it_value.tv_nsec == 0) {
		struct kevent kev[1];

		EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
		(void)kevent(timerfd->kq, kev, nitems(kev), NULL, 0, NULL);

		timerfd_ctx_disarm(timerfd);
		goto success;
	}

	struct itimerspec new_absolute;
	if (is_abstime) {
		new_absolute = *new;
	} else {
		new_absolute = (struct itimerspec){
		    .it_interval = new->it_interval,
		    .it_value = current_time,
		};

		if (!timespecadd_safe(&new_absolute.it_value, &new->it_value,
			&new_absolute.it_value)) {
			ec = EINVAL;
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
timerfd_ctx_settime(TimerFDCtx *timerfd, bool is_abstime,
    struct itimerspec const *new, struct itimerspec *old)
{
	errno_t ec;

	(void)pthread_mutex_lock(&timerfd->mutex);
	ec = timerfd_ctx_settime_impl(timerfd, is_abstime, new, old);
	(void)pthread_mutex_unlock(&timerfd->mutex);

	return ec;
}

errno_t
timerfd_ctx_gettime(TimerFDCtx *timerfd, struct itimerspec *cur)
{
	struct timespec current_time;
	if (clock_gettime(timerfd->clockid, &current_time) < 0) {
		return errno;
	}

	(void)pthread_mutex_lock(&timerfd->mutex);
	timerfd_ctx_gettime_impl(timerfd, cur, &current_time);
	(void)pthread_mutex_unlock(&timerfd->mutex);

	return 0;
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
