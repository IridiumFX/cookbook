#include "apennines/t3/async/threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ----------------------------------------------------------------
 *  internal types
 * ---------------------------------------------------------------- */

typedef struct task_node {
    threadpool_task_fn  fn;
    void               *arg;
    future             *fut;
    struct task_node   *next;
} task_node;

struct future_s {
#ifdef _WIN32
    CRITICAL_SECTION    lock;
    CONDITION_VARIABLE  cond;
#else
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
#endif
    int                 done;       /* 0 = pending, 1 = complete */
    unsigned long       task_rc;    /* return code from task fn  */
    void               *result;     /* result pointer from task  */
};

struct threadpool_s {
#ifdef _WIN32
    CRITICAL_SECTION    lock;
    CONDITION_VARIABLE  cond;
    HANDLE             *threads;
#else
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
    pthread_t          *threads;
#endif
    u32                 num_threads;
    int                 shutdown;       /* 0=running, 1=graceful, 2=immediate */
    int                 stopped;        /* 1 after all workers have joined    */
    task_node          *queue_head;
    task_node          *queue_tail;
    u32                 queue_size;
};

/* ----------------------------------------------------------------
 *  internal helpers
 * ---------------------------------------------------------------- */

static void pool_lock(threadpool *p) {
#ifdef _WIN32
    EnterCriticalSection(&p->lock);
#else
    pthread_mutex_lock(&p->lock);
#endif
}

static void pool_unlock(threadpool *p) {
#ifdef _WIN32
    LeaveCriticalSection(&p->lock);
#else
    pthread_mutex_unlock(&p->lock);
#endif
}

static void pool_signal_all(threadpool *p) {
#ifdef _WIN32
    WakeAllConditionVariable(&p->cond);
#else
    pthread_cond_broadcast(&p->cond);
#endif
}

static void pool_wait(threadpool *p) {
#ifdef _WIN32
    SleepConditionVariableCS(&p->cond, &p->lock, INFINITE);
#else
    pthread_cond_wait(&p->cond, &p->lock);
#endif
}

static task_node *dequeue(threadpool *p) {
    task_node *node = p->queue_head;
    if (!node) return NULL;
    p->queue_head = node->next;
    if (!p->queue_head) p->queue_tail = NULL;
    p->queue_size--;
    node->next = NULL;
    return node;
}

static void future_mark_done(future *f, unsigned long rc, void *result) {
#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    f->task_rc = rc;
    f->result  = result;
    f->done    = 1;
    WakeAllConditionVariable(&f->cond);
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    f->task_rc = rc;
    f->result  = result;
    f->done    = 1;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
#endif
}

