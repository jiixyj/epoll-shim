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

	*eventfd = (EventFDCtx) {
		.kq_ = kq,
		.flags_ = flags,
		.counter_ = counter,
	};

	if ((ec = pthread_mutex_init(&eventfd->mutex_, NULL)) != 0) {
		return ec;
	}

	struct kevent kevs[2];
	int kevs_length = 0;

	if ((ec = kqueue_event_init(&eventfd->kqueue_event_, /**/
		 kevs, &kevs_length, counter > 0)) != 0) {
		goto out2;
	}

	if (kevent(eventfd->kq_, kevs, kevs_length, NULL, 0, NULL) < 0) {
		ec = errno;
		goto out;
	}

	return (0);

out:
	(void)kqueue_event_terminate(&eventfd->kqueue_event_);
out2:
	pthread_mutex_destroy(&eventfd->mutex_);
	return (ec);
}

errno_t
eventfd_ctx_terminate(EventFDCtx *eventfd)
{
	errno_t ec = 0;
	errno_t ec_local;

	ec_local = kqueue_event_terminate(&eventfd->kqueue_event_);
	ec = ec != 0 ? ec : ec_local;
	ec_local = pthread_mutex_destroy(&eventfd->mutex_);
	ec = ec != 0 ? ec : ec_local;

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

	errno_t ec = kqueue_event_trigger(&eventfd->kqueue_event_,
	    eventfd->kq_);
	if (ec != 0) {
		return (ec);
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

	uint_least64_t new_value =			     /**/
	    (eventfd->flags_ & EVENTFD_CTX_FLAG_SEMAPHORE) ? /**/
		  current_value - 1 :
		  0;

	if (new_value == 0 &&
	    kqueue_event_is_triggered(&eventfd->kqueue_event_)) {
		kqueue_event_clear(&eventfd->kqueue_event_, eventfd->kq_);
	}

	eventfd->counter_ = new_value;

	*value =					     /**/
	    (eventfd->flags_ & EVENTFD_CTX_FLAG_SEMAPHORE) ? /**/
		  1 :
		  current_value;
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
