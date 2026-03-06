#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int s_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static const char *env_or(const char *name, const char *fallback) {
    const char *val = getenv(name);
    return (val && *val) ? val : fallback;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("cookbook %d.%d.%d\n",
           cookbook_version_major(),
           cookbook_version_minor(),
           cookbook_version_patch());

    /* configuration from environment */
    const char *port        = env_or("COOKBOOK_PORT", "8080");
    const char *registry_id = env_or("COOKBOOK_REGISTRY_ID", "central");
    const char *db_url      = env_or("COOKBOOK_DB_URL", NULL);
    const char *storage_dir = env_or("COOKBOOK_STORAGE_DIR", "./data/objects");

    /* database */
    cookbook_db *db;
    if (db_url && strstr(db_url, "postgres://")) {
        fprintf(stderr, "cookbook: PostgreSQL backend not yet implemented\n");
        return 1;
    } else {
        const char *sqlite_path = db_url ? db_url : "cookbook.db";
        db = cookbook_db_open_sqlite(sqlite_path);
        if (!db) {
            fprintf(stderr, "cookbook: failed to open database: %s\n", sqlite_path);
            return 1;
        }
        printf("cookbook: database: %s\n", sqlite_path);
    }

    /* run migrations */
    if (cookbook_db_migrate(db) != COOKBOOK_DB_OK) {
        fprintf(stderr, "cookbook: schema migration failed\n");
        db->close(db);
        return 1;
    }

    /* object store */
    cookbook_store *store = cookbook_store_open_fs(storage_dir);
    if (!store) {
        fprintf(stderr, "cookbook: failed to open storage: %s\n", storage_dir);
        db->close(db);
        return 1;
    }
    printf("cookbook: storage: %s\n", storage_dir);

    /* build listen URL */
    char listen_url[256];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%s", port);

    /* start server */
    cookbook_server_opts opts = {
        .listen_url  = listen_url,
        .registry_id = registry_id,
        .db          = db,
        .store       = store
    };

    cookbook_server *srv = cookbook_server_start(&opts);
    if (!srv) {
        store->close(store);
        db->close(db);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (s_running) {
        cookbook_server_poll(srv, 100);
    }

    printf("\ncookbook: shutting down\n");
    cookbook_server_stop(srv);
    store->close(store);
    db->close(db);
    return 0;
}
