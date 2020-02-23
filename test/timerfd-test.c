#define _GNU_SOURCE

#include <atf-c.h>

#include <sys/types.h>

#ifndef __linux__
#include <sys/event.h>
#include <sys/timespec.h>
#endif

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
#include <unistd.h>

#include <sys/timerfd.h>

#include "atf-c-leakcheck.h"

// TODO(jan): Remove this once the definition is exposed in <sys/time.h> in
// all supported FreeBSD versions.
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

#ifndef timespecadd
#define	timespecadd(tsp, usp, vsp)					\
	do {								\
		(vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;		\
		(vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;	\
		if ((vsp)->tv_nsec >= 1000000000L) {			\
			(vsp)->tv_sec++;				\
			(vsp)->tv_nsec -= 1000000000L;			\
		}							\
	} while (0)
#endif

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

ATF_TC_WITHOUT_HEAD(timerfd__many_timers);
ATF_TC_BODY(timerfd__many_timers, tc)
{
	int timer_fds[512];
	int i;

	for (i = 0; i < (int)nitems(timer_fds); ++i) {
		timer_fds[i] = timerfd_create(CLOCK_MONOTONIC, /**/
		    TFD_CLOEXEC | TFD_NONBLOCK);
		ATF_REQUIRE(timer_fds[i] >= 0);
	}
}

static uint64_t
wait_for_timerfd(int timerfd)
{
	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};

	for (;;) {
		ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

		uint64_t timeouts;
		ssize_t r = read(timerfd, &timeouts, sizeof(timeouts));
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			continue;
		}

		ATF_REQUIRE(r == (ssize_t)sizeof(timeouts));
		ATF_REQUIRE(timeouts > 0);
		return timeouts;
	}
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__simple_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	(void)wait_for_timerfd(timerfd);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_periodic_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__simple_periodic_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	uint64_t timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);
	ATF_REQUIRE(timeouts == 1);

	usleep(230000);

	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 2);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__complex_periodic_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__complex_periodic_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000001,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	uint64_t timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);

	ATF_REQUIRE(timeouts == 1);

	usleep(230000);

	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 2);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__reset_periodic_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__reset_periodic_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);

	(void)wait_for_timerfd(timerfd);

	time = (struct itimerspec){
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 50000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	uint64_t timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(timeouts == 1);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 150000000 &&
	    e.tv_nsec < 240000000);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__reenable_periodic_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__reenable_periodic_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);

	uint64_t timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(timeouts == 1);

	time = (struct itimerspec){
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 0,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 0,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
	ATF_REQUIRE(poll(&pfd, 1, 250) == 0);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 300000000 &&
	    e.tv_nsec < 400000000);

	time = (struct itimerspec){
	    .it_value.tv_sec = 1,
	    .it_value.tv_nsec = 0,
	    .it_interval.tv_sec = 1,
	    .it_interval.tv_nsec = 0,
	};
	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	ATF_REQUIRE(close(timerfd) == 0);
}

/*
 * Adapted from sghctoma's example here:
 * https://github.com/jiixyj/epoll-shim/issues/2
 *
 * The SIGUSR1 signal should not kill the process.
 */
