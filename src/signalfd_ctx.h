#ifndef SIGNALFD_CTX_H_
#define SIGNALFD_CTX_H_

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#include <pthread.h>

#include "kqueue_event.h"

typedef struct {
	int kq; // non owning
	pthread_mutex_t mutex;

	sigset_t sigs;
	KQueueEvent kqueue_event;
} SignalFDCtx;

errno_t signalfd_ctx_init(SignalFDCtx *signalfd, int kq, const sigset_t *sigs);
errno_t signalfd_ctx_terminate(SignalFDCtx *signalfd);

errno_t signalfd_ctx_read(SignalFDCtx *signalfd, uint32_t *ident);
void signalfd_ctx_poll(SignalFDCtx *signalfd, uint32_t *revents);

#endif
