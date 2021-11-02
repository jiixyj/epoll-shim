#include "rwlock.h"

#include <errno.h>

/*
 * Inspired by:
 * <https://eli.thegreenplace.net/2019/implementing-reader-writer-locks/>
 */

#define MAX_READERS (1 << 30)

static int
sem_wait_nointr(sem_t *sem)
{
	int rc;

	do {
		rc = sem_wait(sem);
	} while (rc < 0 && errno == EINTR);

	return rc;
}

void
rwlock_init(RWLock *rwlock)
{
	*rwlock = (RWLock) {};
	(void)pthread_mutex_init(&rwlock->mutex, NULL);
	(void)sem_init(&rwlock->writer_wait, 0, 0);
	(void)sem_init(&rwlock->reader_wait, 0, 0);
}

void
rwlock_lock_read(RWLock *rwlock)
{
	if (atomic_fetch_add(&rwlock->num_pending, 1) + 1 < 0) {
		(void)sem_wait_nointr(&rwlock->reader_wait);
	}
}

void
rwlock_unlock_read(RWLock *rwlock)
{
	if (atomic_fetch_sub(&rwlock->num_pending, 1) - 1 < 0) {
		if (atomic_fetch_sub(&rwlock->readers_departing, 1) - 1 == 0) {
			(void)sem_post(&rwlock->writer_wait);
		}
	}
}

void
rwlock_lock_write(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	int_fast32_t r = atomic_fetch_sub(&rwlock->num_pending, MAX_READERS);
	if (r != 0 &&
	    atomic_fetch_add(&rwlock->readers_departing, r) + r != 0) {
		(void)sem_wait_nointr(&rwlock->writer_wait);
	}
}

static inline void
rwlock_unlock_common(RWLock *rwlock, int keep_reading)
{
	int_fast32_t r = atomic_fetch_add(&rwlock->num_pending,
			     MAX_READERS + keep_reading) +
	    MAX_READERS + keep_reading;
	for (int_fast32_t i = keep_reading; i < r; ++i) {
		(void)sem_post(&rwlock->reader_wait);
	}
	(void)pthread_mutex_unlock(&rwlock->mutex);
}

void
rwlock_unlock_write(RWLock *rwlock)
{
	rwlock_unlock_common(rwlock, 0);
}

void
rwlock_downgrade(RWLock *rwlock)
{
	rwlock_unlock_common(rwlock, 1);
}
