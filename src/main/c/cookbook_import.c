/* cookbook-import: Import artifacts from a local directory into a cookbook
   registry. Designed for air-gapped environments (spec §A.4).

   Usage:
     cookbook-import [options] <source-dir>

   Options:
     -u, --url <url>       Registry URL (default: http://localhost:8080)
     -t, --token <token>   Bearer JWT for authentication
     -d, --dry-run         List files that would be imported without uploading
     -v, --verbose         Print each file as it is uploaded
     -h, --help            Show this help

   The source directory should follow the mirror layout:
     <source-dir>/<group-path>/<artifact>/<version>/<filename>

   For example:
     ./mirror/org/acme/core/1.0.0/now.pasta
     ./mirror/org/acme/core/1.0.0/core-1.0.0-linux-x86_64-gnu.tar.gz
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <direct.h>
  #define stat_func _stat
  #define stat_type struct _stat
  #define PATH_SEP '\\'
#else
  #include <dirent.h>
  #include <unistd.h>
  #define stat_func stat
  #define stat_type struct stat
  #define PATH_SEP '/'
#endif

/* ---- minimal HTTP PUT via raw sockets ---- */

#ifdef _WIN32
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close closesocket
  static int sock_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2,2), &wsa);
  }
  static void sock_cleanup(void) { WSACleanup(); }
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
  static int sock_init(void) { return 0; }
  static void sock_cleanup(void) {}
#endif

typedef struct {
    const char *url;
    const char *token;
    int         dry_run;
    int         verbose;
    const char *source_dir;
} import_opts;

/* Parse host:port from URL like "http://host:port" */
static int parse_url(const char *url, char *host, size_t host_sz,
                     char *port, size_t port_sz, char *path_prefix,
                     size_t prefix_sz) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *hp = url + 7;
    const char *slash = strchr(hp, '/');
    const char *colon = strchr(hp, ':');

    if (slash) {
        snprintf(path_prefix, prefix_sz, "%s", slash);
    } else {
        snprintf(path_prefix, prefix_sz, "/");
        slash = hp + strlen(hp);
    }

    if (colon && colon < slash) {
        size_t hl = (size_t)(colon - hp);
        if (hl >= host_sz) hl = host_sz - 1;
        memcpy(host, hp, hl);
        host[hl] = '\0';
        size_t pl = (size_t)(slash - colon - 1);
        if (pl >= port_sz) pl = port_sz - 1;
        memcpy(port, colon + 1, pl);
        port[pl] = '\0';
    } else {
        size_t hl = (size_t)(slash - hp);
        if (hl >= host_sz) hl = host_sz - 1;
        memcpy(host, hp, hl);
        host[hl] = '\0';
        snprintf(port, port_sz, "80");
    }
    return 0;
}

