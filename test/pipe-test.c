#define _GNU_SOURCE

#include <atf-c.h>

#if defined(__FreeBSD__)
#include <sys/capsicum.h>
#endif
#include <sys/epoll.h>
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

/*
 * These tests show that on Linux, POLLOUT and POLLERR may happen at the same
 * time on a write end of a pipe after the read end was closed depending on the
 * contents of the pipe buffer.
 */

ATF_TC_WITHOUT_HEAD(pipe__simple_poll);
ATF_TC_BODY_FD_LEAKCHECK(pipe__simple_poll, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	{
		struct pollfd pfd = {.fd = p[0], .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
		ATF_REQUIRE(pfd.revents == 0);

		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLIN | EPOLLET};
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
		eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLOUT);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
		ATF_REQUIRE(close(ep) == 0);
	}

	ATF_REQUIRE(close(p[0]) == 0);
	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_write_end_after_read_end_close);
ATF_TC_BODY_FD_LEAKCHECK(pipe__poll_write_end_after_read_end_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
#ifdef __linux__
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLERR));
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLHUP));
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	if (eps[0].events == POLLERR) {
		atf_tc_skip(
		    "kqueue based emulation may return just POLLERR here");
	}
	ATF_REQUIRE(eps[0].events == (EPOLLOUT | POLLERR));
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_full_write_end_after_read_end_close);
ATF_TC_BODY_FD_LEAKCHECK(pipe__poll_full_write_end_after_read_end_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
#ifdef __linux__
		ATF_REQUIRE(pfd.revents == POLLERR);
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLHUP));
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	int ret = epoll_wait(ep, eps, 32, 0);
	if (ret == 0) {
		atf_tc_skip("NetBSD hangs here");
	}
	ATF_REQUIRE(ret == 1);
	ATF_REQUIRE(eps[0].events == POLLERR);
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_full_write_end_after_read_end_close_hup);
ATF_TC_BODY_FD_LEAKCHECK(pipe__poll_full_write_end_after_read_end_close_hup,
    tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

#if defined(__FreeBSD__)
	{
		cap_rights_t rights;
		ATF_REQUIRE(cap_rights_get(p[1], &rights) == 0);
		cap_rights_clear(&rights, CAP_READ);
		ATF_REQUIRE(cap_rights_limit(p[1], &rights) == 0);
	}
#endif

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1]};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
#ifdef __linux__
		ATF_REQUIRE(pfd.revents == POLLERR);
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == POLLHUP);
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	int ret = epoll_wait(ep, eps, 32, 0);
	if (ret == 0) {
		atf_tc_skip("NetBSD hangs here");
	}
	ATF_REQUIRE(ret == 1);
	ATF_REQUIRE(eps[0].events == POLLERR);
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_full_minus_1b_write_end_after_read_end_close);
ATF_TC_BODY_FD_LEAKCHECK(
    pipe__poll_full_minus_1b_write_end_after_read_end_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	ATF_REQUIRE(read(p[0], &c, 1) == 1);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
#ifdef __linux__
		ATF_REQUIRE(pfd.revents == POLLERR);
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLHUP));
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	int ret = epoll_wait(ep, eps, 32, 0);
	if (ret == 0) {
		atf_tc_skip("NetBSD hangs here");
	}
	ATF_REQUIRE(ret == 1);
	ATF_REQUIRE(eps[0].events == POLLERR);
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_full_minus_511b_write_end_after_read_end_close);
ATF_TC_BODY_FD_LEAKCHECK(
    pipe__poll_full_minus_511b_write_end_after_read_end_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	for (int i = 0; i < PIPE_BUF - 1; ++i) {
		ATF_REQUIRE(read(p[0], &c, 1) == 1);
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
#ifdef __linux__
		ATF_REQUIRE(pfd.revents == POLLERR);
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLHUP));
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	int ret = epoll_wait(ep, eps, 32, 0);
	if (ret == 0) {
		atf_tc_skip("NetBSD hangs here");
	}
	ATF_REQUIRE(ret == 1);
	ATF_REQUIRE(eps[0].events == POLLERR);
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__poll_full_minus_512b_write_end_after_read_end_close);
ATF_TC_BODY_FD_LEAKCHECK(
    pipe__poll_full_minus_512b_write_end_after_read_end_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	for (int i = 0; i < PIPE_BUF; ++i) {
		ATF_REQUIRE(read(p[0], &c, 1) == 1);
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

	{
		struct pollfd pfd = {.fd = p[1], .events = POLLOUT};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);

#ifdef __linux__
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLERR));
#elif defined(__NetBSD__)
		ATF_REQUIRE(pfd.revents == (POLLOUT | POLLHUP));
