#ifndef EPOLLFD_CTX_H_
#define EPOLLFD_CTX_H_

#include <errno.h>

#include <pthread.h>

typedef struct {
	int kq; // non owning
	pthread_mutex_t mutex;
} EpollFDCtx;

errno_t epollfd_ctx_init(EpollFDCtx *signalfd, int kq);
errno_t epollfd_ctx_terminate(EpollFDCtx *signalfd);

#endif
