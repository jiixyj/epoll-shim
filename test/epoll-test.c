#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include <sys/epoll.h>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#define XSTR(a) STR(a)
#define STR(a) #a

#define TEST(fun)                                                             \
	if (fun != 0) {                                                       \
		printf(STR(fun) " failed\n");                                 \
	} else {                                                              \
		printf(STR(fun) " successful\n");                             \
	}

static int
test1()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep <= 0) {
		return -1;
	}
	close(ep);
	return 0;
}

static int
test2()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	struct epoll_event event;

	if (epoll_wait(ep, &event, 1, 1) != 0) {
		return -1;
	}

	if (epoll_wait(ep, &event, 1, 0) != 0) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test3()
{
	struct epoll_event event;
#if defined(__amd64__)
	return sizeof(event) == 12 ? 0 : -1;
#else
	// TODO: test for other architectures
	return -1;
#endif
}

static int
test4()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != fds[0]) {
		return -1;
	}

	close(ep);
	return 0;
}

static void *
sleep_then_write(void *arg)
{
	usleep(100000);
	uint8_t data = '\0';
	write((int)(intptr_t)arg, &data, 1);
	return NULL;
}

static int
test5(int sleep)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	pthread_t writer_thread;
	pthread_create(&writer_thread, NULL, sleep_then_write,
	    (void *)(intptr_t)(fds[1]));

	if (epoll_wait(ep, &event, 1, sleep) != 1) {
		return -1;
	}

	pthread_join(writer_thread, NULL);

	close(ep);
	return 0;
}

static int
test6()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL) != -1 ||
	    errno != ENOENT) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test7()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL) == -1) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test8(bool change_udata)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.u32 = 42;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	if (change_udata) {
		event.data.u32 = 43;
	}

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) != -1 ||
	    errno != EEXIST) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) != -1 ||
	    errno != EEXIST) {
		return -1;
	}

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.u32 != 42) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test9()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	event.events = 0;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == -1) {
		return -1;
	}

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, 0) != 0) {
		return -1;
	}

	event.events = EPOLLIN;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == -1) {
		return -1;
	}

	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != fds[0]) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test10()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) != -1 ||
	    errno != ENOENT) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test11()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = fd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == -1) {
		return -1;
	}

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, 300) != 0) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == -1) {
		return -1;
	}

	close(fd);
	close(ep);
	return 0;
}

static int
test12()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	uint8_t data = '\0';
	write(fds[1], &data, 1);
	close(fds[1]);

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.events != (EPOLLHUP | EPOLLIN)) {
		return -1;
	}

	if (read(fds[0], &data, 1) == -1) {
		return -1;
	}

	if (event_result.data.fd != fds[0]) {
		return -1;
	}

	close(ep);
	return 0;
}

static int
test13()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == -1) {
		return -1;
	}

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != fds[1] ||
	    event_result.events != EPOLLOUT) {
		return -1;
	}

	uint8_t data[512] = {0};

	for (int i = 0; i < 128; ++i) {
		write(fds[1], &data, sizeof(data));
	}

	if (epoll_wait(ep, &event_result, 1, 300) != 0) {
		return -1;
	}

	event.events = EPOLLIN;
	event.data.fd = fds[0];
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != fds[0] ||
	    event_result.events != EPOLLIN) {
		return -1;
	}

	close(ep);
	return 0;
}

int
main()
{
	TEST(test1());
	TEST(test2());
	TEST(test3());
	TEST(test4());
	TEST(test5(-1));
	TEST(test5(-2));
	TEST(test6());
	TEST(test7());
	TEST(test8(true));
	TEST(test8(false));
	TEST(test9());
	TEST(test10());
	TEST(test11());
	TEST(test12());
	TEST(test13());
	return 0;
}
