#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include "eventfd_ctx.h"

#include "eventfd_ctx.c"

#define REQUIRE(x)                                                            \
	do {                                                                  \
		if (!(x)) {                                                   \
			fprintf(stderr, "failed assertion: %d\n", __LINE__);  \
			abort();                                              \
		}                                                             \
	} while (0)

static void
tc_init_terminate(void)
{
	int kq;
	EventFDCtx eventfd;

	REQUIRE((kq = kqueue()) >= 0);
	REQUIRE(eventfd_ctx_init(&eventfd, kq, 0,
		    EVENTFD_CTX_FLAG_SEMAPHORE) == 0);
	{
		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
	REQUIRE(close(kq) == 0);

	REQUIRE((kq = kqueue()) >= 0);
	REQUIRE(eventfd_ctx_init(&eventfd, kq, 1,
		    EVENTFD_CTX_FLAG_SEMAPHORE) == 0);
	{
		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		REQUIRE(poll(&pfd, 1, 0) == 1);
		REQUIRE(pfd.revents == POLLIN);
	}
	REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
	REQUIRE(close(kq) == 0);
}

static void
tc_simple_write(void)
{
	int kq;
	EventFDCtx eventfd;

	REQUIRE((kq = kqueue()) >= 0);
	REQUIRE(eventfd_ctx_init(&eventfd, kq, 0, 0) == 0);
	{
		REQUIRE(eventfd_ctx_write(&eventfd, UINT64_MAX) == EINVAL);
		REQUIRE(eventfd_ctx_write(&eventfd, UINT64_MAX - 1) == 0);
		REQUIRE(eventfd_ctx_write(&eventfd, 1) == EAGAIN);
		REQUIRE(eventfd_ctx_write(&eventfd, 1) == EAGAIN);

		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		REQUIRE(poll(&pfd, 1, 0) == 1);
		REQUIRE(pfd.revents == POLLIN);

		uint64_t value;
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == UINT64_MAX - 1);

		REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
	REQUIRE(close(kq) == 0);
}

static void
tc_simple_read(void)
{
	int kq;
	EventFDCtx eventfd;
	uint64_t value;

	REQUIRE((kq = kqueue()) >= 0);
	REQUIRE(eventfd_ctx_init(&eventfd, kq, 3,
		    EVENTFD_CTX_FLAG_SEMAPHORE) == 0);
	{
		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		REQUIRE(poll(&pfd, 1, 0) == 1);
		REQUIRE(pfd.revents == POLLIN);

		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == 1);
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == 1);
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == 1);
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == EAGAIN);

		REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
	REQUIRE(close(kq) == 0);
}

static void
tc_simple_write_read(void)
{
	int kq;
	EventFDCtx eventfd;
	uint64_t value;

	REQUIRE((kq = kqueue()) >= 0);
	REQUIRE(eventfd_ctx_init(&eventfd, kq, 0,
		    EVENTFD_CTX_FLAG_SEMAPHORE) == 0);
	{
		struct pollfd pfd = {.fd = kq, .events = POLLIN};
		REQUIRE(poll(&pfd, 1, 0) == 0);

		REQUIRE(eventfd_ctx_write(&eventfd, 2) == 0);

		REQUIRE(poll(&pfd, 1, 0) == 1);
		REQUIRE(pfd.revents == POLLIN);

		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == 1);
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == 0);
		REQUIRE(value == 1);
		REQUIRE(eventfd_ctx_read(&eventfd, &value) == EAGAIN);

		REQUIRE(poll(&pfd, 1, 0) == 0);
	}
	REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
	REQUIRE(close(kq) == 0);
}

typedef struct {
	EventFDCtx *eventfd;
	int signal_pipe[2];
} ReadThreadArgs;

static atomic_int read_counter;

static void *
read_fun(void *arg)
{
	ReadThreadArgs *args = arg;
	EventFDCtx *eventfd = args->eventfd;

	for (;;) {
		uint64_t value;
		errno_t err;

		if ((err = eventfd_ctx_read(eventfd, &value)) == 0) {
			int current_counter =
			    atomic_fetch_add(&read_counter, 1);

			if (current_counter % 10 == 0 &&
			    current_counter < 100) {
				REQUIRE(eventfd_ctx_write(eventfd, /**/
					    10) == 0);
			}

			continue;
		}

		REQUIRE(err == EAGAIN);

		struct pollfd pfds[2] = {/**/
		    {.fd = eventfd->kq_, .events = POLLIN},
		    {.fd = args->signal_pipe[0], .events = POLLIN}};
		REQUIRE(poll(pfds, nitems(pfds), -1) > 0);

		if (pfds[1].revents) {
			break;
		}
	}

	return (NULL);
}

static void
tc_threads_read(void)
{
	int kq;
	EventFDCtx eventfd;
	pthread_t threads[4];
	ReadThreadArgs thread_args[4];

	for (int i = 0; i < 1000; ++i) {
		read_counter = 0;
		REQUIRE((kq = kqueue()) >= 0);
		REQUIRE(eventfd_ctx_init(&eventfd, kq, 0,
			    EVENTFD_CTX_FLAG_SEMAPHORE) == 0);

		uint64_t counter_val = 100;

		for (int i = 0; i < (int)nitems(threads); ++i) {
			thread_args[i].eventfd = &eventfd;
			REQUIRE(pipe2(thread_args[i].signal_pipe,
				    O_CLOEXEC | O_NONBLOCK) == 0);
			REQUIRE(pthread_create(&threads[i], NULL, /**/
				    read_fun, &thread_args[i]) == 0);
		}

		REQUIRE(eventfd_ctx_write(&eventfd, counter_val) == 0);

		while (atomic_load(&read_counter) != 2 * (int)counter_val) {
		}

		for (int i = 0; i < (int)nitems(threads); ++i) {
			REQUIRE(close(thread_args[i].signal_pipe[1]) == 0);
			REQUIRE(pthread_join(threads[i], NULL) == 0);
			REQUIRE(close(thread_args[i].signal_pipe[0]) == 0);
		}

		REQUIRE(eventfd_ctx_terminate(&eventfd) == 0);
		REQUIRE(close(kq) == 0);
		REQUIRE(read_counter == 2 * counter_val);
	}
}

int
main()
{
	tc_init_terminate();
	tc_simple_write();
	tc_simple_read();
	tc_simple_write_read();
	tc_threads_read();
}
