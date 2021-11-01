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

void rwlock_init(RWLock *rwlock);
void rwlock_lock_read(RWLock *rwlock);
void rwlock_unlock_read(RWLock *rwlock);
void rwlock_lock_write(RWLock *rwlock);
void rwlock_unlock_write(RWLock *rwlock);
void rwlock_downgrade(RWLock *rwlock);

#endif
