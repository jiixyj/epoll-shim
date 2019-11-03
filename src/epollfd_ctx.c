#include "epollfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>

#include <assert.h>

errno_t
epollfd_ctx_init(EpollFDCtx *epollfd, int kq)
{
	*epollfd = (EpollFDCtx){.kq = kq};

	return 0;
}

errno_t
epollfd_ctx_terminate(EpollFDCtx *epollfd)
{
	(void)epollfd;

	return 0;
}