#else
		ATF_REQUIRE(pfd.revents == POLLHUP);
#endif
	}

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	if (eps[0].events == POLLERR) {
		atf_tc_skip(
		    "kqueue based emulation may return just POLLERR here");
	}
	ATF_REQUIRE(eps[0].events == (POLLOUT | POLLERR));
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

// #define FORCE_EPOLL

static void
print_statbuf(struct stat *sb)
{
	printf("st_dev: %lu\n", sb->st_dev);
	printf("st_ino: %lu\n", sb->st_ino);
	printf("st_nlink: %lu\n", sb->st_nlink);
	printf("st_mode: %ho\n", sb->st_mode);
	printf("st_uid: %u\n", sb->st_uid);
	printf("st_gid: %u\n", sb->st_gid);
	printf("st_rdev: %lu\n", sb->st_rdev);
	printf("st_size: %lu\n", sb->st_size);
	printf("st_blocks: %lu\n", sb->st_blocks);
	printf("st_blksize: %d\n", sb->st_blksize);
#if !defined(__linux__)
	printf("st_flags: %x\n", sb->st_flags);
	printf("st_gen: %lu\n", sb->st_gen);
#endif
}

static int const SPURIOUS_EV_ADD = 0
#ifdef __NetBSD__
    | EV_ADD
#endif
    ;

static int const SPURIOUS_EV_ONESHOT = 0
#ifdef __FreeBSD__
    | EV_ONESHOT
#endif
    ;

ATF_TC_WITHOUT_HEAD(pipe__pipe_event_poll);
ATF_TC_BODY_FD_LEAKCHECK(pipe__pipe_event_poll, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);
#if defined(__FreeBSD__)
	{
		cap_rights_t rights;
		ATF_REQUIRE(cap_rights_get(p[1], &rights) == 0);
		cap_rights_clear(&rights, CAP_READ);
		ATF_REQUIRE(cap_rights_limit(p[1], &rights) == 0);
	}
