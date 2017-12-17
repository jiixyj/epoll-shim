#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define XSTR(a) STR(a)
#define STR(a) #a

#define TEST(fun)                                                             \
	if ((fun) != 0) {                                                     \
		printf(STR((fun)) " failed\n");                               \
	} else {                                                              \
		printf(STR((fun)) " successful\n");                           \
	}

static int
fd_pipe(int fds[3])
{
	fds[2] = -1;
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return -1;
	}
	return 0;
}

static int
fd_domain_socket(int fds[3])
{
	fds[2] = -1;
	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fds) < 0) {
		return -1;
	}
	return 0;
}

static void *
connector_client(void *arg)
{
	(void)arg;

	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return NULL;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return NULL;
	}

	return (void *)(intptr_t)sock;
}

static int
fd_tcp_socket(int fds[3])
{
	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return -1;
	}

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, /**/
		&enable, sizeof(int)) < 0) {
		return -1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -1;
	}

	if (bind(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return -1;
	}

	if (listen(sock, 5) < 0) {
		return -1;
	}

	pthread_t client_thread;
	if (pthread_create(&client_thread, NULL, connector_client, NULL) < 0) {
		return -1;
	}

	int conn = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
	if (conn < 0) {
		return -1;
	}

	void *client_socket = NULL;

	if (pthread_join(client_thread, &client_socket) < 0) {
		return -1;
	}

	fds[0] = conn;
	fds[1] = (int)(intptr_t)client_socket;
	fds[2] = sock;
	return 0;
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
	if (ep < 0) {
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
	// TODO(jan): test for other architectures
	return -1;
#endif
}

static int
test4(int (*fd_fun)(int fds[3]))
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_fun(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
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
	close(fds[2]);
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.u32 = 42;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
		return -1;
	}

	event.events = 0;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) < 0) {
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
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = fd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) < 0) {
		return -1;
	}

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, 300) != 0) {
		return -1;
	}

	if (epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
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

	if (read(fds[0], &data, 1) < 0) {
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) < 0) {
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
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
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

	if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
		return -1;
	}

	new_value.it_value.tv_sec = now.tv_sec + 1;
	new_value.it_value.tv_nsec = now.tv_nsec;
	new_value.it_interval.tv_sec = 0;
	new_value.it_interval.tv_nsec = 100000000;

	int fd = timerfd_create(CLOCK_REALTIME, 0);
	if (fd < 0) {
		return -1;
	}

	if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL) < 0) {
		return -1;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = fd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) < 0) {
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

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		return -1;
	}

	sfd = signalfd(-1, &mask, 0);
	if (sfd < 0) {
		return -1;
	}

	kill(getpid(), SIGINT);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = sfd;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &event) < 0) {
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

	int fds[3];
	if (fd_pipe(fds) < 0) {
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
	(void)arg;

	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return NULL;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return NULL;
	}

	fprintf(stderr, "got client\n");

	shutdown(sock, SHUT_WR);
	usleep(300000);

	close(sock);

	return NULL;
}

static int
test16(bool specify_rdhup) {
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_tcp_socket(fds) < 0) {
		return -1;
	}

	int rdhup_flag = specify_rdhup ? EPOLLRDHUP : 0;

	struct epoll_event event;
	event.events = EPOLLIN | (specify_rdhup ? 0 : EPOLLRDHUP);
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
		return -1;
	}

	event.events = EPOLLIN | rdhup_flag;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) < 0) {
		return -1;
	}

	shutdown(fds[1], SHUT_WR);

	for (;;) {
		if (epoll_wait(ep, &event, 1, -1) != 1) {
			return -1;
		}

		fprintf(stderr, "got event: %x\n",
		    (int)event.events);

		if (event.events == (EPOLLIN | rdhup_flag)) {
			uint8_t buf;
			ssize_t ret = read(fds[0], &buf, 1);

			if (ret != 0) {
				return -1;
			}

			shutdown(fds[0], SHUT_RDWR);
		} else if (event.events == (EPOLLIN | rdhup_flag | EPOLLHUP)) {
			close(fds[0]);
			break;
		} else {
			return -1;
		}
	}

	if (epoll_wait(ep, &event, 1, 300) != 0) {
		return -1;
	}

	close(fds[1]);
	close(fds[2]);

	close(ep);

	return 0;
}

