#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "civetweb.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

struct cookbook_server {
    struct mg_context  *ctx;
    cookbook_db         *db;
    cookbook_store      *store;
    char               *registry_id;
};

/* ---- helpers ---- */

/* URL-decode in place. Returns decoded length. */
static size_t url_decode(char *buf, size_t len) {
    size_t i = 0, j = 0;
    while (i < len) {
        if (buf[i] == '%' && i + 2 < len) {
            char hex[3] = { buf[i+1], buf[i+2], '\0' };
            buf[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (buf[i] == '+') {
            buf[j++] = ' ';
            i++;
        } else {
            buf[j++] = buf[i++];
        }
    }
    buf[j] = '\0';
    return j;
}

/* Return malloc'd copy of URI after prefix, URL-decoded. */
static char *path_after(const char *uri, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return NULL;
    char *buf = strdup(uri + plen);
    if (buf) url_decode(buf, strlen(buf));
    return buf;
}

/* Split "org/acme/rocketlib/^1.3.0" into group, artifact, tail.
   group = "org.acme" (slashes to dots, all but last two segments)
   artifact = "rocketlib" (second-to-last segment)
   tail = "^1.3.0" (last segment) */
static int split_coord(const char *path, char **group, char **artifact,
                        char **tail) {
    *group = *artifact = *tail = NULL;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return -1;

    *tail = strdup(last_slash + 1);

    const char *prev = last_slash - 1;
    while (prev > path && *prev != '/') prev--;
    if (*prev == '/') {
        size_t art_len = (size_t)(last_slash - prev - 1);
        *artifact = malloc(art_len + 1);
        memcpy(*artifact, prev + 1, art_len);
        (*artifact)[art_len] = '\0';

        size_t grp_len = (size_t)(prev - path);
        *group = malloc(grp_len + 1);
        memcpy(*group, path, grp_len);
        (*group)[grp_len] = '\0';
        for (size_t i = 0; i < grp_len; i++)
            if ((*group)[i] == '/') (*group)[i] = '.';
    } else {
        /* only two segments — not enough for group/artifact/tail */
        free(*tail);
        *tail = NULL;
        return -1;
    }
    return 0;
}

static void send_json(struct mg_connection *conn, int status,
                       const char *body) {
    size_t len = strlen(body);
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "\r\n"
              "%s",
              status, (status < 300) ? "OK" : "Error",
              len, body);
}

/* Read the full request body into a malloc'd buffer. */
static char *read_body(struct mg_connection *conn,
                        const struct mg_request_info *ri,
                        size_t *out_len) {
    long long cl = ri->content_length;
    if (cl <= 0) {
        /* try chunked/unknown length: read up to 1MB */
        size_t cap = 65536, total = 0;
        char *buf = malloc(cap);
        if (!buf) { *out_len = 0; return NULL; }
        for (;;) {
            if (total >= cap) {
                cap *= 2;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(buf); *out_len = 0; return NULL; }
                buf = tmp;
            }
            int n = mg_read(conn, buf + total, cap - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        if (total == 0) { free(buf); *out_len = 0; return NULL; }
        *out_len = total;
        return buf;
    }
    char *buf = malloc((size_t)cl);
    if (!buf) { *out_len = 0; return NULL; }
    size_t total = 0;
    while (total < (size_t)cl) {
        int n = mg_read(conn, buf + total, (size_t)cl - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (total == 0) { free(buf); *out_len = 0; return NULL; }
    *out_len = total;
    return buf;
}

/* ---- route: GET /healthz ---- */

static int handle_healthz(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    send_json(conn, 200, "{\"status\":\"ok\"}\n");
    return 1;
}

/* ---- route: GET /resolve/{group}/{artifact}/{range} ---- */

typedef struct {
    cookbook_range *range;
    char          *buf;
    size_t         len;
    size_t         cap;
    int            count;
} resolve_filter_ctx;

static int resolve_filter_cb(const cookbook_db_row *row, void *user) {
    resolve_filter_ctx *ctx = (resolve_filter_ctx *)user;
    const char *version  = row->values[0];
    const char *snapshot = row->values[1];
    const char *triple   = row->values[2];
    if (!version) return 0;

    cookbook_semver sv;
    if (cookbook_semver_parse(version, &sv) != 0) return 0;
    if (!cookbook_range_satisfies(ctx->range, &sv)) return 0;

    int n;
    if (ctx->count > 0) {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, ",");
        if (n > 0) ctx->len += (size_t)n;
    }
    n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
        "{\"version\":\"%s\",\"snapshot\":%s,\"triples\":[\"%s\"]}",
        version,
        (snapshot && snapshot[0] == '1') ? "true" : "false",
        triple ? triple : "noarch");
    if (n > 0) ctx->len += (size_t)n;
    ctx->count++;
    return 0;
}

static int handle_resolve(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    char *path = path_after(ri->local_uri, "/resolve/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    char *group = NULL, *artifact = NULL, *range_str = NULL;
    if (split_coord(path, &group, &artifact, &range_str) != 0 || !range_str) {
        send_json(conn, 400, "{\"error\":\"Malformed path\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    cookbook_range range;
    if (cookbook_range_parse(range_str, &range) != 0) {
        send_json(conn, 400, "{\"error\":\"Malformed range string\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT a.version, a.snapshot, a.triple "
        "FROM artifacts a "
        "JOIN artifact_semver s ON a.coord_id = s.coord_id "
        "WHERE a.group_id = '%s' AND a.artifact = '%s' "
        "AND a.yanked = 0 AND a.status = 'published' "
        "ORDER BY s.major DESC, s.minor DESC, s.patch DESC",
        group, artifact);

    char result_buf[8192] = {0};
    resolve_filter_ctx ctx = { &range, result_buf, 0, sizeof(result_buf), 0 };

    cookbook_db_status st = srv->db->query(srv->db, sql, resolve_filter_cb, &ctx);

    if (st != COOKBOOK_DB_OK) {
        send_json(conn, 500, "{\"error\":\"Database error\"}\n");
    } else {
        char response[8320];
        snprintf(response, sizeof(response),
                 "{\"versions\":[%s]}\n", result_buf);
        send_json(conn, 200, response);
    }

    free(path); free(group); free(artifact); free(range_str);
    return 1;
}

/* ---- route: /artifact/... ---- */

static int handle_artifact(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    char *path = path_after(ri->local_uri, "/artifact/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    /* build object store key: registry_id/path */
    size_t key_len = strlen(srv->registry_id) + 1 + strlen(path);
    char *key = malloc(key_len + 1);
    snprintf(key, key_len + 1, "%s/%s", srv->registry_id, path);

    if (strcmp(ri->request_method, "GET") == 0) {
        /* ---- GET: serve artifact ---- */
        void *data = NULL;
        size_t len = 0;
        cookbook_store_status sst = srv->store->get(srv->store, key, &data, &len);

        if (sst == COOKBOOK_STORE_NOT_FOUND) {
            send_json(conn, 404, "{\"error\":\"Not found\"}\n");
        } else if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
        } else {
            const char *ct = "application/octet-stream";
            if (strstr(path, ".sha256")) ct = "text/plain";
            else if (strstr(path, ".pasta")) ct = "text/plain";
            else if (strstr(path, ".tar.gz")) ct = "application/gzip";

            mg_send_http_ok(conn, ct, (long long)len);
            mg_write(conn, data, len);
            srv->store->free_buf(data);
        }

    } else if (strcmp(ri->request_method, "PUT") == 0) {
        /* ---- PUT: publish artifact ---- */

        /* immutability check */
        if (srv->store->exists(srv->store, key) == COOKBOOK_STORE_OK) {
            send_json(conn, 409,
                "{\"error\":\"Release coordinate already published\"}\n");
            free(key); free(path);
            return 1;
        }

        /* read body */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len);
        if (!body || body_len == 0) {
            send_json(conn, 400, "{\"error\":\"Empty body\"}\n");
            free(body); free(key); free(path);
            return 1;
        }

        /* store */
        cookbook_store_status sst = srv->store->put(srv->store, key,
                                                     body, body_len);
        if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
            free(body); free(key); free(path);
            return 1;
        }

        /* if now.pasta, parse and register metadata */
        const char *filename = strrchr(path, '/');
        filename = filename ? filename + 1 : path;

        if (strcmp(filename, "now.pasta") == 0) {
            PastaResult pr;
            PastaValue *root = pasta_parse(body, body_len, &pr);
            if (!root) {
                char err[512];
                snprintf(err, sizeof(err),
                    "{\"error\":\"Invalid now.pasta: %s\"}\n", pr.message);
                srv->store->del(srv->store, key);
                send_json(conn, 400, err);
                free(body); free(key); free(path);
                return 1;
            }

            const PastaValue *v_group = pasta_map_get(root, "group");
            const PastaValue *v_artifact = pasta_map_get(root, "artifact");
            const PastaValue *v_version = pasta_map_get(root, "version");

            if (v_group && v_artifact && v_version) {
                const char *grp = pasta_get_string(v_group);
                const char *art = pasta_get_string(v_artifact);
                const char *ver = pasta_get_string(v_version);

                if (grp && art && ver) {
                    char sql[2048];
                    snprintf(sql, sizeof(sql),
                        "INSERT OR IGNORE INTO groups (group_id, owner_sub) "
                        "VALUES ('%s', 'anonymous')", grp);
                    srv->db->exec(srv->db, sql);

                    cookbook_semver sv;
                    cookbook_semver_parse(ver, &sv);

                    snprintf(sql, sizeof(sql),
                        "INSERT OR IGNORE INTO artifacts "
                        "(coord_id, group_id, artifact, version, triple, "
                        " sha256, status) "
                        "VALUES ('%s:%s:%s:noarch', '%s', '%s', '%s', "
                        "'noarch', 'pending', 'published')",
                        grp, art, ver, grp, art, ver);
                    srv->db->exec(srv->db, sql);

                    snprintf(sql, sizeof(sql),
                        "INSERT OR IGNORE INTO artifact_semver "
                        "(coord_id, major, minor, patch, pre_release, "
                        "build_meta) "
                        "VALUES ('%s:%s:%s:noarch', %d, %d, %d, '%s', '%s')",
                        grp, art, ver,
                        sv.major, sv.minor, sv.patch,
                        sv.pre_release, sv.build_meta);
                    srv->db->exec(srv->db, sql);
                }
            }

            pasta_free(root);
        }

        send_json(conn, 201, "{\"status\":\"created\"}\n");
        free(body);

    } else {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    }

    free(key);
    free(path);
    return 1;
}

/* ---- public API ---- */

cookbook_server *cookbook_server_start(const cookbook_server_opts *opts) {
    if (!opts || !opts->db || !opts->store) return NULL;

    cookbook_server *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->db    = opts->db;
    srv->store = opts->store;
    srv->registry_id = strdup(opts->registry_id ? opts->registry_id : "central");

    /* extract port from listen_url (e.g. "http://0.0.0.0:8080" -> "8080") */
    const char *url = opts->listen_url ? opts->listen_url : "http://0.0.0.0:8080";
    const char *port = "8080";
    const char *colon = strrchr(url, ':');
    if (colon) port = colon + 1;

    const char *civetweb_opts[] = {
        "listening_ports", port,
        "num_threads", "4",
        "request_timeout_ms", "30000",
        NULL
    };

    srv->ctx = mg_start(NULL, NULL, civetweb_opts);
    if (!srv->ctx) {
        fprintf(stderr, "cookbook: failed to start civetweb on port %s\n", port);
        free(srv->registry_id);
        free(srv);
        return NULL;
    }

    /* register route handlers */
    mg_set_request_handler(srv->ctx, "/healthz", handle_healthz, srv);
    mg_set_request_handler(srv->ctx, "/resolve/", handle_resolve, srv);
    mg_set_request_handler(srv->ctx, "/artifact/", handle_artifact, srv);

    fprintf(stdout, "cookbook: listening on %s (registry: %s)\n",
            url, srv->registry_id);
    return srv;
}

int cookbook_server_poll(cookbook_server *srv, int timeout_ms) {
    if (!srv || !srv->ctx) return -1;
    /* civetweb is threaded — poll just sleeps */
#ifdef _WIN32
    Sleep((DWORD)timeout_ms);
#else
    struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
    return 0;
}

void cookbook_server_stop(cookbook_server *srv) {
    if (!srv) return;
    if (srv->ctx) mg_stop(srv->ctx);
    free(srv->registry_id);
    free(srv);
}
