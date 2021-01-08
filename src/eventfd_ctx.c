#include "eventfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>

#include <assert.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

_Static_assert(sizeof(unsigned int) < sizeof(uint64_t), "");

errno_t
eventfd_ctx_init(EventFDCtx *eventfd, int kq, unsigned int counter, int flags)
{
	errno_t ec;

	assert((flags & ~(EVENTFD_CTX_FLAG_SEMAPHORE)) == 0);

	*eventfd = (EventFDCtx){
	    .kq_ = kq,
	    .flags_ = flags,
	    .counter_ = counter,
	};

	if ((ec = pthread_mutex_init(&eventfd->mutex_, NULL)) != 0) {
		return ec;
	}

	struct kevent kevs[2];
	int kevs_length = 0;

#ifdef EVFILT_USER
	EV_SET(&kevs[kevs_length++], /**/
	    0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);

	if (counter > 0) {
		EV_SET(&kevs[kevs_length++], /**/
		    0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
	}
#else
	if (pipe2(eventfd->self_pipe_, O_NONBLOCK | O_CLOEXEC) < 0) {
		ec = errno;
		goto out2;
	}

	EV_SET(&kevs[kevs_length++], /**/
	    eventfd->self_pipe_[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (counter > 0) {
		char c = 0;
		if (write(eventfd->self_pipe_[1], &c, 1) < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				ec = errno;
				goto out;
			}
		}
	}
#endif

	if (kevent(eventfd->kq_, kevs, kevs_length, NULL, 0, NULL) < 0) {
		ec = errno;
		goto out;
	}

	eventfd->is_signalled_ = !!(counter > 0);

	return (0);

out:
#ifndef EVFILT_USER
	(void)close(eventfd->self_pipe_[0]);
	(void)close(eventfd->self_pipe_[1]);
out2:
#endif
	pthread_mutex_destroy(&eventfd->mutex_);
	return (ec);
}

errno_t
eventfd_ctx_terminate(EventFDCtx *eventfd)
{
	errno_t ec = pthread_mutex_destroy(&eventfd->mutex_);
#ifndef EVFILT_USER
	if (close(eventfd->self_pipe_[0]) < 0) {
		ec = ec != 0 ? ec : errno;
	}
	if (close(eventfd->self_pipe_[1]) < 0) {
		ec = ec != 0 ? ec : errno;
	}
#endif
	return (ec);
}

static errno_t
eventfd_ctx_write_impl(EventFDCtx *eventfd, uint64_t value)
{
	if (value == UINT64_MAX) {
		return (EINVAL);
	}

	uint_least64_t current_value = eventfd->counter_;

	uint_least64_t new_value;
	if (__builtin_add_overflow(current_value, value, &new_value) ||
	    new_value > UINT64_MAX - 1) {
		return (EAGAIN);
	}

	eventfd->counter_ = new_value;

	if (!eventfd->is_signalled_) {
#ifdef EVFILT_USER
		struct kevent kevs[1];
		EV_SET(&kevs[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);

		if (kevent(eventfd->kq_, kevs, nitems(kevs), /**/
			NULL, 0, NULL) < 0) {
			return (errno);
		}
#else
		char c = 0;
		if (write(eventfd->self_pipe_[1], &c, 1) < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				return (errno);
			}
		}
#endif
		eventfd->is_signalled_ = true;
	}

	return (0);
}

errno_t
eventfd_ctx_write(EventFDCtx *eventfd, uint64_t value)
{
	errno_t ec;

	(void)pthread_mutex_lock(&eventfd->mutex_);
	ec = eventfd_ctx_write_impl(eventfd, value);
	(void)pthread_mutex_unlock(&eventfd->mutex_);

	return ec;
}

static errno_t
eventfd_ctx_read_impl(EventFDCtx *eventfd, uint64_t *value)
{
	uint_least64_t current_value;

	current_value = eventfd->counter_;
	if (current_value == 0) {
		return (EAGAIN);
	}

	uint_least64_t new_value = (eventfd->flags_ &
				       EVENTFD_CTX_FLAG_SEMAPHORE)
	    ? current_value - 1
	    : 0;

	if (new_value == 0 && eventfd->is_signalled_) {
		struct kevent kevs[32];
		int n;

#ifndef EVFILT_USER
		char c[32];
		while (read(eventfd->self_pipe_[0], c, sizeof(c)) >= 0) {
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			return (errno);
		}
#endif

		while ((n = kevent(eventfd->kq_, NULL, 0, kevs, nitems(kevs),
			    &(struct timespec){0, 0})) > 0) {
		}
		if (n < 0) {
			return (errno);
		}

		eventfd->is_signalled_ = false;
	}

	eventfd->counter_ = new_value;

	*value = (eventfd->flags_ & EVENTFD_CTX_FLAG_SEMAPHORE)
	    ? 1
	    : current_value;
	return (0);
}

errno_t
eventfd_ctx_read(EventFDCtx *eventfd, uint64_t *value)
{
	errno_t ec;

	(void)pthread_mutex_lock(&eventfd->mutex_);
	ec = eventfd_ctx_read_impl(eventfd, value);
	(void)pthread_mutex_unlock(&eventfd->mutex_);

	return ec;
}
