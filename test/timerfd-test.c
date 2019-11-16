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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <err.h>
#include <poll.h>
#include <unistd.h>

#include <sys/timerfd.h>

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

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

#ifndef __linux__
static bool
is_fast_timer(int fd)
{
	struct kevent kev[1];
	EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);

	bool is_fast = kevent(fd, kev, nitems(kev), NULL, 0, NULL) == 0;
	close(fd);
	return (is_fast);
}
#endif

ATF_TC_WITHOUT_HEAD(timerfd__many_timers);
ATF_TC_BODY(timerfd__many_timers, tc)
{
	int timer_fds[1024];
	int i;

	for (i = 0; i < (int)nitems(timer_fds); ++i) {
		ATF_REQUIRE(
		    (timer_fds[i] = timerfd_create(CLOCK_MONOTONIC, /**/
			 TFD_CLOEXEC | TFD_NONBLOCK)) >= 0);
	}
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_timer);
ATF_TC_BODY(timerfd__simple_timer, tc)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, /**/
	    TFD_CLOEXEC | TFD_NONBLOCK);

	ATF_REQUIRE(timerfd >= 0);

	struct itimerspec time = {
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 100000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);

#ifndef __linux__
	ATF_REQUIRE(is_fast_timer(timerfd));
#endif
}

ATF_TC_WITHOUT_HEAD(timerfd__simple_periodic_timer);
ATF_TC_BODY(timerfd__simple_periodic_timer, tc)
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

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);

	poll(&pfd, 1, -1);
	uint64_t timeouts;
	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 1);

	usleep(230000);

	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 2);

#ifndef __linux__
	ATF_REQUIRE(is_fast_timer(timerfd));
#endif
}

ATF_TC_WITHOUT_HEAD(timerfd__complex_periodic_timer);
ATF_TC_BODY(timerfd__complex_periodic_timer, tc)
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

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};

	struct timespec b, e;
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);
	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 100000000 &&
	    e.tv_nsec < 150000000);

	poll(&pfd, 1, -1);
	uint64_t timeouts;
	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 1);

	usleep(230000);

	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 2);

#ifndef __linux__
	ATF_REQUIRE(!is_fast_timer(timerfd));
#endif
}

ATF_TC_WITHOUT_HEAD(timerfd__reset_periodic_timer);
ATF_TC_BODY(timerfd__reset_periodic_timer, tc)
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

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

	time = (struct itimerspec){
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 50000000,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 50000000,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

	uint64_t timeouts;
	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 1);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);
	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 150000000 &&
	    e.tv_nsec < 200000000);

#ifndef __linux__
	ATF_REQUIRE(is_fast_timer(timerfd));
#endif
}

ATF_TC_WITHOUT_HEAD(timerfd__reenable_periodic_timer);
ATF_TC_BODY(timerfd__reenable_periodic_timer, tc)
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

	struct pollfd pfd = {.fd = timerfd, .events = POLLIN};
	ATF_REQUIRE(poll(&pfd, 1, -1) == 1);

	uint64_t timeouts;
	ATF_REQUIRE(read(timerfd, &timeouts, sizeof(timeouts)) ==
	    (ssize_t)sizeof(timeouts));
	ATF_REQUIRE(timeouts == 1);

	time = (struct itimerspec){
	    .it_value.tv_sec = 0,
	    .it_value.tv_nsec = 0,
	    .it_interval.tv_sec = 0,
	    .it_interval.tv_nsec = 0,
	};

	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);
	ATF_REQUIRE(poll(&pfd, 1, 200) == 0);

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &e) == 0);
	timespecsub(&e, &b, &e);

	ATF_REQUIRE(e.tv_sec == 0 && e.tv_nsec >= 300000000 &&
	    e.tv_nsec < 350000000);

	time = (struct itimerspec){
	    .it_value.tv_sec = 1,
	    .it_value.tv_nsec = 0,
	    .it_interval.tv_sec = 1,
	    .it_interval.tv_nsec = 0,
	};
	ATF_REQUIRE(timerfd_settime(timerfd, 0, &time, NULL) == 0);

#ifndef __linux__
	ATF_REQUIRE(is_fast_timer(timerfd));
#endif
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, timerfd__many_timers);
	ATF_TP_ADD_TC(tp, timerfd__simple_timer);
	ATF_TP_ADD_TC(tp, timerfd__simple_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__complex_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__reset_periodic_timer);
	ATF_TP_ADD_TC(tp, timerfd__reenable_periodic_timer);

	return atf_no_error();
}
