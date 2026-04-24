#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"
#include "cookbook_auth.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile int s_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    s_running = 0;
}

/* Windows unhandled exception filter — catches SEH exceptions that
   bypass Unix signal handlers (access violations, stack overflows, etc.) */
#ifdef _WIN32
#include <windows.h>
static LONG WINAPI win_exception_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *name = "unknown";
    if (code == EXCEPTION_ACCESS_VIOLATION) name = "ACCESS_VIOLATION";
    else if (code == EXCEPTION_STACK_OVERFLOW) name = "STACK_OVERFLOW";
    else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) name = "INT_DIVIDE_BY_ZERO";
    else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) name = "ILLEGAL_INSTRUCTION";

    FILE *f = fopen("crash.log", "a");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "--- CRASH (Windows SEH) at %s", ctime(&now));
        fprintf(f, "Exception: 0x%08lX (%s)\n", (unsigned long)code, name);
        fprintf(f, "Address: 0x%p\n", ep->ExceptionRecord->ExceptionAddress);
        fprintf(f, "Check the audit logs for the last request before this crash.\n\n");
        fflush(f);
        fclose(f);
    }
    fprintf(stderr, "\ncookbook: FATAL: Windows exception 0x%08lX (%s)\n",
            (unsigned long)code, name);
    fprintf(stderr, "cookbook: crash log written to crash.log\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* Crash handler — writes minimal info before dying */
static void crash_handler(int sig) {
    /* use raw write to avoid malloc/stdio in signal context */
    const char *name = "unknown";
    if (sig == SIGSEGV) name = "SIGSEGV (segmentation fault)";
    else if (sig == SIGABRT) name = "SIGABRT (abort)";
    else if (sig == SIGFPE)  name = "SIGFPE (arithmetic error)";
#ifdef SIGBUS
    else if (sig == SIGBUS)  name = "SIGBUS (bus error)";
#endif

    FILE *f = fopen("crash.log", "a");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "--- CRASH at %s", ctime(&now));
        fprintf(f, "Signal: %d (%s)\n", sig, name);
        fprintf(f, "This usually indicates a bug in cookbook or a corrupt request.\n");
        fprintf(f, "Check the audit logs for the last request before this crash.\n\n");
        fflush(f);
        fclose(f);
    }

    /* also print to stderr */
    fprintf(stderr, "\ncookbook: FATAL: %s (signal %d)\n", name, sig);
    fprintf(stderr, "cookbook: crash log written to crash.log\n");

    /* re-raise to get default behavior (core dump if enabled) */
    signal(sig, SIG_DFL);
    raise(sig);
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

    /* write startup log for crash diagnosis */
    {
        FILE *sl = fopen("startup.log", "w");
        if (sl) {
            time_t now = time(NULL);
            fprintf(sl, "cookbook starting at %s", ctime(&now));
            fprintf(sl, "exe: %s\n", argv[0] ? argv[0] : "(unknown)");
            fflush(sl);
            fclose(sl);
        }
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(win_exception_handler);
#endif

    printf("cookbook %d.%d.%d\n",
           cookbook_version_major(),
           cookbook_version_minor(),
           cookbook_version_patch());

    /* configuration from environment */
    const char *port        = env_or("COOKBOOK_PORT", "8080");
    const char *registry_id = env_or("COOKBOOK_REGISTRY_ID", "central");
    const char *db_url      = env_or("COOKBOOK_DB_URL", NULL);
    const char *storage_dir = env_or("COOKBOOK_STORAGE_DIR", "./data/objects");
    const char *storage_type = env_or("COOKBOOK_STORAGE_TYPE", "fs");
    const char *s3_bucket   = env_or("COOKBOOK_S3_BUCKET", NULL);
    const char *s3_region   = env_or("COOKBOOK_S3_REGION", "us-east-1");
    const char *s3_access   = env_or("COOKBOOK_S3_ACCESS_KEY", NULL);
    const char *s3_secret   = env_or("COOKBOOK_S3_SECRET_KEY", NULL);
    const char *s3_endpoint = env_or("COOKBOOK_S3_ENDPOINT", NULL);
    const char *max_mb_str  = env_or("COOKBOOK_MAX_ARTIFACT_MB", "0");
    int         max_upload_mb = atoi(max_mb_str);
    const char *pending_str = env_or("COOKBOOK_PENDING_TIMEOUT_SEC", "3600");
    int         pending_timeout = atoi(pending_str);
    const char *jwt_ttl_str = env_or("COOKBOOK_JWT_TTL_SEC", "3600");
    int         jwt_ttl = atoi(jwt_ttl_str);
    const char *rate_str    = env_or("COOKBOOK_RATE_LIMIT_PER_MIN", "0");
    int         rate_limit  = atoi(rate_str);
    const char *key_dir     = env_or("COOKBOOK_KEY_DIR", NULL);
    const char *grid_str    = env_or("COOKBOOK_GRID_ENABLED", "0");
    int         grid_enabled = atoi(grid_str);
    const char *grid_hops_str = env_or("COOKBOOK_GRID_MAX_HOPS", "3");
    int         grid_max_hops = atoi(grid_hops_str);
    const char *grid_auth_str = env_or("COOKBOOK_GRID_PEER_AUTH", "0");
    int         grid_peer_auth = atoi(grid_auth_str);
    const char *audit_dir    = env_or("COOKBOOK_AUDIT_DIR", NULL);
    const char *obj_ttl_str = env_or("COOKBOOK_OBJECT_CACHE_TTL_SEC", "0");
    int         obj_cache_ttl = atoi(obj_ttl_str);
    const char *ldap_url     = env_or("COOKBOOK_LDAP_URL", NULL);
    const char *ldap_base    = env_or("COOKBOOK_LDAP_BASE_DN", NULL);
    const char *ldap_attr    = env_or("COOKBOOK_LDAP_USER_ATTR", "uid");
    const char *ldap_grp     = env_or("COOKBOOK_LDAP_GROUP_ATTR", NULL);
    const char *ldap_grp_base = env_or("COOKBOOK_LDAP_GROUP_BASE", NULL);
    const char *oidc_issuer  = env_or("COOKBOOK_OIDC_ISSUER", NULL);
    const char *oidc_cid     = env_or("COOKBOOK_OIDC_CLIENT_ID", NULL);
    const char *ca_bundle    = env_or("COOKBOOK_CA_BUNDLE", NULL);

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

    /* database — backend chosen by the URL scheme / suffix of COOKBOOK_DB_URL:
     *   postgres:// or postgresql://  -> libpq backend
     *   *.kv                          -> apennines append-only KV
     *   *.apennines or apennines://   -> apennines t4/db/database (embedded SQL)
     *   anything else (or unset)      -> SQLite (default) */
    cookbook_db *db;
    if (db_url && (strstr(db_url, "postgres://") || strstr(db_url, "postgresql://"))) {
        db = cookbook_db_open_postgres(db_url);
        if (!db) {
            fprintf(stderr, "cookbook: failed to connect to PostgreSQL: %s\n", db_url);
            return 1;
        }
        printf("cookbook: database: PostgreSQL\n");
    } else if (db_url && (strstr(db_url, ".kv") != NULL)) {
        db = cookbook_db_open_kv(db_url);
        if (!db) {
            fprintf(stderr, "cookbook: failed to open KV store: %s\n", db_url);
            return 1;
        }
        printf("cookbook: database: KV store (%s)\n", db_url);
    } else if (db_url && (strstr(db_url, "apennines://") == db_url ||
                          strstr(db_url, ".apennines") != NULL)) {
        const char *ap_path = (strstr(db_url, "apennines://") == db_url)
                            ? db_url + strlen("apennines://")
                            : db_url;
        db = cookbook_db_open_apennines(ap_path);
        if (!db) {
            fprintf(stderr, "cookbook: failed to open apennines DB: %s\n", ap_path);
            return 1;
        }
        printf("cookbook: database: apennines t4/db/database (%s)\n", ap_path);
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
    cookbook_store *store;
    if (strcmp(storage_type, "s3") == 0) {
        if (!s3_bucket || !s3_access || !s3_secret) {
            fprintf(stderr, "cookbook: S3 storage requires "
                    "COOKBOOK_S3_BUCKET, COOKBOOK_S3_ACCESS_KEY, "
                    "COOKBOOK_S3_SECRET_KEY\n");
            db->close(db);
            return 1;
        }
        store = cookbook_store_open_s3(s3_bucket, s3_region,
                                       s3_access, s3_secret, s3_endpoint);
        if (!store) {
            fprintf(stderr, "cookbook: failed to open S3 storage: %s/%s\n",
                    s3_endpoint ? s3_endpoint : "s3.amazonaws.com", s3_bucket);
            db->close(db);
            return 1;
        }
        printf("cookbook: storage: s3://%s (region: %s)\n", s3_bucket, s3_region);
    } else {
        store = cookbook_store_open_fs(storage_dir);
        if (!store) {
            fprintf(stderr, "cookbook: failed to open storage: %s\n", storage_dir);
            db->close(db);
            return 1;
        }
        printf("cookbook: storage: %s\n", storage_dir);
    }

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
        .registry_sk         = has_key ? registry_sk : NULL,
        .grid_enabled        = grid_enabled,
        .grid_max_hops       = grid_max_hops,
        .grid_peer_auth      = grid_peer_auth,
        .audit_log_dir       = audit_dir,
        .object_cache_ttl_sec = obj_cache_ttl,
        .ldap_url            = ldap_url,
        .ldap_base_dn        = ldap_base,
        .ldap_user_attr      = ldap_attr,
        .ldap_group_attr     = ldap_grp,
        .ldap_group_base     = ldap_grp_base,
        .oidc_issuer         = oidc_issuer,
        .oidc_client_id      = oidc_cid,
        .ca_bundle_path      = ca_bundle
    };

    cookbook_server *srv = cookbook_server_start(&opts);
    if (!srv) {
        store->close(store);
        db->close(db);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* crash handlers — write crash.log before dying */
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif

    while (s_running) {
        cookbook_server_poll(srv, 100);
    }

    printf("\ncookbook: shutting down\n");
    cookbook_server_stop(srv);
    store->close(store);
    db->close(db);
    return 0;
}
