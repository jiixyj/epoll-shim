#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

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

	close(fds[0]);
	close(fds[1]);
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
	close(fds[0]);
	close(fds[1]);
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
	close(fds[0]);
	close(fds[1]);
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
	close(fds[0]);
	close(fds[1]);
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

	close(fds[0]);
	close(fds[1]);
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
	event.data.fd = 42;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == -1) {
		return -1;
	}

	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != 42) {
		return -1;
	}

	close(fds[0]);
	close(fds[1]);
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

	close(fds[0]);
	close(fds[1]);
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
test12(bool do_write_data)
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
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == -1) {
		return -1;
	}

	uint8_t data = '\0';
	if (do_write_data) {
		write(fds[1], &data, 1);
	}
	close(fds[1]);

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.events !=
	    (EPOLLHUP | (do_write_data ? EPOLLIN : 0))) {
		return -1;
	}

	if (read(fds[0], &data, 1) == -1) {
		return -1;
	}

	if (event_result.data.fd != fds[0]) {
		return -1;
	}

	close(fds[0]);
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

	close(fds[0]);
	close(fds[1]);
	close(ep);
	return 0;
}

static int
test14()
{
	struct itimerspec new_value;
	struct timespec now;
	uint64_t exp, tot_exp;
	ssize_t s;

	if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
		return -1;
	}

	new_value.it_value.tv_sec = now.tv_sec + 1;
	new_value.it_value.tv_nsec = now.tv_nsec;
	new_value.it_interval.tv_sec = 0;
	new_value.it_interval.tv_nsec = 100000000;

	int fd = timerfd_create(CLOCK_REALTIME, 0);
	if (fd == -1) {
		return -1;
	}

	if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL) == -1) {
		return -1;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = fd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == -1) {
		return -1;
	}

	struct epoll_event event_result;

	for (tot_exp = 0; tot_exp < 3;) {
		if (epoll_wait(ep, &event_result, 1, -1) != 1) {
			return -1;
		}

		if (event_result.events != EPOLLIN ||
		    event_result.data.fd != fd) {
			return -1;
		}

		s = read(fd, &exp, sizeof(uint64_t));
		if (s != sizeof(uint64_t)) {
			return -1;
		}

		tot_exp += exp;
		printf("read: %llu; total=%llu\n", (unsigned long long)exp,
		    (unsigned long long)tot_exp);
	}

	close(ep);
	close(fd);
	return 0;
}

static int
test15()
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		return -1;
	}

	sfd = signalfd(-1, &mask, 0);
	if (sfd == -1) {
		return -1;
	}

	kill(getpid(), SIGINT);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = sfd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &event) == -1) {
		return -1;
	}

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.events != EPOLLIN || event_result.data.fd != sfd) {
		return -1;
	}

	s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
	if (s != sizeof(struct signalfd_siginfo)) {
		return -1;
	}

	if (fdsi.ssi_signo != SIGINT) {
		return -1;
	}

	close(ep);
	close(sfd);

	return 0;
}

static int
testxx()
{
	/* test that all fds of previous tests have been closed successfully */

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) == -1) {
		return -1;
	}

	if (fds[0] != 3 || fds[1] != 4) {
		return -1;
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
}

static void *
connector(void *arg)
{
	int sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		return NULL;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, &addr, sizeof(addr)) == -1) {
		return NULL;
	}

	fprintf(stderr, "got client\n");

	shutdown(sock, SHUT_WR);
	usleep(300000);

	close(sock);

	return NULL;
}

static int
test16(bool specify_rdhup)
{
	int sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		return -1;
	}

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) ==
	    -1) {
		return -1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -1;
	}

	if (bind(sock, &addr, sizeof(addr)) == -1) {
		return -1;
	}

	if (listen(sock, 5) == -1) {
		return -1;
	}

	pthread_t client_thread;
	pthread_create(&client_thread, NULL, connector, NULL);

	int conn = accept(sock, NULL, NULL);
	if (conn == -1) {
		return -1;
	}

	// if (shutdown(conn, SHUT_RDWR) == -1) {
	// 	return -1;
	// }

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int rdhup_flag = specify_rdhup ? EPOLLRDHUP : 0;

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = conn;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, conn, &event) == -1) {
		return -1;
	}

	event.events = EPOLLIN | rdhup_flag;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, conn, &event) == -1) {
		return -1;
	}

	for (;;) {
		if (epoll_wait(ep, &event, 1, -1) != 1) {
			return -1;
		}

		fprintf(stderr, "got event: %x\n",
		    (int)event.events);

		if (event.events == (EPOLLIN | rdhup_flag)) {
			uint8_t buf;
			ssize_t ret = read(conn, &buf, 1);

			if (ret != 0) {
				return -1;
			}

			shutdown(conn, SHUT_RDWR);
		} else if (event.events == (EPOLLIN | rdhup_flag | EPOLLHUP)) {
			close(conn);
			break;
		} else {
			return -1;
		}
	}

	if (epoll_wait(ep, &event, 1, 300) != 0) {
		return -1;
	}

	pthread_join(client_thread, NULL);

	close(sock);

	return 0;
}

