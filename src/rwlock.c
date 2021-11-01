#include "rwlock.h"

/**/

void
rwlock_init(RWLock *rwlock)
{
	*rwlock = (RWLock) {};
	(void)pthread_mutex_init(&rwlock->mutex, NULL);
	(void)pthread_cond_init(&rwlock->cond, NULL);
}

void
rwlock_lock_read(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	while (rwlock->has_writer) {
		(void)pthread_cond_wait(&rwlock->cond, &rwlock->mutex);
	}
	++rwlock->reader_count;
	(void)pthread_mutex_unlock(&rwlock->mutex);
}

void
rwlock_unlock_read(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	--rwlock->reader_count;
	if (rwlock->reader_count == 0) {
		(void)pthread_cond_broadcast(&rwlock->cond);
	}
	(void)pthread_mutex_unlock(&rwlock->mutex);
}

void
rwlock_lock_write(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	while (rwlock->has_writer) {
		(void)pthread_cond_wait(&rwlock->cond, &rwlock->mutex);
	}
	rwlock->has_writer = true;
	while (rwlock->reader_count > 0) {
		(void)pthread_cond_wait(&rwlock->cond, &rwlock->mutex);
	}
	(void)pthread_mutex_unlock(&rwlock->mutex);
}

void
rwlock_unlock_write(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	rwlock->has_writer = false;
	(void)pthread_cond_broadcast(&rwlock->cond);
	(void)pthread_mutex_unlock(&rwlock->mutex);
}

void
rwlock_downgrade(RWLock *rwlock)
{
	(void)pthread_mutex_lock(&rwlock->mutex);
	++rwlock->reader_count;
	rwlock->has_writer = false;
	(void)pthread_cond_broadcast(&rwlock->cond);
	(void)pthread_mutex_unlock(&rwlock->mutex);
}
