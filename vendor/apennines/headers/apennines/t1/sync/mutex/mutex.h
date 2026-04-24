#ifndef APENNINES_T1_MUTEX_H
#define APENNINES_T1_MUTEX_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef struct { void *handle; } mutex;
typedef struct { void *handle; } rwlock;
typedef struct { void *handle; } cond;
typedef struct { void *handle; } semaphore;

APENNINES_API unsigned long mutex_create(mutex *out);
APENNINES_API unsigned long mutex_lock(mutex *m);
APENNINES_API unsigned long mutex_trylock(unsigned long *success, mutex *m);
APENNINES_API unsigned long mutex_unlock(mutex *m);
APENNINES_API unsigned long mutex_destroy(mutex *m);

APENNINES_API unsigned long rwlock_create(rwlock *out);
APENNINES_API unsigned long rwlock_read_lock(rwlock *rw);
APENNINES_API unsigned long rwlock_read_unlock(rwlock *rw);
APENNINES_API unsigned long rwlock_write_lock(rwlock *rw);
APENNINES_API unsigned long rwlock_write_unlock(rwlock *rw);
APENNINES_API unsigned long rwlock_destroy(rwlock *rw);

APENNINES_API unsigned long cond_create(cond *out);
APENNINES_API unsigned long cond_wait(cond *c, mutex *m);
APENNINES_API unsigned long cond_timedwait(unsigned long *timed_out, cond *c, mutex *m, u64 timeout_ns);
APENNINES_API unsigned long cond_signal(cond *c);
APENNINES_API unsigned long cond_broadcast(cond *c);
APENNINES_API unsigned long cond_destroy(cond *c);

APENNINES_API unsigned long sem_create(semaphore *out, u32 initial);
APENNINES_API unsigned long sem_wait(semaphore *s);
APENNINES_API unsigned long sem_trywait(unsigned long *success, semaphore *s);
APENNINES_API unsigned long sem_post(semaphore *s);
APENNINES_API unsigned long sem_destroy(semaphore *s);

#endif /* APENNINES_T1_MUTEX_H */
