#include <sys/event.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <err.h>

#include "../src/epoll.c"

int
main()
{
	int kq;
	int e;
	uint16_t retval;

	kq = kqueue();
	if (kq < 0) {
		err(1, "kqueue");
	}

	if ((e = kqueue_save_state(kq, 42, 0xfffu)) < 0) {
		errno = -e;
		err(1, "kqueue_save_state");
	}

	if ((e = kqueue_save_state(kq, 42, 0xf0fu)) < 0) {
		errno = -e;
		err(1, "kqueue_save_state");
	}

	if ((e = kqueue_save_state(kq, 41, 0x123u)) < 0) {
		errno = -e;
		err(1, "kqueue_save_state");
	}

	if ((e = kqueue_load_state(kq, 42, &retval)) < 0) {
		errno = -e;
		err(1, "kqueue_load_state");
	}

	fprintf(stderr, "got %x, expected %x\n", (unsigned)retval, 0xf0fu);

	if ((e = kqueue_load_state(kq, 41, &retval)) < 0) {
		errno = -e;
		err(1, "kqueue_load_state");
	}

	fprintf(stderr, "got %x, expected %x\n", (unsigned)retval, 0x123u);
}
