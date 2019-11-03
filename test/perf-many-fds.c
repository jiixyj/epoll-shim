#include <sys/eventfd.h>

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(x)                                                            \
	do {                                                                  \
		if (!(x)) {                                                   \
			fprintf(stderr, "REQUIRE in line %d failed.\n",       \
			    __LINE__);                                        \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
	} while (0)

#define NR_EVENTFDS (50000)

int
main()
{
	int *eventfds = malloc(NR_EVENTFDS * sizeof(int));
	REQUIRE(eventfds);

	for (long i = 0; i < NR_EVENTFDS; ++i) {
		eventfds[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		REQUIRE(eventfds[i] >= 0);
	}

	for (long i = 0; i < 1000000; ++i) {
		REQUIRE(eventfd_write(eventfds[0], 1) == 0);
		if (i % 10000 == 0) {
			fprintf(stderr, ".");
		}
	}
}
