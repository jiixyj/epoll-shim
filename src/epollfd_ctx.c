#include "epollfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>

#include <assert.h>

errno_t
epollfd_ctx_init(EpollFDCtx *epollfd, int kq)
{
	*epollfd = (EpollFDCtx){.kq = kq};

	errno_t ec;
	if ((ec = pthread_mutex_init(&epollfd->mutex, NULL)) != 0) {
		return ec;
	}

	return 0;
}

errno_t
epollfd_ctx_terminate(EpollFDCtx *epollfd)
{
	errno_t ec = 0;
	errno_t ec_local = 0;

	ec_local = pthread_mutex_destroy(&epollfd->mutex);
	ec = ec ? ec : ec_local;

	return ec;
}
