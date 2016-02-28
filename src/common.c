#include <sys/param.h>
#include <sys/types.h>

#include <unistd.h>

extern struct timerfd_context *get_timerfd_context(int fd);
extern ssize_t timerfd_read(
    struct timerfd_context *, void *buf, size_t nbytes);
extern int timerfd_close(struct timerfd_context *);

extern struct signalfd_context *get_signalfd_context(int fd);
extern ssize_t signalfd_read(
    struct signalfd_context *, void *buf, size_t nbytes);
extern int signalfd_close(struct signalfd_context *);

int
epoll_shim_close(int fd)
{
	{
		struct timerfd_context *ctx = get_timerfd_context(fd);
		if (ctx) {
			return timerfd_close(ctx);
		}
	}

	{
		struct signalfd_context *ctx = get_signalfd_context(fd);
		if (ctx) {
			return signalfd_close(ctx);
		}
	}

	return close(fd);
}

ssize_t
epoll_shim_read(int fd, void *buf, size_t nbytes)
{
	{
		struct timerfd_context *ctx = get_timerfd_context(fd);
		if (ctx) {
			return timerfd_read(ctx, buf, nbytes);
		}
	}

	{
		struct signalfd_context *ctx = get_signalfd_context(fd);
		if (ctx) {
			return signalfd_read(ctx, buf, nbytes);
		}
	}

	return read(fd, buf, nbytes);
}
