#ifndef TIMERFD_CTX_H_
#define TIMERFD_CTX_H_

#include <sys/timespec.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <pthread.h>

enum timerfd_kind {
	TIMERFD_KIND_UNDETERMINED,
	TIMERFD_KIND_SIMPLE,
	TIMERFD_KIND_COMPLEX,
};

typedef struct {
	int kq; // non owning
	int flags;
	pthread_mutex_t mutex;
	enum timerfd_kind kind;
	union {
		struct {
			struct itimerspec current_itimerspec;
		} simple;
		struct {
			pthread_t worker;
			timer_t timer;
			uint64_t current_expirations;
		} complx;
	};
} TimerFDCtx;

errno_t timerfd_ctx_init(TimerFDCtx *timerfd, int kq, int clockid);
errno_t timerfd_ctx_terminate(TimerFDCtx *timerfd);

errno_t timerfd_ctx_settime(TimerFDCtx *timerfd, int flags,
    const struct itimerspec *new, struct itimerspec *old);

errno_t timerfd_ctx_read(TimerFDCtx *timerfd, uint64_t *value);

#endif
