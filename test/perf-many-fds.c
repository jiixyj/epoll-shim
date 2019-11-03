#include <sys/eventfd.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REQUIRE(x)                                                            \
	do {                                                                  \
		if (!(x)) {                                                   \
			fprintf(stderr, "REQUIRE in line %d failed.\n",       \
			    __LINE__);                                        \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
	} while (0)

#define NR_EVENTFDS (20000)

int
main()
{
	struct timespec time1;
	struct timespec time2;

	REQUIRE(clock_gettime(CLOCK_MONOTONIC, &time1) == 0);

	int *eventfds = malloc(NR_EVENTFDS * sizeof(int));
	REQUIRE(eventfds);

	for (long i = 0; i < NR_EVENTFDS; ++i) {
		eventfds[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		REQUIRE(eventfds[i] >= 0);
	}

	for (long i = 0; i < 2000000; ++i) {
		REQUIRE(eventfd_write(eventfds[0], 1) == 0);
		if (i % 10000 == 0) {
			fprintf(stderr, ".");
		}
	}

	REQUIRE(clock_gettime(CLOCK_MONOTONIC, &time2) == 0);
	REQUIRE(time2.tv_sec - time1.tv_sec < 15);
}
