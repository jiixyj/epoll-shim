#include <atf-c.h>

#include <sys/epoll.h>

#include <sys/event.h>

#include <errno.h>
#include <stdlib.h>

#include <dirent.h>

static int
lowest_fd(void)
{
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);
	close(kq);
	return kq;
}

ATF_TC_WITHOUT_HEAD(atf__environment);
ATF_TC_BODY(atf__environment, tc)
{
	DIR *cwd = opendir(".");

	ATF_REQUIRE(cwd != NULL);

	system("pwd");

	int number_ents = 0;
	struct dirent *de;
	while ((de = readdir(cwd)) != NULL) {
		ATF_REQUIRE(strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0);
		++number_ents;
	}

	ATF_REQUIRE(number_ents == 2);

	ATF_REQUIRE(getenv("LANG") == NULL);
	ATF_REQUIRE(getenv("LC_ALL") == NULL);
	ATF_REQUIRE(getenv("LC_COLLATE") == NULL);
	ATF_REQUIRE(getenv("LC_CTYPE") == NULL);
	ATF_REQUIRE(getenv("LC_MESSAGES") == NULL);
	ATF_REQUIRE(getenv("LC_MONETARY") == NULL);
	ATF_REQUIRE(getenv("LC_NUMERIC") == NULL);
	ATF_REQUIRE(getenv("LC_TIME") == NULL);
	ATF_REQUIRE(strcmp(getenv("HOME"), getenv("TMPDIR")) == 0);
	ATF_REQUIRE(strcmp(getenv("TZ"), "UTC") == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__simple);
ATF_TC_BODY(epoll__simple, tc)
{
	int lfd = lowest_fd();
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE(close(fd) >= 0);

	ATF_REQUIRE_EQ(lfd, lowest_fd());
}

ATF_TC_WITHOUT_HEAD(epoll__invalid_op);
ATF_TC_BODY(epoll__invalid_op, tc)
{
	int lfd = lowest_fd();
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(fd, 3, 5, NULL) < 0);
	ATF_REQUIRE(close(fd) >= 0);

	ATF_REQUIRE_EQ(lfd, lowest_fd());
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, atf__environment);
	ATF_TP_ADD_TC(tp, epoll__simple);
	ATF_TP_ADD_TC(tp, epoll__invalid_op);

	return atf_no_error();
}
