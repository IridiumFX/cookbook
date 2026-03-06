#include "cookbook_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define PATH_SEP '\\'
  #define mkdir_p(p) _mkdir(p)
  #define stat_func _stat
  #define stat_type struct _stat
  #define access_func _access
  #define F_OK_VAL 0
#else
  #include <unistd.h>
  #define PATH_SEP '/'
  #define mkdir_p(p) mkdir(p, 0755)
  #define stat_func stat
  #define stat_type struct stat
  #define access_func access
  #define F_OK_VAL F_OK
#endif

typedef struct {
    cookbook_store  base;
    char          *root;
} cookbook_store_fs;

static char *join_path(const char *root, const char *key) {
    size_t rlen = strlen(root);
    size_t klen = strlen(key);
    char *path = malloc(rlen + 1 + klen + 1);
    if (!path) return NULL;
    memcpy(path, root, rlen);
    path[rlen] = '/';
    memcpy(path + rlen + 1, key, klen);
    path[rlen + 1 + klen] = '\0';
    return path;
}

static int mkdirs(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return -1;

    /* find last slash to get parent directory */
    char *last = NULL;
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') last = p;
    }
    if (last) {
        *last = '\0';
        /* recursively create parent dirs */
        for (char *p = tmp; *p; p++) {
            if (*p == '/' || *p == '\\') {
                *p = '\0';
                if (strlen(tmp) > 0) mkdir_p(tmp);
                *p = '/';
            }
        }
        mkdir_p(tmp);
    }
    free(tmp);
    return 0;
}

static cookbook_store_status fs_put(cookbook_store *store, const char *key,
                                    const void *data, size_t len) {
    cookbook_store_fs *self = (cookbook_store_fs *)store;
    char *path = join_path(self->root, key);
    if (!path) return COOKBOOK_STORE_ERROR;

    mkdirs(path);

    FILE *f = fopen(path, "wb");
    free(path);
    if (!f) return COOKBOOK_STORE_ERROR;

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? COOKBOOK_STORE_OK : COOKBOOK_STORE_ERROR;
}

static cookbook_store_status fs_get(cookbook_store *store, const char *key,
                                    void **data, size_t *len) {
    cookbook_store_fs *self = (cookbook_store_fs *)store;
    char *path = join_path(self->root, key);
    if (!path) return COOKBOOK_STORE_ERROR;

    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return COOKBOOK_STORE_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) { fclose(f); return COOKBOOK_STORE_ERROR; }

    void *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return COOKBOOK_STORE_ERROR; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (nread != (size_t)size) { free(buf); return COOKBOOK_STORE_ERROR; }

    *data = buf;
    *len = (size_t)size;
    return COOKBOOK_STORE_OK;
}

static cookbook_store_status fs_exists(cookbook_store *store, const char *key) {
    cookbook_store_fs *self = (cookbook_store_fs *)store;
    char *path = join_path(self->root, key);
    if (!path) return COOKBOOK_STORE_ERROR;

    int rc = access_func(path, F_OK_VAL);
    free(path);
    return (rc == 0) ? COOKBOOK_STORE_OK : COOKBOOK_STORE_NOT_FOUND;
}

static cookbook_store_status fs_del(cookbook_store *store, const char *key) {
    cookbook_store_fs *self = (cookbook_store_fs *)store;
    char *path = join_path(self->root, key);
    if (!path) return COOKBOOK_STORE_ERROR;

    int rc = remove(path);
    free(path);
    return (rc == 0) ? COOKBOOK_STORE_OK : COOKBOOK_STORE_NOT_FOUND;
}

static void fs_free_buf(void *data) {
    free(data);
}

static void fs_close(cookbook_store *store) {
    cookbook_store_fs *self = (cookbook_store_fs *)store;
    free(self->root);
    free(self);
}

cookbook_store *cookbook_store_open_fs(const char *root_dir) {
    cookbook_store_fs *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    const char *dir = (root_dir && *root_dir) ? root_dir : "./data/objects";
    self->root = strdup(dir);
    if (!self->root) { free(self); return NULL; }

    /* ensure root directory exists */
    mkdirs(self->root);
    mkdir_p(self->root);

    self->base.put      = fs_put;
    self->base.get      = fs_get;
    self->base.exists   = fs_exists;
    self->base.del      = fs_del;
    self->base.free_buf = fs_free_buf;
    self->base.close    = fs_close;
    return &self->base;
}