/* ----------------------------------------------------------------
 *  worker thread entry point
 * ---------------------------------------------------------------- */

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID param) {
#else
static void *worker_entry(void *param) {
#endif
    threadpool *pool = (threadpool *)param;

    for (;;) {
        task_node *task;

        pool_lock(pool);

        while (pool->queue_size == 0 && pool->shutdown == 0) {
            pool_wait(pool);
        }

        /* immediate shutdown: drop out right away */
        if (pool->shutdown == 2) {
            pool_unlock(pool);
            break;
        }

        /* graceful shutdown with empty queue: done */
        if (pool->shutdown == 1 && pool->queue_size == 0) {
            pool_unlock(pool);
            break;
        }

        task = dequeue(pool);
        pool_unlock(pool);

        if (task) {
            void          *res = NULL;
            unsigned long  rc  = task->fn(task->arg, &res);
            /* task->fut is NULL for detached submits — skip mark_done */
            if (task->fut) {
                future_mark_done(task->fut, rc, res);
            }
            free(task);
        }
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ----------------------------------------------------------------
 *  threadpool_create
 * ---------------------------------------------------------------- */

unsigned long threadpool_create(threadpool **out, u32 num_threads) {
    threadpool *p;
    u32 i;

    if (!out) return 1;
    if (num_threads == 0) return 2;

    p = (threadpool *)calloc(1, sizeof(threadpool));
    if (!p) return 3;

#ifdef _WIN32
    InitializeCriticalSection(&p->lock);
    InitializeConditionVariable(&p->cond);

    p->threads = (HANDLE *)calloc(num_threads, sizeof(HANDLE));
    if (!p->threads) {
        DeleteCriticalSection(&p->lock);
        free(p);
        return 3;
    }
#else
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        free(p);
        return 4;
    }
    if (pthread_cond_init(&p->cond, NULL) != 0) {
        pthread_mutex_destroy(&p->lock);
        free(p);
        return 4;
    }

    p->threads = (pthread_t *)calloc(num_threads, sizeof(pthread_t));
    if (!p->threads) {
        pthread_cond_destroy(&p->cond);
        pthread_mutex_destroy(&p->lock);
        free(p);
        return 3;
    }
#endif

    p->num_threads = num_threads;
    p->shutdown    = 0;
    p->stopped     = 0;
    p->queue_head  = NULL;
    p->queue_tail  = NULL;
    p->queue_size  = 0;

    for (i = 0; i < num_threads; i++) {
#ifdef _WIN32
        p->threads[i] = CreateThread(NULL, 0, worker_entry, p, 0, NULL);
        if (!p->threads[i]) {
            /* shut down already-created threads */
            pool_lock(p);
            p->shutdown = 2;
            pool_signal_all(p);
            pool_unlock(p);
            for (u32 j = 0; j < i; j++) {
                WaitForSingleObject(p->threads[j], INFINITE);
                CloseHandle(p->threads[j]);
            }
            DeleteCriticalSection(&p->lock);
            free(p->threads);
            free(p);
            return 5;
        }
#else
        if (pthread_create(&p->threads[i], NULL, worker_entry, p) != 0) {
            /* shut down already-created threads */
            pool_lock(p);
            p->shutdown = 2;
            pool_signal_all(p);
            pool_unlock(p);
            for (u32 j = 0; j < i; j++) {
                pthread_join(p->threads[j], NULL);
            }
            pthread_cond_destroy(&p->cond);
            pthread_mutex_destroy(&p->lock);
            free(p->threads);
            free(p);
            return 5;
        }
#endif
    }

    *out = p;
    return 0;
}

/* ----------------------------------------------------------------
 *  threadpool_submit
 * ---------------------------------------------------------------- */

unsigned long threadpool_submit(future **out_future, threadpool *pool,
                                threadpool_task_fn fn, void *arg) {
    future    *f;
    task_node *node;

    if (!out_future) return 1;
    if (!pool)       return 2;
    if (!fn)         return 3;

    pool_lock(pool);
    if (pool->shutdown != 0) {
        pool_unlock(pool);
        return 4;
    }
    pool_unlock(pool);

    /* allocate future */
    f = (future *)calloc(1, sizeof(future));
    if (!f) return 5;

#ifdef _WIN32
    InitializeCriticalSection(&f->lock);
    InitializeConditionVariable(&f->cond);
#else
    if (pthread_mutex_init(&f->lock, NULL) != 0) {
        free(f);
        return 5;
    }
    if (pthread_cond_init(&f->cond, NULL) != 0) {
        pthread_mutex_destroy(&f->lock);
        free(f);
        return 5;
    }
#endif
    f->done    = 0;
    f->task_rc = 0;
    f->result  = NULL;

    /* allocate task node */
    node = (task_node *)malloc(sizeof(task_node));
    if (!node) {
#ifdef _WIN32
        DeleteCriticalSection(&f->lock);
#else
        pthread_cond_destroy(&f->cond);
        pthread_mutex_destroy(&f->lock);
#endif
        free(f);
        return 5;
    }
    node->fn   = fn;
    node->arg  = arg;
    node->fut  = f;
    node->next = NULL;

    /* enqueue */
    pool_lock(pool);
    if (pool->shutdown != 0) {
        pool_unlock(pool);
#ifdef _WIN32
        DeleteCriticalSection(&f->lock);
#else
        pthread_cond_destroy(&f->cond);
        pthread_mutex_destroy(&f->lock);
#endif
        free(f);
        free(node);
        return 4;
    }

    if (pool->queue_tail) {
        pool->queue_tail->next = node;
    } else {
        pool->queue_head = node;
    }
    pool->queue_tail = node;
    pool->queue_size++;

    /* wake one worker */
#ifdef _WIN32
    WakeConditionVariable(&pool->cond);
#else
    pthread_cond_signal(&pool->cond);
#endif

    pool_unlock(pool);

    *out_future = f;
    return 0;
}

/* ----------------------------------------------------------------
 *  threadpool_submit_detached
 *
 *  Enqueue a task without a future. Caller gets no handle back and
 *  can't wait for completion. Safe for fire-and-forget work (e.g. the
 *  http_server accept loop handing connections to the pool) where
 *  calling future_destroy early would race with the worker's
 *  future_mark_done → use-after-free.
 * ---------------------------------------------------------------- */

unsigned long threadpool_submit_detached(threadpool *pool,
                                          threadpool_task_fn fn,
                                          void *arg) {
    task_node *node;

    if (!pool) return 1;
    if (!fn)   return 2;

    pool_lock(pool);
    if (pool->shutdown != 0) {
        pool_unlock(pool);
        return 3;
    }
    pool_unlock(pool);

    node = (task_node *)malloc(sizeof(task_node));
    if (!node) return 4;

    node->fn   = fn;
    node->arg  = arg;
    node->fut  = NULL;           /* detached — no future */
    node->next = NULL;

    pool_lock(pool);
    if (pool->shutdown != 0) {
        pool_unlock(pool);
        free(node);
        return 3;
    }

    if (pool->queue_tail) {
        pool->queue_tail->next = node;
    } else {
        pool->queue_head = node;
    }
    pool->queue_tail = node;
    pool->queue_size++;

#ifdef _WIN32
    WakeConditionVariable(&pool->cond);
#else
    pthread_cond_signal(&pool->cond);
#endif

    pool_unlock(pool);
    return 0;
}

/* ----------------------------------------------------------------
 *  shutdown helpers
 * ---------------------------------------------------------------- */

static unsigned long join_workers(threadpool *pool) {
    u32 i;
    unsigned long rc = 0;

    for (i = 0; i < pool->num_threads; i++) {
#ifdef _WIN32
        if (WaitForSingleObject(pool->threads[i], INFINITE) != WAIT_OBJECT_0)
            rc = 2;
        CloseHandle(pool->threads[i]);
#else
        if (pthread_join(pool->threads[i], NULL) != 0)
            rc = 2;
#endif
    }
    pool->stopped = 1;
    return rc;
}

/* drain pending tasks (mark their futures as cancelled) */
static void drain_queue(threadpool *pool) {
    task_node *node;

    while ((node = dequeue(pool)) != NULL) {
        /* Mark the future done with a non-zero rc so waiters unblock.
         * We use rc=1 to signal cancellation. Detached tasks have no
         * future — just drop them. */
        if (node->fut) {
            future_mark_done(node->fut, 1, NULL);
        }
        free(node);
    }
}

/* ----------------------------------------------------------------
 *  threadpool_shutdown  (graceful)
 * ---------------------------------------------------------------- */

unsigned long threadpool_shutdown(threadpool *pool) {
    unsigned long rc;

    if (!pool) return 1;

    pool_lock(pool);
    if (pool->shutdown != 0) {
        pool_unlock(pool);
        return 0; /* already shutting down / shut down */
    }
    pool->shutdown = 1;
    pool_signal_all(pool);
    pool_unlock(pool);

    rc = join_workers(pool);
    return rc;
}

/* ----------------------------------------------------------------
 *  threadpool_shutdown_now  (immediate)
 * ---------------------------------------------------------------- */

unsigned long threadpool_shutdown_now(threadpool *pool) {
    unsigned long rc;

    if (!pool) return 1;

    pool_lock(pool);
    pool->shutdown = 2;
    drain_queue(pool);
    pool_signal_all(pool);
    pool_unlock(pool);

    rc = join_workers(pool);
    return rc;
}

/* ----------------------------------------------------------------
 *  threadpool_destroy
 * ---------------------------------------------------------------- */

unsigned long threadpool_destroy(threadpool *pool) {
    if (!pool) return 1;
    if (!pool->stopped) return 2;

#ifdef _WIN32
    DeleteCriticalSection(&pool->lock);
#else
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->lock);
#endif
    free(pool->threads);
    free(pool);
    return 0;
}

/* ----------------------------------------------------------------
 *  future_wait
 * ---------------------------------------------------------------- */

unsigned long future_wait(void **out_result, future *f) {
    if (!out_result) return 1;
    if (!f)          return 2;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    while (!f->done)
        SleepConditionVariableCS(&f->cond, &f->lock, INFINITE);
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    while (!f->done)
        pthread_cond_wait(&f->cond, &f->lock);
    pthread_mutex_unlock(&f->lock);
#endif

    *out_result = f->result;

    if (f->task_rc != 0) {
        *out_result = (void *)(uintptr_t)f->task_rc;
        return 4;
    }
    return 0;
}

/* ----------------------------------------------------------------
 *  future_is_done
 * ---------------------------------------------------------------- */

unsigned long future_is_done(unsigned long *out_done, future *f) {
    if (!out_done) return 1;
    if (!f)        return 2;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    *out_done = f->done ? 1 : 0;
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    *out_done = f->done ? 1 : 0;
    pthread_mutex_unlock(&f->lock);
#endif
    return 0;
}

/* ----------------------------------------------------------------
 *  future_destroy
 * ---------------------------------------------------------------- */

unsigned long future_destroy(future *f) {
    if (!f) return 1;

#ifdef _WIN32
    DeleteCriticalSection(&f->lock);
#else
    pthread_cond_destroy(&f->cond);
    pthread_mutex_destroy(&f->lock);
#endif
    free(f);
    return 0;
}
