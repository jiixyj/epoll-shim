#include <atf-c.h>

#include <sys/epoll.h>

#include <sys/event.h>

#include <errno.h>

static int
lowest_fd(void)
{
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);
	close(kq);
	return kq;
}

ATF_TC_WITHOUT_HEAD(epoll__simple);
ATF_TC_BODY(epoll__simple, tc)
{
	int lfd = lowest_fd();
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE(close(fd) >= 0);

	ATF_REQUIRE(lfd == lowest_fd());
}

ATF_TC_WITHOUT_HEAD(epoll__invalid_op);
ATF_TC_BODY(epoll__invalid_op, tc)
{
	int lfd = lowest_fd();
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(fd, 3, 5, NULL));
	ATF_REQUIRE(close(fd) >= 0);

	ATF_REQUIRE(lfd == lowest_fd());
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, epoll__simple);
	ATF_TP_ADD_TC(tp, epoll__invalid_op);

	return atf_no_error();
}
