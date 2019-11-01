#ifndef EVENTFD_CTX_H_
#define EVENTFD_CTX_H_

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int kq_;
	atomic_uint_least64_t counter_;
	bool is_semaphore_;
} EventFDCtx;

errno_t eventfd_ctx_init(EventFDCtx *eventfd, unsigned int counter,
    bool is_semaphore);
errno_t eventfd_ctx_terminate(EventFDCtx *eventfd);

int eventfd_ctx_fd(EventFDCtx *eventfd);

errno_t eventfd_ctx_write(EventFDCtx *eventfd, uint64_t value);
errno_t eventfd_ctx_read(EventFDCtx *eventfd, uint64_t *value);

#endif
