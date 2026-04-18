#include "apennines/t1/sync/thread/thread.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#endif

#ifdef _WIN32

typedef struct {
    thread_fn fn;
    void *arg;
} thread_wrapper_ctx;

static DWORD WINAPI thread_wrapper_win32(LPVOID param) {
    thread_wrapper_ctx *ctx = (thread_wrapper_ctx *)param;
    thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    unsigned long ret;

    free(ctx);
    ret = fn(arg);
    return (DWORD)ret;
}

unsigned long thread_create(thread_handle *out, thread_fn fn, void *arg) {
    HANDLE h;
    thread_wrapper_ctx *ctx;

    if (!out) return 1;
    if (!fn) return 2;
    ctx = (thread_wrapper_ctx *)malloc(sizeof(thread_wrapper_ctx));
    if (!ctx) return 3;
    ctx->fn = fn;
    ctx->arg = arg;
    h = CreateThread(NULL, 0, thread_wrapper_win32, ctx, 0, NULL);
    if (!h) {
        free(ctx);
        return 4;
    }
    out->handle = h;
    return 0;
}

unsigned long thread_join(unsigned long *ret_val, thread_handle *t) {
    DWORD code;

    if (!ret_val) return 1;
    if (!t) return 2;
    if (!t->handle) return 3;
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) return 4;
    if (!GetExitCodeThread(t->handle, &code)) return 5;
    CloseHandle(t->handle);
    t->handle = NULL;
    *ret_val = (unsigned long)code;
    return 0;
}

unsigned long thread_detach(thread_handle *t) {
    if (!t) return 1;
    if (!t->handle) return 2;
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
}

unsigned long thread_yield(void) {
    SwitchToThread();
    return 0;
}

unsigned long thread_self(u64 *out) {
    if (!out) return 1;
    *out = (u64)GetCurrentThreadId();
    return 0;
}

unsigned long thread_set_name(const char *name) {
    if (!name) return 1;
    /* SetThreadDescription requires Windows 10 1607+; skip for portability */
    return 0;
}

unsigned long thread_sleep(u64 ns) {
    DWORD ms = (DWORD)(ns / 1000000ULL);
    if (ms == 0 && ns > 0) ms = 1;
    Sleep(ms);
    return 0;
}

#else /* POSIX */

typedef struct {
    thread_fn fn;
    void *arg;
} thread_wrapper_ctx;

static void *thread_wrapper_posix(void *param) {
    thread_wrapper_ctx *ctx = (thread_wrapper_ctx *)param;
    thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    unsigned long ret;

    free(ctx);
    ret = fn(arg);
    return (void *)(uintptr_t)ret;
}

unsigned long thread_create(thread_handle *out, thread_fn fn, void *arg) {
    pthread_t *pt;
    thread_wrapper_ctx *ctx;

    if (!out) return 1;
    if (!fn) return 2;
    pt = (pthread_t *)malloc(sizeof(pthread_t));
    if (!pt) return 3;
    ctx = (thread_wrapper_ctx *)malloc(sizeof(thread_wrapper_ctx));
    if (!ctx) {
        free(pt);
        return 4;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    if (pthread_create(pt, NULL, thread_wrapper_posix, ctx) != 0) {
        free(ctx);
        free(pt);
        return 5;
    }
    out->handle = pt;
    return 0;
}

unsigned long thread_join(unsigned long *ret_val, thread_handle *t) {
    void *retptr;
    pthread_t *pt;

    if (!ret_val) return 1;
    if (!t) return 2;
    if (!t->handle) return 3;
    pt = (pthread_t *)t->handle;
    if (pthread_join(*pt, &retptr) != 0) return 4;
    *ret_val = (unsigned long)(uintptr_t)retptr;
    free(pt);
    t->handle = NULL;
    return 0;
}

unsigned long thread_detach(thread_handle *t) {
    pthread_t *pt;

    if (!t) return 1;
    if (!t->handle) return 2;
    pt = (pthread_t *)t->handle;
    if (pthread_detach(*pt) != 0) return 3;
    free(pt);
    t->handle = NULL;
    return 0;
}

unsigned long thread_yield(void) {
    sched_yield();
    return 0;
}

unsigned long thread_self(u64 *out) {
    if (!out) return 1;
    *out = (u64)(uintptr_t)pthread_self();
    return 0;
}

unsigned long thread_set_name(const char *name) {
    if (!name) return 1;
#ifdef __APPLE__
    if (pthread_setname_np(name) != 0) return 2;
#elif defined(__linux__)
    if (pthread_setname_np(pthread_self(), name) != 0) return 2;
#endif
    return 0;
}

unsigned long thread_sleep(u64 ns) {
    struct timespec ts;

    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    if (nanosleep(&ts, NULL) == -1) return 1;
    return 0;
}

#endif /* _WIN32 */
