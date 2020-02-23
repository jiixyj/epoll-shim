#define _GNU_SOURCE

#include <atf-c.h>

#include <sys/eventfd.h>
#include <sys/param.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include "atf-c-leakcheck.h"

#define nitems(x) (sizeof((x)) / sizeof((x)[0]))

ATF_TC_WITHOUT_HEAD(eventfd__init_terminate);
ATF_TC_BODY_FD_LEAKCHECK(eventfd__init_terminate, tc)
{
	int efd;

	ATF_REQUIRE((efd = eventfd(0,
			 EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) >= 0);
	{
		struct pollfd pfd = {.fd = efd, .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	ATF_REQUIRE(close(efd) == 0);

	ATF_REQUIRE((efd = eventfd(1,
			 EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) >= 0);
	{
		struct pollfd pfd = {.fd = efd, .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
		ATF_REQUIRE(pfd.revents == POLLIN);
	}
	ATF_REQUIRE(close(efd) == 0);
}

ATF_TC_WITHOUT_HEAD(eventfd__simple_write);
ATF_TC_BODY_FD_LEAKCHECK(eventfd__simple_write, tc)
{
	int efd;

	ATF_REQUIRE((efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) >= 0);
	{
		ATF_REQUIRE_ERRNO(EINVAL, eventfd_write(efd, UINT64_MAX) < 0);
		ATF_REQUIRE(eventfd_write(efd, UINT64_MAX - 1) == 0);
		ATF_REQUIRE_ERRNO(EAGAIN, eventfd_write(efd, 1) < 0);
		ATF_REQUIRE_ERRNO(EAGAIN, eventfd_write(efd, 1) < 0);

		struct pollfd pfd = {.fd = efd, .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
		ATF_REQUIRE(pfd.revents == POLLIN);

		uint64_t value;
		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == UINT64_MAX - 1);

		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	ATF_REQUIRE(close(efd) == 0);
}

ATF_TC_WITHOUT_HEAD(eventfd__simple_read);
ATF_TC_BODY_FD_LEAKCHECK(eventfd__simple_read, tc)
{
	int efd;
	uint64_t value;

	ATF_REQUIRE((efd = eventfd(3,
			 EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) >= 0);
	{
		struct pollfd pfd = {.fd = efd, .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
		ATF_REQUIRE(pfd.revents == POLLIN);

		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == 1);
		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == 1);
		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == 1);
		ATF_REQUIRE_ERRNO(EAGAIN, eventfd_read(efd, &value) < 0);

		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	ATF_REQUIRE(close(efd) == 0);
}

ATF_TC_WITHOUT_HEAD(eventfd__simple_write_read);
ATF_TC_BODY_FD_LEAKCHECK(eventfd__simple_write_read, tc)
{
	int efd;
	uint64_t value;

	ATF_REQUIRE((efd = eventfd(0,
			 EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) >= 0);
	{
		struct pollfd pfd = {.fd = efd, .events = POLLIN};
		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);

		ATF_REQUIRE(eventfd_write(efd, 2) == 0);

		ATF_REQUIRE(poll(&pfd, 1, 0) == 1);
		ATF_REQUIRE(pfd.revents == POLLIN);

		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == 1);
		ATF_REQUIRE(eventfd_read(efd, &value) == 0);
		ATF_REQUIRE(value == 1);
		ATF_REQUIRE_ERRNO(EAGAIN, eventfd_read(efd, &value) < 0);

		ATF_REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	ATF_REQUIRE(close(efd) == 0);
}

typedef struct {
	int efd;
	int signal_pipe[2];
} ReadThreadArgs;

static atomic_int read_counter;

static void *
read_fun(void *arg)
{
	ReadThreadArgs *args = arg;
	int efd = args->efd;

	for (;;) {
		uint64_t value;

		if (eventfd_read(efd, &value) == 0) {
			int current_counter =
			    atomic_fetch_add(&read_counter, 1);

			if (current_counter % 10 == 0 &&
			    current_counter < 100) {
				ATF_REQUIRE(eventfd_write(efd, 10) == 0);
			}

			continue;
		}

		ATF_REQUIRE(errno == EAGAIN);

		struct pollfd pfds[2] = {/**/
		    {.fd = efd, .events = POLLIN},
		    {.fd = args->signal_pipe[0], .events = POLLIN}};
		ATF_REQUIRE(poll(pfds, nitems(pfds), -1) > 0);

		if (pfds[1].revents) {
			break;
		}
	}

	return (NULL);
}

ATF_TC_WITHOUT_HEAD(eventfd__threads_read);
ATF_TC_BODY_FD_LEAKCHECK(eventfd__threads_read, tc)
{
	int efd;
	pthread_t threads[4];
	ReadThreadArgs thread_args[4];

	for (int i = 0; i < 1000; ++i) {
		read_counter = 0;
		ATF_REQUIRE(
		    (efd = eventfd(0,
			 EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) >= 0);

		uint64_t counter_val = 100;

		for (int i = 0; i < (int)nitems(threads); ++i) {
			thread_args[i].efd = efd;
			ATF_REQUIRE(pipe2(thread_args[i].signal_pipe,
					O_CLOEXEC | O_NONBLOCK) == 0);
			ATF_REQUIRE(pthread_create(&threads[i], NULL, /**/
					read_fun, &thread_args[i]) == 0);
		}

		ATF_REQUIRE(eventfd_write(efd, counter_val) == 0);

		while (atomic_load(&read_counter) != 2 * (int)counter_val) {
		}

		for (int i = 0; i < (int)nitems(threads); ++i) {
			ATF_REQUIRE(close(thread_args[i].signal_pipe[1]) == 0);
			ATF_REQUIRE(pthread_join(threads[i], NULL) == 0);
			ATF_REQUIRE(close(thread_args[i].signal_pipe[0]) == 0);
		}

		ATF_REQUIRE(close(efd) == 0);
		ATF_REQUIRE(read_counter == 2 * counter_val);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, eventfd__init_terminate);
	ATF_TP_ADD_TC(tp, eventfd__simple_write);
	ATF_TP_ADD_TC(tp, eventfd__simple_read);
	ATF_TP_ADD_TC(tp, eventfd__simple_write_read);
	ATF_TP_ADD_TC(tp, eventfd__threads_read);

	return atf_no_error();
}
