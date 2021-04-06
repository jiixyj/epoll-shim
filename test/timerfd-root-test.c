#define _GNU_SOURCE

#include <atf-c.h>

#include <sys/types.h>

#include <sys/param.h>
#include <sys/select.h>
#include <sys/time.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <err.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <sys/timerfd.h>

#include "atf-c-leakcheck.h"

static struct timespec current_time;
static void
reset_time(void)
{
	(void)clock_settime(CLOCK_REALTIME, &current_time);
}

ATF_TC_WITHOUT_HEAD(timerfd_root__zero_read_on_abs_realtime);
ATF_TC_BODY_FD_LEAKCHECK(timerfd_root__zero_read_on_abs_realtime, tc)
{
	int tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	ATF_REQUIRE(tfd >= 0);

	ATF_REQUIRE(clock_gettime(CLOCK_REALTIME, &current_time) == 0);
	ATF_REQUIRE(atexit(reset_time) == 0);

	ATF_REQUIRE(timerfd_settime(tfd, TFD_TIMER_ABSTIME,
			&(struct itimerspec) {
			    .it_value = current_time,
			    .it_interval.tv_sec = 1,
			    .it_interval.tv_nsec = 0,
			},
			NULL) == 0);

	ATF_REQUIRE(
	    poll(&(struct pollfd) { .fd = tfd, .events = POLLIN }, 1, -1) == 1);

	{
		int r = clock_settime(CLOCK_REALTIME,
		    &(struct timespec) {
			.tv_sec = current_time.tv_sec - 1,
			.tv_nsec = current_time.tv_nsec,
		    });
		if (r < 0 && errno == EPERM) {
			atf_tc_skip("root required");
		}
		ATF_REQUIRE(r == 0);
	}

	uint64_t exp;
	ssize_t r = read(tfd, &exp, sizeof(exp));
	ATF_REQUIRE(r == 0);

	{
		int r = fcntl(tfd, F_GETFL);
		ATF_REQUIRE(r >= 0);
		r = fcntl(tfd, F_SETFL, r | O_NONBLOCK);
		ATF_REQUIRE(r >= 0);
	}

	r = read(tfd, &exp, sizeof(exp));
	ATF_REQUIRE_ERRNO(EAGAIN, r < 0);

	current_time.tv_sec += 1;
	ATF_REQUIRE(poll(&(struct pollfd) { .fd = tfd, .events = POLLIN }, 1,
			1800) == 1);
	r = read(tfd, &exp, sizeof(exp));
	ATF_REQUIRE(r == (ssize_t)sizeof(exp));
	ATF_REQUIRE(exp == 1);

	ATF_REQUIRE(close(tfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd_root__read_on_abs_realtime_no_interval);
ATF_TC_BODY_FD_LEAKCHECK(timerfd_root__read_on_abs_realtime_no_interval, tc)
{
	int tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	ATF_REQUIRE(tfd >= 0);

	ATF_REQUIRE(clock_gettime(CLOCK_REALTIME, &current_time) == 0);
	ATF_REQUIRE(atexit(reset_time) == 0);

	ATF_REQUIRE(timerfd_settime(tfd, TFD_TIMER_ABSTIME,
			&(struct itimerspec) {
			    .it_value = current_time,
			    .it_interval.tv_sec = 0,
			    .it_interval.tv_nsec = 0,
			},
			NULL) == 0);

	ATF_REQUIRE(
	    poll(&(struct pollfd) { .fd = tfd, .events = POLLIN }, 1, -1) == 1);

	{
		int r = clock_settime(CLOCK_REALTIME,
		    &(struct timespec) {
			.tv_sec = current_time.tv_sec - 1,
			.tv_nsec = current_time.tv_nsec,
		    });
		if (r < 0 && errno == EPERM) {
			atf_tc_skip("root required");
		}
		ATF_REQUIRE(r == 0);
	}

	uint64_t exp;
	ssize_t r = read(tfd, &exp, sizeof(exp));
	ATF_REQUIRE(r == (ssize_t)sizeof(exp));
	ATF_REQUIRE(exp == 1);

	ATF_REQUIRE(close(tfd) == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, timerfd_root__zero_read_on_abs_realtime);
	ATF_TP_ADD_TC(tp, timerfd_root__read_on_abs_realtime_no_interval);

	return atf_no_error();
}
