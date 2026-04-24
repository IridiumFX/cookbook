#include "apennines/t1/sync/mutex/mutex.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <errno.h>

/*
 * POSIX semaphore.h declares sem_wait, sem_post, etc. which collide with
 * our exported sem_wait, sem_post names. We call through static wrappers
 * defined here, before our own function definitions shadow the POSIX names.
 */
#include <semaphore.h>
static int p_sem_init(sem_t *s, int psh, unsigned val) { return sem_init(s, psh, val); }
static int p_sem_wait(sem_t *s) { return sem_wait(s); }
static int p_sem_trywait(sem_t *s) { return sem_trywait(s); }
static int p_sem_post(sem_t *s) { return sem_post(s); }
static int p_sem_destroy(sem_t *s) { return sem_destroy(s); }
#endif

/* ---- mutex ---- */

unsigned long mutex_create(mutex *out) {
#ifdef _WIN32
    CRITICAL_SECTION *cs;

    if (!out) return 1;
    cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return 2;
    InitializeCriticalSection(cs);
    out->handle = cs;
#else
    pthread_mutex_t *mtx;

    if (!out) return 1;
    mtx = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!mtx) return 2;
    if (pthread_mutex_init(mtx, NULL) != 0) {
        free(mtx);
        return 2;
    }
    out->handle = mtx;
#endif
    return 0;
}