ATF_TC_WITHOUT_HEAD(timerfd__expire_five);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__expire_five, tc)
{
	int fd;
	struct itimerspec value;
	uint64_t total_exp = 0;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	ATF_REQUIRE(fd >= 0);

	value.it_value.tv_sec = 3;
	value.it_value.tv_nsec = 0;
	value.it_interval.tv_sec = 1;
	value.it_interval.tv_nsec = 0;

	ATF_REQUIRE(timerfd_settime(fd, 0, &value, NULL) == 0);

	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	kill(getpid(), SIGUSR1);

	for (;;) {
		uint64_t exp = wait_for_timerfd(fd);

		printf("timer expired %u times\n", (unsigned)exp);

		total_exp += exp;
		if (total_exp >= 5) {
			break;
		}
	}

	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_gettime);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__simple_gettime, tc)
{
	struct itimerspec curr_value;

	int fd = timerfd_create(CLOCK_MONOTONIC, 0);
	ATF_REQUIRE(fd >= 0);

	ATF_REQUIRE(timerfd_gettime(fd, &curr_value) == 0);

	ATF_REQUIRE(curr_value.it_value.tv_sec == 0);
	ATF_REQUIRE(curr_value.it_value.tv_nsec == 0);
	ATF_REQUIRE(curr_value.it_interval.tv_sec == 0);
	ATF_REQUIRE(curr_value.it_interval.tv_nsec == 0);

	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_blocking_periodic_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__simple_blocking_periodic_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);

	uint64_t timeouts = 0;
	int num_loop_iterations = 0;

	while (timeouts < 3) {
		uint64_t timeouts_local;
		ATF_REQUIRE(
		    read(timerfd, &timeouts_local, sizeof(timeouts_local)) ==
		    (ssize_t)sizeof(timeouts_local));
		ATF_REQUIRE(timeouts_local > 0);

		++num_loop_iterations;
		timeouts += timeouts_local;
	}

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 300000000 &&
	    e.tv_nsec < 350000000);

	ATF_REQUIRE(num_loop_iterations <= 3);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__argument_checks);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__argument_checks, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE_ERRNO(EFAULT, timerfd_settime(timerfd, 0, NULL, NULL) < 0);
	ATF_REQUIRE_ERRNO(EFAULT, timerfd_settime(-2, 0, NULL, NULL) < 0);
	ATF_REQUIRE_ERRNO(EBADF, timerfd_settime(-2, 0, &time, NULL) < 0);
	ATF_REQUIRE_ERRNO(EFAULT, timerfd_settime(-2, 42, NULL, NULL) < 0);
	ATF_REQUIRE_ERRNO(EINVAL, timerfd_settime(-2, 42, &time, NULL) < 0);
	ATF_REQUIRE_ERRNO(EINVAL,
	    timerfd_settime(timerfd, 42, &time, NULL) < 0);

	ATF_REQUIRE_ERRNO(EINVAL,
	    timerfd_create(CLOCK_MONOTONIC | 42, TFD_CLOEXEC));
	ATF_REQUIRE_ERRNO(EINVAL,
	    timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | 42));

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__upgrade_simple_to_complex);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__upgrade_simple_to_complex, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);

	(void)wait_for_timerfd(timerfd);

	time = (struct itimerspec){
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 50000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 70000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	uint64_t timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(timeouts == 1);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 150000000 &&
	    e.tv_nsec < 200000000);

	timeouts = wait_for_timerfd(timerfd);
	ATF_REQUIRE(timeouts == 1);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 220000000 &&
	    e.tv_nsec < 270000000);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC_WITHOUT_HEAD(timerfd__absolute_timer);
ATF_TC_BODY_FD_LEAKCHECK(timerfd__absolute_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);

	struct itimerspec time = {
	    .it_value = b,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 0,
	};

	struct timespec ts_600ms = {
	    .tv_sec = 0,
	    .tv_nsec = 600000000,
	};

	timespecadd(&time.it_value, &ts_600ms, &time.it_value);

	ATF_REQUIRE(timerfd_settime(timerfd, /**/
			TFD_TIMER_ABSTIME, &time, NULL) == 0);

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

	// Don't read(2) here!

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 &&
	    /* Don't check for this because of spurious wakeups. */
	    /* e.tv_nsec >= 600000000 && */
	    e.tv_nsec < 650000000);

	struct itimerspec zeroed_its = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 0,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 0,
	};
	ATF_REQUIRE(timerfd_settime(timerfd, 0, &zeroed_its, NULL) == 0);

	uint64_t timeouts;
	ATF_REQUIRE_ERRNO(EAGAIN,
	    read(timerfd, &timeouts, sizeof(timeouts)) < 0);

	ATF_REQUIRE(poll(&pfd, 1, 0) == 0);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TC(timerfd__periodic_timer_performance);
ATF_TC_HEAD(timerfd__periodic_timer_performance, tc)
{
	atf_tc_set_md_var(tc, "timeout", "1");
}
ATF_TC_BODY_FD_LEAKCHECK(timerfd__periodic_timer_performance, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 1,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 1,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	usleep(400000);

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

	uint64_t timeouts;
	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts >= 400000000);

	ATF_REQUIRE(close(timerfd) == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, timerfd__many_timers);
	ATF_TP_ADD_TC(tp, timerfd__simple_timer);
	ATF_TP_ADD_TC(tp, timerfd__simple_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__complex_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__reset_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__reenable_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__expire_five);
	ATF_TP_ADD_TC(tp, timerfd__simple_gettime);
	ATF_TP_ADD_TC(tp, timerfd__simple_blocking_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__argument_checks);
	ATF_TP_ADD_TC(tp, timerfd__upgrade_simple_to_complex);
	ATF_TP_ADD_TC(tp, timerfd__absolute_timer);
	ATF_TP_ADD_TC(tp, timerfd__periodic_timer_performance);

	return atf_no_error();
}
