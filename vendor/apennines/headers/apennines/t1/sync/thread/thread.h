#ifndef APENNINES_T1_THREAD_H
#define APENNINES_T1_THREAD_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef unsigned long (*thread_fn)(void *arg);

typedef struct {
    void *handle; /* opaque: pthread_t* or HANDLE */
} thread_handle;

APENNINES_API unsigned long thread_create(thread_handle *out, thread_fn fn, void *arg);
APENNINES_API unsigned long thread_join(unsigned long *ret_val, thread_handle *t);
APENNINES_API unsigned long thread_detach(thread_handle *t);
APENNINES_API unsigned long thread_yield(void);
APENNINES_API unsigned long thread_self(u64 *out);
APENNINES_API unsigned long thread_set_name(const char *name);
APENNINES_API unsigned long thread_sleep(u64 ns);

#endif /* APENNINES_T1_THREAD_H */