unsigned long mutex_lock(mutex *m) {
    if (!m) return 1;
    if (!m->handle) return 1;
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    if (pthread_mutex_lock((pthread_mutex_t *)m->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long mutex_trylock(unsigned long *success, mutex *m) {
    if (!success) return 1;
    if (!m) return 2;
    if (!m->handle) return 2;
#ifdef _WIN32
    *success = TryEnterCriticalSection((CRITICAL_SECTION *)m->handle) ? 1 : 0;
#else
    {
        int r = pthread_mutex_trylock((pthread_mutex_t *)m->handle);
        if (r == 0) {
            *success = 1;
        } else if (r == EBUSY) {
            *success = 0;
        } else {
            return 3;
        }
    }
#endif
    return 0;
}

unsigned long mutex_unlock(mutex *m) {
    if (!m) return 1;
    if (!m->handle) return 1;
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    if (pthread_mutex_unlock((pthread_mutex_t *)m->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long mutex_destroy(mutex *m) {
    if (!m) return 1;
    if (!m->handle) return 1;
#ifdef _WIN32
    DeleteCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_destroy((pthread_mutex_t *)m->handle);
#endif
    free(m->handle);
    m->handle = NULL;
    return 0;
}

/* ---- rwlock ---- */

unsigned long rwlock_create(rwlock *out) {
#ifdef _WIN32
    SRWLOCK *rw;

    if (!out) return 1;
    rw = (SRWLOCK *)malloc(sizeof(SRWLOCK));
    if (!rw) return 2;
    InitializeSRWLock(rw);
    out->handle = rw;
#else
    pthread_rwlock_t *rw;

    if (!out) return 1;
    rw = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (!rw) return 2;
    if (pthread_rwlock_init(rw, NULL) != 0) {
        free(rw);
        return 2;
    }
    out->handle = rw;
#endif
    return 0;
}

unsigned long rwlock_read_lock(rwlock *rw) {
    if (!rw) return 1;
    if (!rw->handle) return 1;
#ifdef _WIN32
    AcquireSRWLockShared((SRWLOCK *)rw->handle);
#else
    if (pthread_rwlock_rdlock((pthread_rwlock_t *)rw->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long rwlock_read_unlock(rwlock *rw) {
    if (!rw) return 1;
    if (!rw->handle) return 1;
#ifdef _WIN32
    ReleaseSRWLockShared((SRWLOCK *)rw->handle);
#else
    if (pthread_rwlock_unlock((pthread_rwlock_t *)rw->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long rwlock_write_lock(rwlock *rw) {
    if (!rw) return 1;
    if (!rw->handle) return 1;
#ifdef _WIN32
    AcquireSRWLockExclusive((SRWLOCK *)rw->handle);
#else
    if (pthread_rwlock_wrlock((pthread_rwlock_t *)rw->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long rwlock_write_unlock(rwlock *rw) {
    if (!rw) return 1;
    if (!rw->handle) return 1;
#ifdef _WIN32
    ReleaseSRWLockExclusive((SRWLOCK *)rw->handle);
#else
    if (pthread_rwlock_unlock((pthread_rwlock_t *)rw->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long rwlock_destroy(rwlock *rw) {
    if (!rw) return 1;
    if (!rw->handle) return 1;
#ifdef _WIN32
    /* SRWLock does not need destruction */
#else
    pthread_rwlock_destroy((pthread_rwlock_t *)rw->handle);
#endif
    free(rw->handle);
    rw->handle = NULL;
    return 0;
}

/* ---- cond ---- */

unsigned long cond_create(cond *out) {
#ifdef _WIN32
    CONDITION_VARIABLE *cv;

    if (!out) return 1;
    cv = (CONDITION_VARIABLE *)malloc(sizeof(CONDITION_VARIABLE));
    if (!cv) return 2;
    InitializeConditionVariable(cv);
    out->handle = cv;
#else
    pthread_cond_t *cv;

    if (!out) return 1;
    cv = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    if (!cv) return 2;
    if (pthread_cond_init(cv, NULL) != 0) {
        free(cv);
        return 2;
    }
    out->handle = cv;
#endif
    return 0;
}

unsigned long cond_wait(cond *c, mutex *m) {
    if (!c) return 1;
    if (!c->handle) return 1;
    if (!m) return 2;
    if (!m->handle) return 2;
#ifdef _WIN32
    if (!SleepConditionVariableCS((CONDITION_VARIABLE *)c->handle,
                                  (CRITICAL_SECTION *)m->handle, INFINITE)) return 3;
#else
    if (pthread_cond_wait((pthread_cond_t *)c->handle,
                          (pthread_mutex_t *)m->handle) != 0) return 3;
#endif
    return 0;
}

unsigned long cond_timedwait(unsigned long *timed_out, cond *c, mutex *m, u64 timeout_ns) {
    if (!timed_out) return 1;
    if (!c) return 2;
    if (!c->handle) return 2;
    if (!m) return 3;
    if (!m->handle) return 3;
#ifdef _WIN32
    {
        DWORD ms = (DWORD)(timeout_ns / 1000000ULL);
        if (ms == 0 && timeout_ns > 0) ms = 1;
        if (!SleepConditionVariableCS((CONDITION_VARIABLE *)c->handle,
                                      (CRITICAL_SECTION *)m->handle, ms)) {
            if (GetLastError() == ERROR_TIMEOUT) {
                *timed_out = 1;
                return 0;
            }
            return 4;
        }
        *timed_out = 0;
    }
#else
    {
        struct timespec ts;
        int r;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
        ts.tv_nsec += (long)(timeout_ns % 1000000000ULL);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        r = pthread_cond_timedwait((pthread_cond_t *)c->handle,
                                   (pthread_mutex_t *)m->handle, &ts);
        if (r == ETIMEDOUT) {
            *timed_out = 1;
            return 0;
        }
        if (r != 0) return 4;
        *timed_out = 0;
    }
#endif
    return 0;
}

unsigned long cond_signal(cond *c) {
    if (!c) return 1;
    if (!c->handle) return 1;
#ifdef _WIN32
    WakeConditionVariable((CONDITION_VARIABLE *)c->handle);
#else
    if (pthread_cond_signal((pthread_cond_t *)c->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long cond_broadcast(cond *c) {
    if (!c) return 1;
    if (!c->handle) return 1;
#ifdef _WIN32
    WakeAllConditionVariable((CONDITION_VARIABLE *)c->handle);
#else
    if (pthread_cond_broadcast((pthread_cond_t *)c->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long cond_destroy(cond *c) {
    if (!c) return 1;
    if (!c->handle) return 1;
#ifdef _WIN32
    /* CONDITION_VARIABLE does not need destruction */
#else
    pthread_cond_destroy((pthread_cond_t *)c->handle);
#endif
    free(c->handle);
    c->handle = NULL;
    return 0;
}

/* ---- semaphore ---- */

unsigned long sem_create(semaphore *out, u32 initial) {
#ifdef _WIN32
    HANDLE h;

    if (!out) return 1;
    h = CreateSemaphore(NULL, (LONG)initial, 0x7FFFFFFF, NULL);
    if (!h) return 2;
    out->handle = h;
#else
    sem_t *s;

    if (!out) return 1;
    s = (sem_t *)malloc(sizeof(sem_t));
    if (!s) return 2;
    if (p_sem_init(s, 0, initial) != 0) {
        free(s);
        return 2;
    }
    out->handle = s;
#endif
    return 0;
}

unsigned long sem_wait(semaphore *s) {
    if (!s) return 1;
    if (!s->handle) return 1;
#ifdef _WIN32
    if (WaitForSingleObject((HANDLE)s->handle, INFINITE) != WAIT_OBJECT_0) return 2;
#else
    if (p_sem_wait((sem_t *)s->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long sem_trywait(unsigned long *success, semaphore *s) {
    if (!success) return 1;
    if (!s) return 2;
    if (!s->handle) return 2;
#ifdef _WIN32
    {
        DWORD r = WaitForSingleObject((HANDLE)s->handle, 0);
        if (r == WAIT_OBJECT_0) {
            *success = 1;
        } else if (r == WAIT_TIMEOUT) {
            *success = 0;
        } else {
            return 3;
        }
    }
#else
    {
        int r = p_sem_trywait((sem_t *)s->handle);
        if (r == 0) {
            *success = 1;
        } else if (errno == EAGAIN) {
            *success = 0;
        } else {
            return 3;
        }
    }
#endif
    return 0;
}

unsigned long sem_post(semaphore *s) {
    if (!s) return 1;
    if (!s->handle) return 1;
#ifdef _WIN32
    if (!ReleaseSemaphore((HANDLE)s->handle, 1, NULL)) return 2;
#else
    if (p_sem_post((sem_t *)s->handle) != 0) return 2;
#endif
    return 0;
}

unsigned long sem_destroy(semaphore *s) {
    if (!s) return 1;
    if (!s->handle) return 1;
#ifdef _WIN32
    CloseHandle((HANDLE)s->handle);
#else
    p_sem_destroy((sem_t *)s->handle);
    free(s->handle);
#endif
    s->handle = NULL;
    return 0;
}
