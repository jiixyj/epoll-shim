#ifndef EPOLLFD_CTX_H_
#define EPOLLFD_CTX_H_

#include <errno.h>

typedef struct {
	int kq; // non owning
} EpollFDCtx;

errno_t epollfd_ctx_init(EpollFDCtx *signalfd, int kq);
errno_t epollfd_ctx_terminate(EpollFDCtx *signalfd);

#endif
