#include <sys/eventfd.h>
#undef read
#undef write
#undef close

#include <sys/types.h>

#include <sys/capsicum.h>
#include <sys/filio.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <err.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#include <cuse.h>

#include "epoll-shimd.h"

struct eventfd_cuse_ctx {
	bool initialized;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int flags;
	uint64_t value;
};

static void
hup_catcher(int dummy)
{
	(void)dummy;
}

static sem_t nr_free_threads_sem;
static errno_t worker_spawn(void);

static int
eventfd_blocking_wait(struct eventfd_cuse_ctx *handle)
{
	errno_t ec;

	for (;;) {
		/*
		 * If we are entering a blocking syscall we may need to spawn
		 * a new thread to handle other requests.
		 */

		if (sem_trywait(&nr_free_threads_sem) == 0) {
			break;
		}

		if ((ec = worker_spawn()) != 0) {
			/*
			 * If the per-process thread limit is reached we are
			 * out of luck.
			 * The best thing we can do is to wait for another
			 * thread to become free again.
			 */

			if (sem_clockwait_np(&nr_free_threads_sem,
				CLOCK_MONOTONIC, 0,
				&(struct timespec){.tv_sec = 1}, NULL) == 0) {
				break;
			}
		}
	}

	(void)pthread_cond_wait(&handle->cond, &handle->mutex);

	if (sem_post(&nr_free_threads_sem) < 0) {
		return CUSE_ERR_OTHER;
	}

	if (cuse_got_peer_signal() == 0) {
		return CUSE_ERR_SIGNAL;
	}

	return 0;
}

static _Noreturn void
worker_thread_loop()
{
	int ec;

	for (;;) {
		ec = cuse_wait_and_process();

		if (ec != 0) {
			break;
		}
	}

	exit(0);
}

static void *
worker_thread(void *arg)
{
	pthread_barrier_t *barrier = arg;

	(void)pthread_barrier_wait(barrier);
	worker_thread_loop();

	/* unreachable */
	/* return NULL; */
}

static errno_t
worker_spawn()
{
	errno_t ec;

	pthread_barrier_t barrier;
	if ((ec = pthread_barrier_init(&barrier, NULL, 2)) != 0) {
		return ec;
	}

	pthread_t thread;
	if ((ec = pthread_create(&thread, NULL, /**/
		 worker_thread, &barrier)) != 0) {
		goto out;
	}

	(void)sem_post(&nr_free_threads_sem);
	(void)pthread_barrier_wait(&barrier);

	ec = 0;

out:
	(void)pthread_barrier_destroy(&barrier);
	return ec;
}

static errno_t
cond_init_shared(pthread_cond_t *cond)
{
	errno_t ec;
	pthread_condattr_t cond_attr;

	if ((ec = pthread_condattr_init(&cond_attr)) != 0) {
		return ec;
	}

	if ((ec = pthread_condattr_setpshared(&cond_attr,
		 PTHREAD_PROCESS_SHARED)) != 0) {
		goto out;
	}

	ec = pthread_cond_init(cond, &cond_attr);

out:
	(void)pthread_condattr_destroy(&cond_attr);
	return ec;
}

static int
eventfd_cuse_open(struct cuse_dev *cdev, int fflags)
{
	errno_t ec;

	if ((fflags & (CUSE_FFLAG_READ | CUSE_FFLAG_WRITE)) !=
	    (CUSE_FFLAG_READ | CUSE_FFLAG_WRITE)) {
		return CUSE_ERR_INVALID;
	}

	struct eventfd_cuse_ctx *handle =
	    malloc(sizeof(struct eventfd_cuse_ctx));
	if (!handle) {
		return CUSE_ERR_NO_MEMORY;
	}

	handle->initialized = false;
	if ((ec = pthread_mutex_init(&handle->mutex, NULL)) != 0) {
		free(handle);
		return CUSE_ERR_NO_MEMORY;
	}

	/*
	 * We must use a PTHREAD_PROCESS_SHARED condition variable
	 * because only those are interruptible by signals.
	 */
	if ((ec = cond_init_shared(&handle->cond)) != 0) {
		(void)pthread_mutex_destroy(&handle->mutex);
		free(handle);
		return CUSE_ERR_NO_MEMORY;
	}

	cuse_dev_set_per_file_handle(cdev, handle);
	return 0;
}

static int
eventfd_cuse_close(struct cuse_dev *cdev, int fflags)
{
	(void)fflags;

	struct eventfd_cuse_ctx *handle = cuse_dev_get_per_file_handle(cdev);
	cuse_dev_set_per_file_handle(cdev, NULL);

	(void)pthread_cond_destroy(&handle->cond);
	(void)pthread_mutex_destroy(&handle->mutex);
	free(handle);
	return 0;
}

