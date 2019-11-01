#ifndef EVENTFD_CTX_H_
#define EVENTFD_CTX_H_

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define EVENTFD_CTX_FLAG_SEMAPHORE (1 << 0)
#define EVENTFD_CTX_FLAG_NONBLOCK (1 << 1)

typedef struct {
	int kq_;
	int flags_;
	atomic_uint_least64_t counter_;
} EventFDCtx;

errno_t eventfd_ctx_init(EventFDCtx *eventfd, unsigned int counter, int flags);
errno_t eventfd_ctx_terminate(EventFDCtx *eventfd);

int eventfd_ctx_fd(EventFDCtx *eventfd);

errno_t eventfd_ctx_write(EventFDCtx *eventfd, uint64_t value);
errno_t eventfd_ctx_read(EventFDCtx *eventfd, uint64_t *value);

#endif
