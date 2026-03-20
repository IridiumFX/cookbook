#ifndef APENNINES_T3_THREADPOOL_H
#define APENNINES_T3_THREADPOOL_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ----------------------------------------------------------------
 *  types
 * ---------------------------------------------------------------- */

typedef struct threadpool_s  threadpool;
typedef struct future_s      future;

/*
 * Task function signature.
 * Returns 0 on success (non-zero = error hatch).
 * May optionally set *result to a pointer the caller retrieves via
 * future_wait.
 */
typedef unsigned long (*threadpool_task_fn)(void *arg, void **result);

/* ----------------------------------------------------------------
 *  threadpool lifetime
 * ---------------------------------------------------------------- */

/*
 * threadpool_create - create a pool with `num_threads` worker threads.
 *
 * Hatches:
 *   1 - out is NULL
 *   2 - num_threads is 0
 *   3 - allocation failure
 *   4 - mutex/cond init failure
 *   5 - thread creation failure
 */
APENNINES_API unsigned long threadpool_create(threadpool **out, u32 num_threads);

/*
 * threadpool_submit - submit a task (fn + arg) and receive a future handle.
 *
 * Hatches:
 *   1 - out_future is NULL
 *   2 - pool is NULL
 *   3 - fn is NULL
 *   4 - pool is shut down
 *   5 - allocation failure
 */
APENNINES_API unsigned long threadpool_submit(future **out_future, threadpool *pool,
                                              threadpool_task_fn fn, void *arg);

/*
 * threadpool_shutdown - graceful shutdown; finish pending tasks, then stop.
 *
 * Hatches:
 *   1 - pool is NULL
 *   2 - join failure
 */
APENNINES_API unsigned long threadpool_shutdown(threadpool *pool);

/*
 * threadpool_shutdown_now - immediate shutdown; cancel pending, interrupt workers.
 *
 * Hatches:
 *   1 - pool is NULL
 *   2 - join failure
 */
APENNINES_API unsigned long threadpool_shutdown_now(threadpool *pool);

/*
 * threadpool_destroy - free all pool resources.
 * Pool must have been shut down first.
 *
 * Hatches:
 *   1 - pool is NULL
 *   2 - pool not shut down
 */
APENNINES_API unsigned long threadpool_destroy(threadpool *pool);

/* ----------------------------------------------------------------
 *  future
 * ---------------------------------------------------------------- */

/*
 * future_wait - block until the task completes, output the task result.
 *
 * Hatches:
 *   1 - out_result is NULL
 *   2 - f is NULL
 *   3 - wait failure
 *   4 - task returned non-zero (task error code stored in *out_result as
 *       (void*)(uintptr_t)task_rc)
 */
APENNINES_API unsigned long future_wait(void **out_result, future *f);

/*
 * future_is_done - non-blocking check if the task has completed.
 *
 * Hatches:
 *   1 - out_done is NULL
 *   2 - f is NULL
 */
APENNINES_API unsigned long future_is_done(unsigned long *out_done, future *f);

/*
 * future_destroy - free the future handle.
 *
 * Hatches:
 *   1 - f is NULL
 */
APENNINES_API unsigned long future_destroy(future *f);

#endif /* APENNINES_T3_THREADPOOL_H */