static int
eventfd_cuse_read(struct cuse_dev *cdev, int fflags, void *peer_ptr, int len)
{
	int ec;
	struct eventfd_cuse_ctx *handle = cuse_dev_get_per_file_handle(cdev);

	if (len < (int)sizeof(uint64_t) || !handle->initialized) {
		return CUSE_ERR_INVALID;
	}

	(void)pthread_mutex_lock(&handle->mutex);

	while (handle->value == 0) {
		if (fflags & CUSE_FFLAG_NONBLOCK) {
			(void)pthread_mutex_unlock(&handle->mutex);
			return CUSE_ERR_WOULDBLOCK;
		} else {
			if ((ec = eventfd_blocking_wait(handle)) != 0) {
				(void)pthread_mutex_unlock(&handle->mutex);
				return ec;
			}
		}
	}

	if (handle->flags & EFD_SEMAPHORE) {
		uint64_t one = 1;
		ec = cuse_copy_out(&one, /**/
		    peer_ptr, (int)sizeof(uint64_t));
		if (ec == 0) {
			--handle->value;
		}
	} else {
		ec = cuse_copy_out(&handle->value, /**/
		    peer_ptr, (int)sizeof(uint64_t));
		if (ec == 0) {
			handle->value = 0;
		}
	}

	(void)pthread_mutex_unlock(&handle->mutex);

	cuse_poll_wakeup();
	(void)pthread_cond_broadcast(&handle->cond);

	return (int)sizeof(uint64_t);
}

static int
eventfd_cuse_write(struct cuse_dev *cdev, int fflags, void const *peer_ptr,
    int len)
{
	int ec;
	struct eventfd_cuse_ctx *handle = cuse_dev_get_per_file_handle(cdev);

	if (len < (int)sizeof(uint64_t) || !handle->initialized) {
		return CUSE_ERR_INVALID;
	}

	uint64_t new_value;
	ec = cuse_copy_in(peer_ptr, &new_value, sizeof(uint64_t));
	if (ec != 0) {
		return ec;
	}

	if (new_value == UINT64_MAX) {
		return CUSE_ERR_INVALID;
	}

	(void)pthread_mutex_lock(&handle->mutex);

	uint64_t new_value_result;
	while (__builtin_add_overflow(new_value, handle->value,
		   &new_value_result) ||
	    new_value_result == UINT64_MAX) {
		if (fflags & CUSE_FFLAG_NONBLOCK) {
			(void)pthread_mutex_unlock(&handle->mutex);
			return CUSE_ERR_WOULDBLOCK;
		} else {
			if ((ec = eventfd_blocking_wait(handle)) != 0) {
				(void)pthread_mutex_unlock(&handle->mutex);
				return ec;
			}
		}
	}
	handle->value = new_value_result;

	(void)pthread_mutex_unlock(&handle->mutex);

	cuse_poll_wakeup();
	(void)pthread_cond_broadcast(&handle->cond);

	return (int)sizeof(uint64_t);
}

static int
eventfd_cuse_ioctl(struct cuse_dev *cdev, int fflags, /**/
    unsigned long cmd, void *peer_data)
{
	int ec;

	(void)fflags;

	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		return 0;
	}

	if (cmd == EVENTFD_IOCTL_SETUP) {
		struct eventfd_setup setup;

		if ((ec = cuse_copy_in(peer_data, /**/
			 &setup, sizeof(setup))) != 0) {
			return ec;
		}

		if (setup.flags &
		    ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) {
			return CUSE_ERR_INVALID;
		}

		struct eventfd_cuse_ctx *handle =
		    cuse_dev_get_per_file_handle(cdev);

		(void)pthread_mutex_lock(&handle->mutex);
		if (handle->initialized) {
			(void)pthread_mutex_unlock(&handle->mutex);
			return CUSE_ERR_INVALID;
		}

		handle->flags = setup.flags;
		handle->value = setup.initval;
		handle->initialized = true;

		(void)pthread_mutex_unlock(&handle->mutex);

		return 0;
	}

	return CUSE_ERR_INVALID;
}

static int
eventfd_cuse_poll(struct cuse_dev *cdev, int fflags, int events)
{
	(void)fflags;

	int revents = 0;
	struct eventfd_cuse_ctx *handle = cuse_dev_get_per_file_handle(cdev);

	(void)pthread_mutex_lock(&handle->mutex);

	if (!handle->initialized) {
		revents = CUSE_POLL_ERROR;
	} else {
		if (handle->value != 0) {
			revents |= events & CUSE_POLL_READ;
		}

		if (handle->value < UINT64_MAX - 1) {
			revents |= events & CUSE_POLL_WRITE;
		}
	}

	(void)pthread_mutex_unlock(&handle->mutex);

	return revents;
}

static struct cuse_methods epoll_eventfd_methods = {
    .cm_open = eventfd_cuse_open,
    .cm_close = eventfd_cuse_close,
    .cm_read = eventfd_cuse_read,
    .cm_write = eventfd_cuse_write,
    .cm_ioctl = eventfd_cuse_ioctl,
    .cm_poll = eventfd_cuse_poll,
};

int
main()
{
	int ec;

	if (signal(SIGHUP, hup_catcher) == SIG_ERR) {
		err(1, "signal");
	}

	if ((ec = cuse_init()) < 0) {
		errx(1, "cuse_init: %d", ec);
	}

	if ((ec = sem_init(&nr_free_threads_sem, 0, 0)) < 0) {
		err(1, "sem_init");
	}

	struct cuse_dev *cdev = cuse_dev_create(&epoll_eventfd_methods, NULL,
	    NULL, 0, 0, 0666, "epoll_shim/eventfd");
	if (!cdev) {
		errx(1, "cuse_dev_create failed");
	}

	if (cap_enter() < 0) {
		err(1, "cap_enter");
	}

	worker_thread_loop();
}
