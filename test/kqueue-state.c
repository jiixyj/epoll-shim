#include <sys/types.h>

#include <sys/event.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <err.h>

#include "../src/epollfd_ctx.c"

int
main()
{
	int kq;
	errno_t ec;
	uint16_t retval;

	kq = kqueue();
	if (kq < 0) {
		err(1, "kqueue");
	}

	if ((ec = kqueue_save_state(kq, 42, 0xfffu)) != 0) {
		errno = ec;
		err(1, "kqueue_save_state");
	}

	if ((ec = kqueue_save_state(kq, 42, 0xf0fu)) != 0) {
		errno = ec;
		err(1, "kqueue_save_state");
	}

	if ((ec = kqueue_save_state(kq, 41, 0x123u)) != 0) {
		errno = ec;
		err(1, "kqueue_save_state");
	}

	if ((ec = kqueue_load_state(kq, 42, &retval)) != 0) {
		errno = ec;
		err(1, "kqueue_load_state");
	}

	fprintf(stderr, "got %x, expected %x\n", (unsigned)retval, 0xf0fu);

	if ((ec = kqueue_load_state(kq, 41, &retval)) != 0) {
		errno = ec;
		err(1, "kqueue_load_state");
	}

	fprintf(stderr, "got %x, expected %x\n", (unsigned)retval, 0x123u);
}