#endif

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[1], EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 1, NULL, 0, NULL) == 0);

	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);

	ATF_REQUIRE_MSG(kev[0].flags == (EV_CLEAR | SPURIOUS_EV_ADD), "%x",
	    kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 16384, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){.events = EPOLLIN | EPOLLOUT | EPOLLET};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	for (int i = 0; i < PIPE_BUF - 1; ++i) {
		ATF_REQUIRE(read(p[0], &c, 1) == 1);
	}

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(read(p[0], &c, 1) == 1);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE(kev[0].flags == (EV_CLEAR | SPURIOUS_EV_ADD));
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);

	ATF_REQUIRE(read(p[0], &c, 1) == 1);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE(kev[0].flags == (EV_CLEAR | SPURIOUS_EV_ADD));
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF + 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);

	/* kqueue based emulation will trigger another edge here. */
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE_MSG(kev[0].flags ==
		(EV_CLEAR | SPURIOUS_EV_ADD | EV_EOF | SPURIOUS_EV_ONESHOT),
	    "%04x", kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	if (kev[0].data == 0) {
		atf_tc_skip("kev.data == 0 is a valid value on EV_EOF");
	}
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF + 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
	ATF_REQUIRE(close(kq) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == (EPOLLOUT | EPOLLERR));
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__fifo_event_poll);
ATF_TC_BODY_FD_LEAKCHECK(pipe__fifo_event_poll, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(mkfifo("the_fifo", 0666) == 0);

	ATF_REQUIRE(
	    (p[0] = open("the_fifo", O_RDONLY | O_CLOEXEC | O_NONBLOCK)) >= 0);
	ATF_REQUIRE(
	    (p[1] = open("the_fifo", O_WRONLY | O_CLOEXEC | O_NONBLOCK)) >= 0);

	{
		int fl = fcntl(p[0], F_GETFL, 0);
		int rq = O_RDONLY;
		ATF_REQUIRE((fl & O_ACCMODE) == rq);
	}
	{
		int fl = fcntl(p[1], F_GETFL, 0);
		int rq = O_WRONLY;
		ATF_REQUIRE((fl & O_ACCMODE) == rq);
	}

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[1], EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, 0);
	EV_SET(&kev[1], p[1], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 2, NULL, 0, NULL) == 0);

	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);

	/*
	 * NetBSD still suffers from the bug fixed by
	 * https://github.com/freebsd/freebsd/commit/4681aa72a96557236594d33069de3b60506be886
	 */
	if (kev[0].flags & EV_EOF) {
		atf_tc_skip("EVFILT_WRITE broken on FIFOs");
	}

	ATF_REQUIRE_MSG(kev[0].flags == EV_CLEAR, "%x", kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 16384, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){
	    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
	};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	for (int i = 0; i < PIPE_BUF - 1; ++i) {
		ATF_REQUIRE(read(p[0], &c, 1) == 1);
	}

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(read(p[0], &c, 1) == 1);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE(kev[0].flags == EV_CLEAR);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);

	ATF_REQUIRE(read(p[0], &c, 1) == 1);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE(kev[0].flags == EV_CLEAR);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF + 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);

	/* kqueue based emulation will trigger another edge here. */
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev), NULL) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE_MSG(kev[0].flags == (EV_CLEAR | EV_EOF), "%04x",
	    kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF + 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == (EPOLLOUT | EPOLLERR));

	ATF_REQUIRE(
	    (p[0] = open("the_fifo", O_RDONLY | O_CLOEXEC | O_NONBLOCK)) >= 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev), NULL) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
	ATF_REQUIRE(kev[0].flags == EV_CLEAR);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == PIPE_BUF + 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLOUT);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(close(kq) == 0);
	kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	EV_SET(&kev[0], p[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 1, NULL, 0, NULL) == 0);

	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
	ATF_REQUIRE(kev[0].filter == EVFILT_READ);
	ATF_REQUIRE_MSG(kev[0].flags == EV_CLEAR, "%x", kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 65023, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(close(ep) == 0);
	ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	eps[0] = (struct epoll_event){
	    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
	};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLIN);

	while ((r = read(p[0], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(close(p[1]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
	ATF_REQUIRE(kev[0].filter == EVFILT_READ);
	ATF_REQUIRE(kev[0].flags == (EV_EOF | EV_CLEAR));
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 0, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLHUP);

	ATF_REQUIRE(read(p[0], &c, 1) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(
	    (p[1] = open("the_fifo", O_WRONLY | O_CLOEXEC | O_NONBLOCK)) >= 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

	ATF_REQUIRE(write(p[1], &c, 1) == 1);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
	ATF_REQUIRE(kev[0].filter == EVFILT_READ);
	ATF_REQUIRE(kev[0].flags == EV_CLEAR);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 1, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
	ATF_REQUIRE(close(kq) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == EPOLLIN);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[0]) == 0);
	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__closed_read_end);
ATF_TC_BODY_FD_LEAKCHECK(pipe__closed_read_end, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);
#if defined(__FreeBSD__)
	{
		cap_rights_t rights;
		ATF_REQUIRE(cap_rights_get(p[1], &rights) == 0);
		cap_rights_clear(&rights, CAP_READ);
		ATF_REQUIRE(cap_rights_limit(p[1], &rights) == 0);
	}
#endif

	ATF_REQUIRE(close(p[0]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[1], EVFILT_READ,  /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);
	EV_SET(&kev[1], p[1], EVFILT_WRITE, /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 2, kev, 2, NULL) == 2);
	ATF_REQUIRE((kev[0].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[0].data == 0);
	ATF_REQUIRE((kev[1].flags & EV_ERROR) != 0);
#ifdef __NetBSD__
	ATF_REQUIRE(kev[1].data == EBADF);
#else
	ATF_REQUIRE(kev[1].data == EPIPE);
#endif

	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
	ATF_REQUIRE(kev[0].filter == EVFILT_READ);
	ATF_REQUIRE_MSG(kev[0].flags ==
		(EV_EOF | EV_CLEAR | SPURIOUS_EV_ADD | EV_RECEIPT),
	    "%04x", kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	ATF_REQUIRE_MSG(kev[0].data == 0, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
	ATF_REQUIRE(close(kq) == 0);
#endif
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){
		    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
		};
		int ret = epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]);
#ifdef __NetBSD__
		if (ret < 0 && errno == EBADF) {
			atf_tc_skip("NetBSD does not support EVFILT_USER");
		}
#endif
		ATF_REQUIRE(ret == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == (EPOLLOUT | EPOLLERR));
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){
		    .events = EPOLLIN | EPOLLRDHUP | EPOLLET,
		};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLERR);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLET};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLERR);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = 0};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLERR);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLERR);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLERR);

		ATF_REQUIRE(close(ep) == 0);
	}

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__closed_read_end_register_before_close);
ATF_TC_BODY_FD_LEAKCHECK(pipe__closed_read_end_register_before_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

#if defined(__FreeBSD__)
	{
		int fl = fcntl(p[0], F_GETFL, 0);
		ATF_REQUIRE((fl & O_ACCMODE) == O_RDWR);
	}
	{
		int fl = fcntl(p[1], F_GETFL, 0);
		ATF_REQUIRE((fl & O_ACCMODE) == O_RDWR);
	}
	{
		cap_rights_t rights;
		ATF_REQUIRE(cap_rights_get(p[1], &rights) == 0);
		cap_rights_clear(&rights, CAP_READ);
		ATF_REQUIRE(cap_rights_limit(p[1], &rights) == 0);
	}
#else
	{
		int fl = fcntl(p[0], F_GETFL, 0);
		ATF_REQUIRE((fl & O_ACCMODE) == O_RDONLY);
	}
	{
		int fl = fcntl(p[1], F_GETFL, 0);
		ATF_REQUIRE((fl & O_ACCMODE) == O_WRONLY);
	}
#endif

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[1], EVFILT_READ,  /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);
	EV_SET(&kev[1], p[1], EVFILT_WRITE, /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 2, kev, 2, NULL) == 2);
	ATF_REQUIRE((kev[0].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[0].data == 0);
	ATF_REQUIRE((kev[1].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[1].data == 0);
#endif
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){
	    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
	};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &eps[0]) == 0);

	ATF_REQUIRE(close(p[0]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 2);
	{
		ATF_REQUIRE(kev[1].ident == (uintptr_t)p[1]);
		ATF_REQUIRE(kev[1].filter == EVFILT_READ);
		ATF_REQUIRE_MSG(kev[1].flags ==
			(EV_EOF | EV_CLEAR | SPURIOUS_EV_ADD | EV_RECEIPT),
		    "%04x", kev[1].flags);
		ATF_REQUIRE(kev[1].fflags == 0);
		ATF_REQUIRE_MSG(kev[1].data == 0, "%d", (int)kev[0].data);
		ATF_REQUIRE(kev[1].udata == 0);
	}
	{
		ATF_REQUIRE(kev[0].ident == (uintptr_t)p[1]);
		ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
		ATF_REQUIRE_MSG(kev[0].flags ==
			(EV_EOF | EV_CLEAR | SPURIOUS_EV_ADD |
			    SPURIOUS_EV_ONESHOT | EV_RECEIPT),
		    "%04x", kev[0].flags);
		ATF_REQUIRE(kev[0].fflags == 0);
		if (kev[0].data == 0) {
			atf_tc_skip(
			    "kev.data == 0 is a valid value on EV_EOF");
		}
		ATF_REQUIRE_MSG(kev[0].data == 16384, "%d", (int)kev[0].data);
		ATF_REQUIRE(kev[0].udata == 0);
	}
	ATF_REQUIRE(close(kq) == 0);
#endif

	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == (EPOLLOUT | EPOLLERR));
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[1]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__closed_write_end);
ATF_TC_BODY_FD_LEAKCHECK(pipe__closed_write_end, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

#if defined(__FreeBSD__)
	{
		cap_rights_t rights;
		ATF_REQUIRE(cap_rights_get(p[0], &rights) == 0);
		cap_rights_clear(&rights, CAP_WRITE);
		ATF_REQUIRE(cap_rights_limit(p[0], &rights) == 0);
	}
#endif

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	ATF_REQUIRE(close(p[1]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[0], EVFILT_READ,  /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);
	EV_SET(&kev[1], p[0], EVFILT_WRITE, /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 2, kev, 2, NULL) == 2);
	ATF_REQUIRE((kev[0].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[0].data == 0);
	ATF_REQUIRE((kev[1].flags & EV_ERROR) != 0);
#ifdef __NetBSD__
	ATF_REQUIRE(kev[1].data == EBADF);
#else
	ATF_REQUIRE(kev[1].data == EPIPE);
#endif

	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
	ATF_REQUIRE(kev[0].filter == EVFILT_READ);
	ATF_REQUIRE_MSG(kev[0].flags ==
		(EV_EOF | (EV_CLEAR | SPURIOUS_EV_ADD) | EV_RECEIPT),
	    "%04x", kev[0].flags);
	ATF_REQUIRE(kev[0].fflags == 0);
	int const pipe_read_size =
#ifdef __NetBSD__
	    16384
#else
	    65536
#endif
	    ;
	ATF_REQUIRE_MSG(kev[0].data == pipe_read_size, "%d", (int)kev[0].data);
	ATF_REQUIRE(kev[0].udata == 0);
	ATF_REQUIRE(close(kq) == 0);
#endif
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){
		    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
		};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == (EPOLLIN | EPOLLHUP));
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLOUT | EPOLLET};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLHUP);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}
	{
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(ep >= 0);

		struct epoll_event eps[32];
		eps[0] = (struct epoll_event){.events = EPOLLET};
		ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
		ATF_REQUIRE(eps[0].events == EPOLLHUP);
		ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 0);

		ATF_REQUIRE(close(ep) == 0);
	}

	ATF_REQUIRE(close(p[0]) == 0);
}

