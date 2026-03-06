#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"
#include "cookbook_auth.h"

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

/* Load a 32-byte or 64-byte key from a hex string file.
   Returns 0 on success, -1 on error. */
static int load_hex_file(const char *path, unsigned char *out, size_t out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char hex[256];
    if (!fgets(hex, sizeof(hex), f)) { fclose(f); return -1; }
    fclose(f);

    /* strip trailing whitespace */
    size_t hlen = strlen(hex);
    while (hlen > 0 && (hex[hlen-1] == '\n' || hex[hlen-1] == '\r' ||
                         hex[hlen-1] == ' '))
        hex[--hlen] = '\0';

    if (hlen != out_len * 2) return -1;

    for (size_t i = 0; i < out_len; i++) {
        char byte_hex[3] = { hex[i*2], hex[i*2+1], '\0' };
        char *endp;
        unsigned long val = strtoul(byte_hex, &endp, 16);
        if (*endp != '\0') return -1;
        out[i] = (unsigned char)val;
    }
    return 0;
}

/* Save a key as hex to a file. */
static int save_hex_file(const char *path, const unsigned char *data,
                          size_t len) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t i = 0; i < len; i++)
        fprintf(f, "%02x", data[i]);
    fprintf(f, "\n");
    fclose(f);
    return 0;
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
    const char *max_mb_str  = env_or("COOKBOOK_MAX_ARTIFACT_MB", "0");
    int         max_upload_mb = atoi(max_mb_str);
    const char *pending_str = env_or("COOKBOOK_PENDING_TIMEOUT_SEC", "3600");
    int         pending_timeout = atoi(pending_str);
    const char *jwt_ttl_str = env_or("COOKBOOK_JWT_TTL_SEC", "3600");
    int         jwt_ttl = atoi(jwt_ttl_str);
    const char *rate_str    = env_or("COOKBOOK_RATE_LIMIT_PER_MIN", "0");
    int         rate_limit  = atoi(rate_str);
    const char *key_dir     = env_or("COOKBOOK_KEY_DIR", NULL);

    /* registry Ed25519 key pair */
    unsigned char registry_pk[32], registry_sk[64];
    int has_key = 0;

    if (key_dir) {
        char pk_path[512], sk_path[512];
        snprintf(pk_path, sizeof(pk_path), "%s/registry.pub", key_dir);
        snprintf(sk_path, sizeof(sk_path), "%s/registry.key", key_dir);

        if (load_hex_file(pk_path, registry_pk, 32) == 0 &&
            load_hex_file(sk_path, registry_sk, 64) == 0) {
            has_key = 1;
            printf("cookbook: loaded registry key from %s\n", key_dir);
        } else {
            /* generate a new key pair */
            printf("cookbook: generating new registry key pair in %s\n", key_dir);
            if (cookbook_keygen(registry_pk, registry_sk) == 0) {
                save_hex_file(pk_path, registry_pk, 32);
                save_hex_file(sk_path, registry_sk, 64);
                has_key = 1;
            } else {
                fprintf(stderr, "cookbook: warning: failed to generate key\n");
            }
        }
    }

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
        .listen_url          = listen_url,
        .registry_id         = registry_id,
        .db                  = db,
        .store               = store,
        .max_upload_mb       = max_upload_mb,
        .pending_timeout_sec = pending_timeout,
        .jwt_ttl_sec         = jwt_ttl,
        .rate_limit_per_min  = rate_limit,
        .registry_pk         = has_key ? registry_pk : NULL,
        .registry_sk         = has_key ? registry_sk : NULL
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