static int http_put(const char *url, const char *rel_path,
                    const void *data, size_t data_len,
                    const char *token, char *resp_buf, size_t resp_sz) {
    char host[256], port[16], prefix[256];
    if (parse_url(url, host, sizeof(host), port, sizeof(port),
                  prefix, sizeof(prefix)) != 0) {
        fprintf(stderr, "error: invalid URL: %s\n", url);
        return -1;
    }

    /* remove trailing slash from prefix */
    size_t pfx_len = strlen(prefix);
    if (pfx_len > 1 && prefix[pfx_len - 1] == '/')
        prefix[pfx_len - 1] = '\0';

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "error: cannot resolve %s:%s\n", host, port);
        return -1;
    }

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "error: cannot connect to %s:%s\n", host, port);
        sock_close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    /* build HTTP request */
    char header[2048];
    int hlen;
    if (token && *token) {
        hlen = snprintf(header, sizeof(header),
            "PUT %s/artifact/%s HTTP/1.1\r\n"
            "Host: %s:%s\r\n"
            "Content-Length: %zu\r\n"
            "Authorization: Bearer %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            prefix, rel_path, host, port, data_len, token);
    } else {
        hlen = snprintf(header, sizeof(header),
            "PUT %s/artifact/%s HTTP/1.1\r\n"
            "Host: %s:%s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            prefix, rel_path, host, port, data_len);
    }

    /* send header + body */
    send(fd, header, hlen, 0);
    size_t sent = 0;
    while (sent < data_len) {
        int chunk = (data_len - sent > 65536) ? 65536 : (int)(data_len - sent);
        int n = send(fd, (const char *)data + sent, chunk, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }

    /* read response */
    size_t total = 0;
    while (total < resp_sz - 1) {
        int n = recv(fd, resp_buf + total, (int)(resp_sz - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    resp_buf[total] = '\0';

    sock_close(fd);

    /* extract status code */
    if (strncmp(resp_buf, "HTTP/1.", 7) == 0 && total > 12) {
        return atoi(resp_buf + 9);
    }
    return -1;
}

/* ---- directory traversal ---- */

typedef struct {
    char **paths;
    int    count;
    int    cap;
} file_list;

static void file_list_add(file_list *fl, const char *path) {
    if (fl->count >= fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 256;
        fl->paths = realloc(fl->paths, (size_t)fl->cap * sizeof(char *));
    }
    fl->paths[fl->count++] = strdup(path);
}

#ifdef _WIN32
static void scan_dir(const char *dir, const char *prefix, file_list *fl) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.cFileName[0] == '.') continue;
        char full[1024], rel[1024];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, fd.cFileName);
        else
            snprintf(rel, sizeof(rel), "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(full, rel, fl);
        } else {
            file_list_add(fl, rel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan_dir(const char *dir, const char *prefix, file_list *fl) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024], rel[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, ent->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", ent->d_name);

        stat_type st;
        if (stat_func(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, rel, fl);
        } else if (S_ISREG(st.st_mode)) {
            file_list_add(fl, rel);
        }
    }
    closedir(d);
}
#endif

static void *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    *out_len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <source-dir>\n"
        "\n"
        "Import artifacts from a local directory into a cookbook registry.\n"
        "\n"
        "Options:\n"
        "  -u, --url <url>       Registry URL (default: http://localhost:8080)\n"
        "  -t, --token <token>   Bearer JWT for authentication\n"
        "  -d, --dry-run         List files without uploading\n"
        "  -v, --verbose         Print each file as it is uploaded\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Source directory layout:\n"
        "  <source-dir>/<group-path>/<artifact>/<version>/<filename>\n"
        "\n"
        "Example:\n"
        "  %s -u http://registry:8080 -t eyJ... ./mirror\n",
        prog, prog);
}

int main(int argc, char **argv) {
    import_opts opts = {
        .url = "http://localhost:8080",
        .token = NULL,
        .dry_run = 0,
        .verbose = 0,
        .source_dir = NULL
    };

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--url") == 0) &&
            i + 1 < argc) {
            opts.url = argv[++i];
        } else if ((strcmp(argv[i], "-t") == 0 ||
                    strcmp(argv[i], "--token") == 0) && i + 1 < argc) {
            opts.token = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 ||
                   strcmp(argv[i], "--dry-run") == 0) {
            opts.dry_run = 1;
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--verbose") == 0) {
            opts.verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            opts.source_dir = argv[i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!opts.source_dir) {
        fprintf(stderr, "error: source directory required\n\n");
        usage(argv[0]);
        return 1;
    }

    /* scan source directory */
    file_list fl = { NULL, 0, 0 };
    scan_dir(opts.source_dir, "", &fl);

    if (fl.count == 0) {
        fprintf(stderr, "error: no files found in %s\n", opts.source_dir);
        return 1;
    }

    printf("cookbook-import: found %d file(s) in %s\n", fl.count, opts.source_dir);

    if (opts.dry_run) {
        for (int i = 0; i < fl.count; i++)
            printf("  %s\n", fl.paths[i]);
        printf("\n(dry run — no files uploaded)\n");
        for (int i = 0; i < fl.count; i++) free(fl.paths[i]);
        free(fl.paths);
        return 0;
    }

    /* initialize sockets */
    if (sock_init() != 0) {
        fprintf(stderr, "error: failed to initialize networking\n");
        return 1;
    }

    int uploaded = 0, failed = 0, skipped = 0;

    for (int i = 0; i < fl.count; i++) {
        const char *rel = fl.paths[i];

        /* skip .sha256, .sig, .countersig — registry generates these */
        if (strstr(rel, ".sha256") || strstr(rel, ".countersig")) {
            if (opts.verbose)
                printf("  skip (sidecar): %s\n", rel);
            skipped++;
            continue;
        }

        /* build full path */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", opts.source_dir, rel);

        size_t data_len = 0;
        void *data = read_file(full_path, &data_len);
        if (!data) {
            fprintf(stderr, "  error reading: %s\n", full_path);
            failed++;
            continue;
        }

        if (opts.verbose)
            printf("  PUT /artifact/%s (%zu bytes)...", rel, data_len);

        char resp[4096];
        int status = http_put(opts.url, rel, data, data_len,
                              opts.token, resp, sizeof(resp));
        free(data);

        if (status == 201) {
            uploaded++;
            if (opts.verbose) printf(" 201 created\n");
        } else if (status == 409) {
            skipped++;
            if (opts.verbose) printf(" 409 exists (skipped)\n");
        } else {
            failed++;
            if (opts.verbose)
                printf(" %d error\n", status);
            else
                fprintf(stderr, "  failed (%d): %s\n", status, rel);
        }
    }

    printf("\ncookbook-import: %d uploaded, %d skipped, %d failed\n",
           uploaded, skipped, failed);

    for (int i = 0; i < fl.count; i++) free(fl.paths[i]);
    free(fl.paths);
    sock_cleanup();

    return failed > 0 ? 1 : 0;
}