ATF_TC_WITHOUT_HEAD(pipe__closed_write_end_register_before_close);
ATF_TC_BODY_FD_LEAKCHECK(pipe__closed_write_end_register_before_close, tc)
{
	int p[2] = {-1, -1};

	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
	ATF_REQUIRE(p[0] >= 0);
	ATF_REQUIRE(p[1] >= 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
	int kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	struct kevent kev[32];
	EV_SET(&kev[0], p[0], EVFILT_READ,  /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);
	EV_SET(&kev[1], p[0], EVFILT_WRITE, /**/
	    EV_ADD | EV_CLEAR | EV_RECEIPT, /**/
	    0, 0, 0);

	ATF_REQUIRE(kevent(kq, kev, 2, kev, 2, NULL) == 2);
	ATF_REQUIRE((kev[0].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[0].data == 0);
	ATF_REQUIRE((kev[1].flags & EV_ERROR) != 0);
	ATF_REQUIRE(kev[1].data == 0);
#endif
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event eps[32];
	eps[0] = (struct epoll_event){
	    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
	};
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &eps[0]) == 0);

	char c = 0;
	ssize_t r;
	while ((r = write(p[1], &c, 1)) == 1) {
	}
	ATF_REQUIRE(r < 0);
	ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);

	ATF_REQUIRE(close(p[1]) == 0);

