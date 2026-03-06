#ifndef COOKBOOK_STORE_H
#define COOKBOOK_STORE_H

#include "cookbook.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cookbook_store cookbook_store;

typedef enum {
    COOKBOOK_STORE_OK = 0,
    COOKBOOK_STORE_ERROR,
    COOKBOOK_STORE_NOT_FOUND,
    COOKBOOK_STORE_EXISTS
} cookbook_store_status;

struct cookbook_store {
    cookbook_store_status (*put)(cookbook_store *store, const char *key,
                                const void *data, size_t len);
    cookbook_store_status (*get)(cookbook_store *store, const char *key,
                                void **data, size_t *len);
    cookbook_store_status (*exists)(cookbook_store *store, const char *key);
    cookbook_store_status (*del)(cookbook_store *store, const char *key);
    void                 (*free_buf)(void *data);
    void                 (*close)(cookbook_store *store);
};

/* Filesystem backend — stores objects as files under root_dir.
   Object keys map directly to relative paths (slashes preserved).
   NULL or empty root_dir defaults to "./data/objects". */
COOKBOOK_API cookbook_store *cookbook_store_open_fs(const char *root_dir);

#endif /* COOKBOOK_STORE_H */
