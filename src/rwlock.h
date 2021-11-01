#ifndef RWLOCK_H_
#define RWLOCK_H_

#include <stdbool.h>

#include <pthread.h>

typedef struct {
	int reader_count;
	bool has_writer;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} RWLock;

#define RWLOCK_INITIALIZER                          \
	{                                           \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.cond = PTHREAD_COND_INITIALIZER    \
	}

void rwlock_lock_read(RWLock *rwlock);
void rwlock_unlock_read(RWLock *rwlock);
void rwlock_lock_write(RWLock *rwlock);
void rwlock_unlock_write(RWLock *rwlock);
void rwlock_downgrade(RWLock *rwlock);

#endif