static int
test17()
{
	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return -1;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = sock;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, sock, &event) < 0) {
		return -1;
	}

	int ret = epoll_wait(ep, &event, 1, 100);
	if (!(ret == 0 || ret == 1)) {
		return -1;
	}

	// TODO(jan): Linux returns EPOLLHUP, FreeBSD times out
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
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_pipe(fds) < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) < 0) {
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
test20(int (*fd_fun)(int fds[3]))
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	int fds[3];
	if (fd_fun(fds) < 0) {
		return -1;
	}

	shutdown(fds[0], SHUT_WR);

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) < 0) {
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
		} else if (fd_fun == fd_domain_socket &&
		    (event_result.events & (EPOLLOUT | EPOLLHUP)) ==
			(EPOLLOUT | EPOLLHUP)) {
			// TODO(jan): Linux sets EPOLLERR in addition
			{
				int error = 0;
				socklen_t errlen = sizeof(error);
				getsockopt(fds[1], SOL_SOCKET, SO_ERROR,
				    (void *)&error, &errlen);
				fprintf(stderr, "socket error: %d (%s)\n",
				    error, strerror(error));
			}
			break;
		} else if (fd_fun == fd_tcp_socket &&
		    event_result.events == (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
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
		usleep(100000);
	}

	close(fds[1]);
	close(fds[2]);
	close(ep);

	return 0;
}

static void *
connector2(void *arg)
{
	(void)arg;

	int sock = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return NULL;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return NULL;
	}

	fprintf(stderr, "got client\n");

	uint8_t data = '\0';
	write(sock, &data, 0);
	usleep(500000);
	close(sock);

	return NULL;
}

static int
test21()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	int sock = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return -1;
	}

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, /**/
		&enable, sizeof(int)) < 0) {
		return -1;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -1;
	}

	if (bind(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return -1;
	}

	pthread_t client_thread;
	pthread_create(&client_thread, NULL, connector2, NULL);

	int fds[2];
	fds[0] = sock;

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = fds[0];

	if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0) {
		return -1;
	}

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	fprintf(stderr, "got event: %x %d\n", (int)event_result.events,
	    (int)event_result.events);

	if (event_result.events != EPOLLIN) {
		return -1;
	}

	uint8_t data = '\0';
	if (read(fds[0], &data, 1) < 0) {
		return -1;
	}

	if (event_result.data.fd != fds[0]) {
		return -1;
	}

	pthread_join(client_thread, NULL);

	close(fds[0]);
	close(ep);
	return 0;
}

static int
test22()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

#define FDS_SIZE 13

	int fds[FDS_SIZE][3];

	for (int i = 0; i < FDS_SIZE; ++i) {
		if (fd_domain_socket(fds[i]) < 0) {
			return -1;
		}
	}

	uint8_t data = '\0';
	for (int i = 0; i < FDS_SIZE; ++i) {
		write(fds[i][1], &data, 1);
	}

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;

	for (int i = 0; i < FDS_SIZE; ++i) {
		event.data.fd = fds[i][0];
		if (epoll_ctl(ep, EPOLL_CTL_ADD, fds[i][0], &event) < 0) {
			return -1;
		}
	}

	for (int j = 0; j < 100; ++j) {
		struct epoll_event event_result[32];
		int event_size;
		if ((event_size = epoll_wait(ep, event_result, 32, -1)) < 0) {
			return -1;
		}

		for (int i = 0; i < event_size; ++i) {
			fprintf(stderr, "got event %d: %x %d dat: %d\n", i,
			    (int)event_result[i].events,
			    (int)event_result[i].events,
			    event_result[i].data.fd);

			if ((event_result[i].events & EPOLLIN) && !(i % 4)) {
				char data;
				read(event_result[i].data.fd, &data, 1);
			}
		}

		fprintf(stderr, "\n");

		usleep(1000000);

		if (!(j % 5)) {
			for (int i = 0; i < FDS_SIZE; ++i) {
				write(fds[i][1], &data, 1);
			}
		}
	}

	for (int i = 0; i < FDS_SIZE; ++i) {
		close(fds[i][0]);
		close(fds[i][1]);
	}

#undef FDS_SIZE

	close(ep);
	return 0;
}

static int
test23()
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0) {
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = 0;

	if (epoll_ctl(ep, EPOLL_CTL_ADD, 0, &event) < 0) {
		return -1;
	}

	fprintf(stderr, "press Ctrl+D\n");

	struct epoll_event event_result;
	if (epoll_wait(ep, &event_result, 1, -1) != 1) {
		return -1;
	}

	if (event_result.data.fd != 0) {
		return -1;
	}

	fprintf(stderr, "got event: %x\n", (int)event.events);

	char c;
	if (read(0, &c, 1) != 0) {
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
	TEST(test4(fd_pipe));
	TEST(test4(fd_domain_socket));
	TEST(test4(fd_tcp_socket));
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
	TEST(test20(fd_tcp_socket));
	TEST(test20(fd_domain_socket));
	TEST(test21());
	// TEST(test22());
	TEST(test23());

	TEST(testxx());
	return 0;
}
