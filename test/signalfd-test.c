#include <atf-c.h>

#include <sys/types.h>

#include <sys/param.h>
#include <sys/select.h>
#include <sys/time.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <err.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/signalfd.h>

#include "atf-c-leakcheck.h"

ATF_TC_WITHOUT_HEAD(signalfd__simple_signalfd);
ATF_TC_BODY_FD_LEAKCHECK(signalfd__simple_signalfd, tcptr)
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

	sfd = signalfd(-1, &mask, 0);
	ATF_REQUIRE(sfd >= 0);

	kill(getpid(), SIGINT);

	s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
	ATF_REQUIRE(s == sizeof(struct signalfd_siginfo));

	ATF_REQUIRE(fdsi.ssi_signo == SIGINT);

	ATF_REQUIRE(close(sfd) == 0);
}

static void *
sleep_then_kill(void *arg)
{
	(void)arg;
	usleep(300000);
	kill(getpid(), SIGINT);
	return NULL;
}

ATF_TC_WITHOUT_HEAD(signalfd__blocking_read);
ATF_TC_BODY_FD_LEAKCHECK(signalfd__blocking_read, tcptr)
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

	sfd = signalfd(-1, &mask, 0);
	ATF_REQUIRE(sfd >= 0);

	pthread_t writer_thread;
	ATF_REQUIRE(
	    pthread_create(&writer_thread, NULL, sleep_then_kill, NULL) == 0);

	s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
	ATF_REQUIRE(s == sizeof(struct signalfd_siginfo));

	ATF_REQUIRE(fdsi.ssi_signo == SIGINT);

	ATF_REQUIRE(pthread_join(writer_thread, NULL) == 0);

	ATF_REQUIRE(close(sfd) == 0);
}

ATF_TC_WITHOUT_HEAD(signalfd__nonblocking_read);
ATF_TC_BODY_FD_LEAKCHECK(signalfd__nonblocking_read, tcptr)
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

	sfd = signalfd(-1, &mask, SFD_NONBLOCK);
	ATF_REQUIRE(sfd >= 0);

	ATF_REQUIRE_ERRNO(EAGAIN,
	    read(sfd, &fdsi, sizeof(struct signalfd_siginfo)) < 0);

	pthread_t writer_thread;
	ATF_REQUIRE(
	    pthread_create(&writer_thread, NULL, sleep_then_kill, NULL) == 0);

	int read_counter = 0;
	do {
		++read_counter;
		s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
	} while (s < 0 && errno == EAGAIN);

	ATF_REQUIRE(s == sizeof(struct signalfd_siginfo));
	ATF_REQUIRE_MSG(read_counter > 10, "%d", read_counter);

	ATF_REQUIRE(fdsi.ssi_signo == SIGINT);

	ATF_REQUIRE(pthread_join(writer_thread, NULL) == 0);

	ATF_REQUIRE(close(sfd) == 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, signalfd__simple_signalfd);
	ATF_TP_ADD_TC(tp, signalfd__blocking_read);
	ATF_TP_ADD_TC(tp, signalfd__nonblocking_read);

	return atf_no_error();
}
