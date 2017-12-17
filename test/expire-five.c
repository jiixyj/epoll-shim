/*
 * Adapted from sghctoma's example here:
 * https://github.com/jiixyj/epoll-shim/issues/2
 *
 * The SIGUSR1 signal should not kill the process.
 */

#include <err.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/timerfd.h>

int
main()
{
	int fd;
	struct itimerspec value;
	uint64_t total_exp = 0;

	if ((fd = timerfd_create(CLOCK_MONOTONIC, /**/
		 TFD_CLOEXEC | TFD_NONBLOCK)) < 0) {
		err(1, "timerfd_create");
	}

	value.it_value.tv_sec = 3;
	value.it_value.tv_nsec = 0;
	value.it_interval.tv_sec = 1;
	value.it_interval.tv_nsec = 0;

	if (timerfd_settime(fd, 0, &value, NULL) < 0) {
		err(1, "timerfd_settime");
	}

	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	kill(getpid(), SIGUSR1);

	for (;;) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int n;
		ssize_t r;
		uint64_t exp;

		n = poll(&pfd, 1, -1);
		if (n < 0) {
			err(1, "poll");
		}

		r = read(fd, &exp, sizeof(uint64_t));
		if (r < 0) {
			err(1, "read");
		}
		if (r != sizeof(uint64_t)) {
			errx(1, "invalid read from timerfd");
		}

		printf("timer expired %u times\n", (unsigned)exp);

		total_exp += exp;
		if (total_exp >= 5) {
			break;
		}
	}

	close(fd);

	return (0);
}
