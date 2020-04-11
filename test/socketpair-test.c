#define _GNU_SOURCE

#include <atf-c.h>

#include <sys/types.h>

#if defined(__FreeBSD__)
#include <sys/capsicum.h>
#endif
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#ifndef __linux__
#include <sys/event.h>
#include <sys/param.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include "atf-c-leakcheck.h"

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

ATF_TC_WITHOUT_HEAD(socketpair__simple_socketpair);
ATF_TC_BODY_FD_LEAKCHECK(socketpair__simple_socketpair, tc)
{
	int p[2] = {-1, -1};
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, p) == 0);

	{
		struct pollfd pfd = {.fd = p[0], .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
		ATF_REQUIRE(pfd.revents == 0);

		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLIN};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
		ATF_REQUIRE(close(ep) == 0);
	}

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
		ATF_REQUIRE(pfd.revents == POLLOUT);

		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLOUT};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLOUT);
		ATF_REQUIRE(close(ep) == 0);
	}

	ATF_REQUIRE(close(p[0]) == 0);
	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, socketpair__simple_socketpair);

	return atf_no_error();
}
