/*
 * cookbook_stress — stress test driver for the cookbook registry server.
 *
 * Starts an in-process cookbook server and hammers it with concurrent
 * HTTP requests (publish, resolve, GET) using raw sockets. Reports
 * throughput, error rates, and latency percentiles.
 *
 * Usage:
 *   cookbook_stress [options]
 *
 *   -c, --concurrency <n>   Number of concurrent workers (default: 8)
 *   -n, --requests <n>      Total requests per phase (default: 1000)
 *   -p, --port <port>       Server port (default: 19080)
 *   -q, --quiet             Only print summary
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define CLOSESOCK closesocket
  typedef HANDLE thread_t;
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <pthread.h>
  #include <netdb.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define CLOSESOCK close
  typedef pthread_t thread_t;
#endif

#include "cookbook_server.h"
#include "cookbook_db.h"
#include "cookbook_store.h"

/* ---- timing ---- */

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart * 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ---- worker stats ---- */

typedef struct {
    int      requests;
    int      ok_2xx;
    int      err_4xx;
    int      err_5xx;
    int      err_conn;
    double  *latencies;    /* array of latency measurements (ms) */
    int      lat_count;
} worker_stats;

/* ---- HTTP helper ---- */

static char last_error_body[1024];

static int http_request(const char *host, int port,
                        const char *method, const char *path,
                        const char *content_type,
                        const char *accept,
                        const char *body, size_t body_len,
                        int *status_out, double *latency_ms) {
    double t0 = now_ms();
    *status_out = 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) return -1;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        CLOSESOCK(s);
        return -1;
    }

    /* build request */
    char req[4096];
    int rlen;
    if (body && body_len > 0) {
        rlen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "%s%s%s"
            "Connection: close\r\n"
            "\r\n",
            method, path, host, port,
            content_type ? content_type : "application/octet-stream",
            body_len,
            accept ? "Accept: " : "",
            accept ? accept : "",
            accept ? "\r\n" : "");
    } else {
        rlen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "%s%s%s"
            "Connection: close\r\n"
            "\r\n",
            method, path, host, port,
            accept ? "Accept: " : "",
            accept ? accept : "",
            accept ? "\r\n" : "");
    }

    send(s, req, rlen, 0);
    if (body && body_len > 0) {
        send(s, body, (int)body_len, 0);
    }

    /* read response — we only need the status line */
    char resp[8192];
    int total = 0, n;
    while (total < (int)sizeof(resp) - 1) {
        n = recv(s, resp + total, (int)sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        /* stop after headers */
        if (strstr(resp, "\r\n\r\n")) break;
    }
    resp[total] = '\0';
    CLOSESOCK(s);

    /* read remaining body */
    while (total < (int)sizeof(resp) - 1) {
        n = recv(s, resp + total, (int)sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    CLOSESOCK(s);

    /* parse status */
    if (total > 12 && strncmp(resp, "HTTP/1.", 7) == 0) {
        *status_out = atoi(resp + 9);
    }

    /* capture error body for debugging */
    if (*status_out >= 400) {
        char *body_start = strstr(resp, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            snprintf(last_error_body, sizeof(last_error_body),
                     "%s", body_start);
        }
    }

    *latency_ms = now_ms() - t0;
    return 0;
}

/* ---- worker context ---- */

typedef struct {
    int           id;
    int           port;
    int           requests;
    int           phase;       /* 0=publish, 1=resolve, 2=get, 3=conneg */
    int           quiet;
    worker_stats  stats;
} worker_ctx;

static const char *SAMPLE_PASTA =
    "{ group: \"org.stress\", artifact: \"lib\","
    " version: \"%d.%d.%d\" }";

#ifdef _WIN32
static DWORD WINAPI worker_thread(void *arg) {
#else
static void *worker_thread(void *arg) {
#endif
    worker_ctx *w = (worker_ctx *)arg;
    worker_stats *s = &w->stats;

    s->latencies = malloc(sizeof(double) * (size_t)w->requests);
    s->lat_count = 0;

    for (int i = 0; i < w->requests; i++) {
        int status = 0;
        double lat = 0;
        int rc = -1;

        if (w->phase == 0) {
            /* PUBLISH: PUT unique descriptors */
            int major = w->id;
            int minor = i / 256;
            int patch = i % 256;
            char pasta_body[512];
            snprintf(pasta_body, sizeof(pasta_body), SAMPLE_PASTA,
                     major, minor, patch);
            char path[256];
            snprintf(path, sizeof(path),
                "/artifact/org/stress/lib/%d.%d.%d/now.pasta",
                major, minor, patch);
            rc = http_request("127.0.0.1", w->port,
                "PUT", path, "application/x-pasta", NULL,
                pasta_body, strlen(pasta_body),
                &status, &lat);
        } else if (w->phase == 1) {
            /* RESOLVE: GET /resolve/ with range */
            char path[256];
            snprintf(path, sizeof(path),
                "/resolve/org.stress/lib/^%d.0.0", w->id);
            rc = http_request("127.0.0.1", w->port,
                "GET", path, NULL, NULL, NULL, 0, &status, &lat);
        } else if (w->phase == 2) {
            /* GET: fetch published descriptors */
            int patch = i % 256;
            int minor = i / 256;
            char path[256];
            snprintf(path, sizeof(path),
                "/artifact/org/stress/lib/%d.%d.%d/now.pasta",
                w->id, minor, patch);
            rc = http_request("127.0.0.1", w->port,
                "GET", path, NULL, NULL, NULL, 0, &status, &lat);
        } else if (w->phase == 3) {
            /* CONNEG: GET descriptors with different Accept headers */
            int patch = i % 256;
            int minor = i / 256;
            char path[256];
            snprintf(path, sizeof(path),
                "/artifact/org/stress/lib/%d.%d.%d/now.pasta",
                w->id, minor, patch);
            const char *accepts[] = {
                "application/x-pasta",
                "application/json",
                "text/plain",
                "*/*"
            };
            rc = http_request("127.0.0.1", w->port,
                "GET", path, NULL, accepts[i % 4],
                NULL, 0, &status, &lat);
        }

        s->requests++;
        if (rc != 0) {
            s->err_conn++;
        } else if (status >= 200 && status < 300) {
            s->ok_2xx++;
        } else if (status >= 400 && status < 500) {
            s->err_4xx++;
        } else if (status >= 500) {
            s->err_5xx++;
        } else {
            s->err_conn++; /* unexpected status */
        }

        if (rc == 0) {
            s->latencies[s->lat_count++] = lat;
        }
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- percentile ---- */

static int double_cmp(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(double *arr, int n, double p) {
    if (n == 0) return 0;
    int idx = (int)(p / 100.0 * (double)(n - 1));
    if (idx >= n) idx = n - 1;
    return arr[idx];
}

/* ---- main ---- */

int main(int argc, char **argv) {
    int concurrency = 8;
    int requests = 1000;
    int port = 19080;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 ||
             strcmp(argv[i], "--concurrency") == 0) && i + 1 < argc)
            concurrency = atoi(argv[++i]);
        else if ((strcmp(argv[i], "-n") == 0 ||
                  strcmp(argv[i], "--requests") == 0) && i + 1 < argc)
            requests = atoi(argv[++i]);
        else if ((strcmp(argv[i], "-p") == 0 ||
                  strcmp(argv[i], "--port") == 0) && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-q") == 0 ||
                 strcmp(argv[i], "--quiet") == 0)
            quiet = 1;
        else {
            fprintf(stderr,
                "Usage: cookbook_stress [-c concurrency] [-n requests] "
                "[-p port] [-q]\n");
            return 1;
        }
    }

    /* limit to sane values */
    if (concurrency < 1) concurrency = 1;
    if (concurrency > 64) concurrency = 64;
    if (requests < 1) requests = 1;
    if (requests > 100000) requests = 100000;

    int per_worker = requests / concurrency;
    if (per_worker < 1) per_worker = 1;

    printf("cookbook stress test\n");
    printf("  concurrency: %d\n", concurrency);
    printf("  requests/phase: %d (%d per worker)\n",
           per_worker * concurrency, per_worker);
    printf("  port: %d\n\n", port);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* start server */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    if (!db) { fprintf(stderr, "failed to open db\n"); return 1; }
    cookbook_db_migrate(db);

    char store_dir[256];
    snprintf(store_dir, sizeof(store_dir), "./stress_data_%d", port);
    cookbook_store *store = cookbook_store_open_fs(store_dir);
    if (!store) { fprintf(stderr, "failed to open store\n"); return 1; }

    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);

    cookbook_server_opts opts = {
        .listen_url = listen_url,
        .registry_id = "stress",
        .db = db,
        .store = store,
        .max_upload_mb = 0,
        .pending_timeout_sec = 3600,
        .jwt_ttl_sec = 3600,
        .rate_limit_per_min = 0,
        .registry_pk = NULL,
        .registry_sk = NULL
    };

    cookbook_server *srv = cookbook_server_start(&opts);
    if (!srv) {
        fprintf(stderr, "failed to start server on port %d\n", port);
        store->close(store);
        db->close(db);
        return 1;
    }

    /* give server a moment to bind */
#ifdef _WIN32
    Sleep(200);
#else
    usleep(200000);
#endif

    const char *phase_names[] = {
        "PUBLISH (PUT)",
        "RESOLVE (GET /resolve/)",
        "GET (artifact)",
        "CONNEG (Accept header variants)"
    };

    for (int phase = 0; phase < 4; phase++) {
        if (!quiet) printf("--- Phase %d: %s ---\n", phase, phase_names[phase]);

        worker_ctx *workers = calloc((size_t)concurrency, sizeof(worker_ctx));
        thread_t *threads = calloc((size_t)concurrency, sizeof(thread_t));

        double t_start = now_ms();

        for (int i = 0; i < concurrency; i++) {
            workers[i].id = i;
            workers[i].port = port;
            workers[i].requests = per_worker;
            workers[i].phase = phase;
            workers[i].quiet = quiet;

#ifdef _WIN32
            threads[i] = CreateThread(NULL, 0, worker_thread,
                                       &workers[i], 0, NULL);
#else
            pthread_create(&threads[i], NULL, worker_thread, &workers[i]);
#endif
        }

        for (int i = 0; i < concurrency; i++) {
#ifdef _WIN32
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
#else
            pthread_join(threads[i], NULL);
#endif
        }

        double elapsed = now_ms() - t_start;

        /* aggregate stats */
        int total_req = 0, total_2xx = 0, total_4xx = 0;
        int total_5xx = 0, total_conn = 0;
        int total_lat = 0;

        /* count total latencies first */
        for (int i = 0; i < concurrency; i++)
            total_lat += workers[i].stats.lat_count;

        double *all_lat = malloc(sizeof(double) * (size_t)(total_lat + 1));
        int lat_idx = 0;

        for (int i = 0; i < concurrency; i++) {
            worker_stats *s = &workers[i].stats;
            total_req += s->requests;
            total_2xx += s->ok_2xx;
            total_4xx += s->err_4xx;
            total_5xx += s->err_5xx;
            total_conn += s->err_conn;
            for (int j = 0; j < s->lat_count; j++)
                all_lat[lat_idx++] = s->latencies[j];
            free(s->latencies);
        }

        qsort(all_lat, (size_t)lat_idx, sizeof(double), double_cmp);

        double rps = (elapsed > 0) ? (total_req / elapsed * 1000.0) : 0;

        printf("  requests:   %d in %.0f ms (%.0f req/s)\n",
               total_req, elapsed, rps);
        printf("  status:     2xx=%d  4xx=%d  5xx=%d  conn_err=%d\n",
               total_2xx, total_4xx, total_5xx, total_conn);
        if (lat_idx > 0) {
            printf("  latency:    p50=%.1f ms  p95=%.1f ms  p99=%.1f ms  "
                   "max=%.1f ms\n",
                   percentile(all_lat, lat_idx, 50),
                   percentile(all_lat, lat_idx, 95),
                   percentile(all_lat, lat_idx, 99),
                   all_lat[lat_idx - 1]);
        }
        if ((total_4xx > 0 || total_5xx > 0) && last_error_body[0]) {
            printf("  last error: %s\n", last_error_body);
            last_error_body[0] = '\0';
        }
        printf("\n");

        free(all_lat);
        free(workers);
        free(threads);
    }

    /* cleanup */
    cookbook_server_stop(srv);

    /* remove stress data directory (best-effort) */
    char rm_cmd[300];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", store_dir);
    system(rm_cmd);

    printf("stress test complete.\n");

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