#if !defined(__linux__) && !defined(FORCE_EPOLL)
#ifdef __FreeBSD__
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 2);
	{
		ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
		ATF_REQUIRE(kev[0].filter == EVFILT_WRITE);
		ATF_REQUIRE_MSG(kev[0].flags ==
			(EV_EOF | EV_CLEAR | EV_ONESHOT | EV_RECEIPT),
		    "%04x", kev[0].flags);
		ATF_REQUIRE(kev[0].fflags == 0);
		ATF_REQUIRE_MSG(kev[0].data == 4096, "%d", (int)kev[0].data);
		ATF_REQUIRE(kev[0].udata == 0);
	}
	{
		ATF_REQUIRE(kev[1].ident == (uintptr_t)p[0]);
		ATF_REQUIRE(kev[1].filter == EVFILT_READ);
		ATF_REQUIRE_MSG(kev[1].flags ==
			(EV_EOF | EV_CLEAR | EV_RECEIPT),
		    "%04x", kev[1].flags);
		ATF_REQUIRE(kev[1].fflags == 0);
		ATF_REQUIRE_MSG(kev[1].data == 65536, "%d", (int)kev[1].data);
		ATF_REQUIRE(kev[1].udata == 0);
	}
#else
	ATF_REQUIRE(kevent(kq, NULL, 0, kev, nitems(kev),
			&(struct timespec){0, 0}) == 1);
	{
		ATF_REQUIRE(kev[0].ident == (uintptr_t)p[0]);
		ATF_REQUIRE(kev[0].filter == EVFILT_READ);
		ATF_REQUIRE_MSG(kev[0].flags ==
			(EV_EOF | EV_CLEAR | SPURIOUS_EV_ADD | EV_RECEIPT),
		    "%04x", kev[0].flags);
		ATF_REQUIRE(kev[0].fflags == 0);
		ATF_REQUIRE_MSG(kev[0].data == 16384, "%d", (int)kev[1].data);
		ATF_REQUIRE(kev[0].udata == 0);
	}
