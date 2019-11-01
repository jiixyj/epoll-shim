#include <stddef.h>

#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

struct signalfd_context;
struct timerfd_context;
struct eventfd_context;

extern pthread_mutex_t timerfd_context_mtx;
extern struct timerfd_context *get_timerfd_context(int fd, bool create_new);
extern ssize_t timerfd_read(struct timerfd_context *, void *buf,
    size_t nbytes);
extern int timerfd_close(struct timerfd_context *);

extern pthread_mutex_t signalfd_context_mtx;
extern struct signalfd_context *get_signalfd_context(int fd, bool create_new);
extern ssize_t signalfd_read(struct signalfd_context *, void *buf,
    size_t nbytes);
extern int signalfd_close(struct signalfd_context *);

extern pthread_mutex_t eventfd_context_mtx;
extern struct eventfd_context *get_eventfd_context(int fd, bool create_new);
extern ssize_t eventfd_helper_read(struct eventfd_context *, void *buf,
    size_t nbytes);
extern ssize_t eventfd_helper_write(struct eventfd_context *, void const *buf,
    size_t nbytes);
extern int eventfd_close(struct eventfd_context *);

#define WRAP(context, return_type, call, unlock_after_call)                   \
	if (fd >= 0) {                                                        \
		pthread_mutex_lock(&context##_mtx);                           \
		struct context *ctx = get_##context(fd, false);               \
		if (ctx) {                                                    \
			return_type ret = (call);                             \
			if (unlock_after_call) {                              \
				pthread_mutex_unlock(&context##_mtx);         \
			}                                                     \
			return ret;                                           \
		}                                                             \
		pthread_mutex_unlock(&context##_mtx);                         \
	}

int
epoll_shim_close(int fd)
{
	WRAP(timerfd_context, int, timerfd_close(ctx), true)
	WRAP(signalfd_context, int, signalfd_close(ctx), true)
	WRAP(eventfd_context, int, eventfd_close(ctx), true)

	return close(fd);
}

ssize_t
epoll_shim_read(int fd, void *buf, size_t nbytes)
{
	WRAP(timerfd_context, ssize_t, timerfd_read(ctx, buf, nbytes), false)
	WRAP(signalfd_context, ssize_t, signalfd_read(ctx, buf, nbytes), false)
	WRAP(eventfd_context, ssize_t, eventfd_helper_read(ctx, buf, nbytes),
	    false)

	return read(fd, buf, nbytes);
}

ssize_t
epoll_shim_write(int fd, void const *buf, size_t nbytes)
{
	WRAP(eventfd_context, ssize_t, eventfd_helper_write(ctx, buf, nbytes),
	    false)

	return write(fd, buf, nbytes);
}
