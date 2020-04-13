#define _GNU_SOURCE

#include <atf-c.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef __linux__
#include <sys/event.h>
#endif

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <dlfcn.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "atf-c-leakcheck.h"

static int malloc_fail_cnt = INT_MAX;
static bool need_real_calloc;

static void
decrement_malloc_fail_cnt(void)
{
	--malloc_fail_cnt;
	// write(2, "decr\n", 5);
}

static void *
dlsym_wrapper(void *restrict handle, const char *restrict symbol)
{
	// write(2, "dlsymen\n", 8);

	need_real_calloc = true;
	int old_malloc_fail_cnt = malloc_fail_cnt;
	malloc_fail_cnt = INT_MAX;
	void *ret = dlsym(handle, symbol);
	malloc_fail_cnt = old_malloc_fail_cnt;
	need_real_calloc = false;

	// write(2, "dlsymex\n", 8);

	return ret;
}

#ifdef __FreeBSD__
int
kqueue(void)
{
	// write(2, "kqueue\n", 7);

	if (malloc_fail_cnt <= 0) {
		errno = ENOMEM;
		return -1;
	}

	int (*real_kqueue)(void) =
	    (int (*)(void))dlsym_wrapper(RTLD_NEXT, "kqueue");

	decrement_malloc_fail_cnt();

	return real_kqueue();
}

int
kevent(int kq, const struct kevent *changelist, int nchanges,
    struct kevent *eventlist, int nevents,
    const struct timespec *timeout)
{
	// write(2, "kevent\n", 7);

	if (malloc_fail_cnt <= 0) {
		errno = ENOMEM;
		return -1;
	}

	int (*real_kevent)(int, const struct kevent *, int,
	    struct kevent *, int, const struct timespec *) =
	    (int (*)(int, const struct kevent *, int,
		struct kevent *, int,
		const struct timespec *))dlsym_wrapper(RTLD_NEXT, "kevent");

	decrement_malloc_fail_cnt();

	return real_kevent(kq, changelist, nchanges, eventlist, nevents,
	    timeout);
}
#endif

void *
malloc(size_t size)
{
	// write(2, "malloc\n", 7);

	if (malloc_fail_cnt <= 0) {
		errno = ENOMEM;
		return NULL;
	}

	void *(*real_malloc)(size_t) =
	    (void *(*)(size_t))dlsym_wrapper(RTLD_NEXT, "malloc");

	decrement_malloc_fail_cnt();

	return real_malloc(size);
}

void *
calloc(size_t number, size_t size)
{
	// write(2, "calloc\n", 7);

#ifdef __linux__
	if (need_real_calloc) {
		extern void *__libc_calloc(size_t, size_t);
		return __libc_calloc(number, size);
	}
#endif

	if (malloc_fail_cnt <= 0) {
		errno = ENOMEM;
		return NULL;
	}

	void *(*real_calloc)(size_t, size_t) =
	    (void *(*)(size_t, size_t))dlsym_wrapper(RTLD_NEXT, "calloc");

	decrement_malloc_fail_cnt();

	return real_calloc(number, size);
}

void *
realloc(void *ptr, size_t size)
{
	// write(2, "realloc\n", 8);

	if (malloc_fail_cnt <= 0) {
		errno = ENOMEM;
		return NULL;
	}

	void *(*real_realloc)(void *, size_t) =
	    (void *(*)(void *, size_t))dlsym_wrapper(RTLD_NEXT, "realloc");

	decrement_malloc_fail_cnt();

	return real_realloc(ptr, size);
}

int
pthread_mutex_init(pthread_mutex_t *restrict mutex,
    const pthread_mutexattr_t *restrict attr)
{
	// write(2, "mutexi\n", 7);

	if (malloc_fail_cnt <= 0) {
		return ENOMEM;
	}

	int (*real_pthread_mutex_init)(pthread_mutex_t * restrict,
	    const pthread_mutexattr_t *restrict) =
	    (int (*)(pthread_mutex_t * restrict,
		const pthread_mutexattr_t *restrict))
		dlsym_wrapper(RTLD_NEXT, "pthread_mutex_init");

	decrement_malloc_fail_cnt();

	return real_pthread_mutex_init(mutex, attr);
}