#endif
	ATF_REQUIRE(close(kq) == 0);
#endif
	ATF_REQUIRE(epoll_wait(ep, eps, 32, 0) == 1);
	ATF_REQUIRE(eps[0].events == (EPOLLIN | EPOLLHUP));
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE(close(p[0]) == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, pipe__simple_poll);
	ATF_TP_ADD_TC(tp, pipe__poll_write_end_after_read_end_close);
	ATF_TP_ADD_TC(tp, pipe__poll_full_write_end_after_read_end_close);
	ATF_TP_ADD_TC(tp, pipe__poll_full_write_end_after_read_end_close_hup);
	ATF_TP_ADD_TC(tp,
	    pipe__poll_full_minus_1b_write_end_after_read_end_close);
	ATF_TP_ADD_TC(tp,
	    pipe__poll_full_minus_511b_write_end_after_read_end_close);
	ATF_TP_ADD_TC(tp,
	    pipe__poll_full_minus_512b_write_end_after_read_end_close);
	ATF_TP_ADD_TC(tp, pipe__pipe_event_poll);
	ATF_TP_ADD_TC(tp, pipe__fifo_event_poll);
	ATF_TP_ADD_TC(tp, pipe__closed_read_end);
	ATF_TP_ADD_TC(tp, pipe__closed_read_end_register_before_close);
	ATF_TP_ADD_TC(tp, pipe__closed_write_end);
	ATF_TP_ADD_TC(tp, pipe__closed_write_end_register_before_close);

	return atf_no_error();
}