static int
test17()
{
	int sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		return -1;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = sock;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, sock, &event) == -1) {
		return -1;
	}

	int ret = epoll_wait(ep, &event, 1, 100);
	if (!(ret == 0 || ret == 1)) {
		return -1;
	}

	// TODO: Linux returns EPOLLHUP, FreeBSD times out
	if (!(event.events == EPOLLHUP || ret == 0)) {
		return -1;
	}

	close(ep);
	close(sock);

	return 0;
}

static int
test18()
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

	for (;;) {
		struct epoll_event event_result;
		if (epoll_wait(ep, &event_result, 1, -1) != 1) {
			return -1;
		}

		fprintf(stderr, "got event: %x %d\n", (int)event_result.events,
		    (int)event_result.events);

		if (event_result.data.fd != fds[1]) {
			return -1;
		}

		if (event_result.events == EPOLLOUT) {
			// continue
		} else if (event_result.events == (EPOLLOUT | EPOLLERR)) {
			break;
		} else {
			return -1;
		}

		uint8_t data[512] = {0};
		write(fds[1], &data, sizeof(data));

		close(fds[0]);
	}

	close(fds[0]);
	close(fds[1]);
	close(ep);
	return 0;
}

static int
test19()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int fds[2];
	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fds) == -1) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == -1) {
		return -1;
	}

	for (;;) {
		struct epoll_event event_result;
		if (epoll_wait(ep, &event_result, 1, -1) != 1) {
			return -1;
		}

		fprintf(stderr, "got event: %x %d\n", (int)event_result.events,
		    (int)event_result.events);

		if (event_result.data.fd != fds[1]) {
			return -1;
		}

		if (event_result.events == EPOLLOUT) {
			// continue
		} else if ((event_result.events & (EPOLLOUT | EPOLLHUP)) ==
		    (EPOLLOUT | EPOLLHUP)) {
			// TODO: Linux sets EPOLLERR in addition
			{
				int error = 0;
				socklen_t errlen = sizeof(error);
				getsockopt(fds[1], SOL_SOCKET, SO_ERROR,
				    (void *)&error, &errlen);
				fprintf(stderr, "socket error: %d (%s)\n",
				    error, strerror(error));
			}
			break;
		} else {
			return -1;
		}

		uint8_t data[512] = {0};
		write(fds[1], &data, sizeof(data));

		close(fds[0]);
	}

	close(fds[0]);
	close(fds[1]);
	close(ep);
	return 0;
}

static int
test20()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep == -1) {
		return -1;
	}

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		return -1;
	}

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) ==
	    -1) {
		return -1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -1;
	}

	if (bind(sock, &addr, sizeof(addr)) == -1) {
		return -1;
	}

	if (listen(sock, 5) == -1) {
		return -1;
	}

	pthread_t client_thread;
	pthread_create(&client_thread, NULL, connector, NULL);

	int conn = accept(sock, NULL, NULL);
	if (conn == -1) {
		return -1;
	}

	int fds[2];
	fds[1] = conn;

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == -1) {
		return -1;
	}

	for (;;) {
		struct epoll_event event_result;
		if (epoll_wait(ep, &event_result, 1, -1) != 1) {
			return -1;
		}

		if (event_result.data.fd != fds[1]) {
			return -1;
		}

		if (event_result.events == EPOLLOUT) {
			// continue
		} else if (event_result.events ==
		    (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
			{
				int error = 0;
				socklen_t errlen = sizeof(error);
				getsockopt(fds[1], SOL_SOCKET, SO_ERROR,
				    (void *)&error, &errlen);
				fprintf(stderr, "socket error: %d (%s)\n",
				    error, strerror(error));
			}
			break;
		} else {
			return -1;
		}

		uint8_t data[512] = {0};
		write(fds[1], &data, sizeof(data));
	}

	close(fds[1]);
	close(ep);

	pthread_join(client_thread, NULL);
	close(sock);
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
	TEST(test12(false));
	TEST(test12(true));
	TEST(test13());
	TEST(test14());
	TEST(test15());
	TEST(test16(true));
	TEST(test16(false));
	TEST(test17());
	TEST(test18());
	TEST(test19());
	TEST(test20());

	TEST(testxx());
	return 0;
}