static void *
write_to_pipe_thread_fun(void *arg)
{
	int p = *(int *)arg;

	usleep(100000);

	char c = 0;
	ATF_REQUIRE(write(p, &c, 1) == 1);

	return NULL;
}

ATF_TC_WITHOUT_HEAD(malloc_fail__epoll);
ATF_TC_BODY_FD_LEAKCHECK(malloc_fail__epoll, tc)
{
	int p[2];
	ATF_REQUIRE(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);

	pthread_t thread;

	for (int fail_cnt = 0;; ++fail_cnt) {
		malloc_fail_cnt = INT_MAX;

		if (fail_cnt != 0) {
			ATF_REQUIRE(pthread_join(thread, NULL) == 0);
		}

		{
			char c;
			while (read(p[0], &c, 1) == 1) {
			}
			ATF_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
		}

		ATF_REQUIRE(pthread_create(&thread, NULL,
				write_to_pipe_thread_fun, &p[1]) == 0);

		malloc_fail_cnt = fail_cnt;

		int ep = epoll_create1(EPOLL_CLOEXEC);
		if (ep < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			continue;
		}

		struct epoll_event event = {.events = EPOLLIN};
		int r = epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &event);
		if (r < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			ATF_REQUIRE(close(ep) == 0);
			continue;
		}

		r = epoll_wait(ep, &event, 1, -1);
		if (r < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			ATF_REQUIRE(close(ep) == 0);
			continue;
		}
		ATF_REQUIRE(r == 1);
		ATF_REQUIRE(event.events == POLLIN);

#ifdef close
#undef close
#define NEEDS_EPOLL_SHIM_CLOSE
#endif
		ATF_REQUIRE(close(ep) == 0);
#ifdef NEEDS_EPOLL_SHIM_CLOSE
#define close epoll_shim_close
#endif
		ATF_REQUIRE_ERRNO(EBADF, close(ep) < 0);

		break;
	}

	ATF_REQUIRE(pthread_join(thread, NULL) == 0);

	ATF_REQUIRE(close(p[0]) == 0);
	ATF_REQUIRE(close(p[1]) == 0);

	malloc_fail_cnt = INT_MAX;
}

ATF_TC_WITHOUT_HEAD(malloc_fail__timerfd);
ATF_TC_BODY_FD_LEAKCHECK(malloc_fail__timerfd, tc)
{
	for (int fail_cnt = 0;; ++fail_cnt) {
		malloc_fail_cnt = fail_cnt;

		int tfd = timerfd_create(CLOCK_MONOTONIC,
		    TFD_NONBLOCK | TFD_CLOEXEC);
		if (tfd < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			continue;
		}
		ATF_REQUIRE(close(tfd) == 0);

		break;
	}

	malloc_fail_cnt = INT_MAX;
}

ATF_TC_WITHOUT_HEAD(malloc_fail__eventfd);
ATF_TC_BODY_FD_LEAKCHECK(malloc_fail__eventfd, tc)
{
	for (int fail_cnt = 0;; ++fail_cnt) {
		malloc_fail_cnt = fail_cnt;

		int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (efd < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			continue;
		}
		ATF_REQUIRE(close(efd) == 0);

		break;
	}

	malloc_fail_cnt = INT_MAX;
}

ATF_TC_WITHOUT_HEAD(malloc_fail__signalfd);
ATF_TC_BODY_FD_LEAKCHECK(malloc_fail__signalfd, tc)
{
	for (int fail_cnt = 0;; ++fail_cnt) {
		malloc_fail_cnt = fail_cnt;

		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGINT);

		int sfd = signalfd(-1, &mask, 0);
		if (sfd < 0) {
			ATF_REQUIRE_ERRNO(ENOMEM, true);
			continue;
		}
		ATF_REQUIRE(close(sfd) == 0);

		break;
	}

	malloc_fail_cnt = INT_MAX;
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, malloc_fail__epoll);
	ATF_TP_ADD_TC(tp, malloc_fail__timerfd);
	ATF_TP_ADD_TC(tp, malloc_fail__eventfd);
	ATF_TP_ADD_TC(tp, malloc_fail__signalfd);

	return atf_no_error();
}
