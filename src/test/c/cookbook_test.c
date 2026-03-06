#include <stdio.h>
#include <string.h>
#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_semver.h"

static int tests_run    = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do {                          \
    tests_run++;                                        \
    if (!(cond)) {                                      \
        tests_failed++;                                 \
        fprintf(stderr, "FAIL: %s (%s:%d)\n",           \
                (msg), __FILE__, __LINE__);              \
    }                                                   \
} while (0)

static void test_version(void) {
    ASSERT(cookbook_version_major() == 0, "major == 0");
    ASSERT(cookbook_version_minor() == 1, "minor == 1");
    ASSERT(cookbook_version_patch() == 0, "patch == 0");
}

static void test_resources_path(void) {
#ifdef COOKBOOK_TEST_RESOURCES
    const char *res = COOKBOOK_TEST_RESOURCES;
    ASSERT(res != NULL && strlen(res) > 0, "resource path is set");
#else
    ASSERT(0, "COOKBOOK_TEST_RESOURCES not defined");
#endif
}

static void test_db_open_close(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    ASSERT(db != NULL, "open in-memory sqlite");
    db->close(db);
}

static void test_db_migrate(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    ASSERT(db != NULL, "open db for migrate");
    cookbook_db_status st = cookbook_db_migrate(db);
    ASSERT(st == COOKBOOK_DB_OK, "migrate succeeds");
    db->close(db);
}

static int count_cb(const cookbook_db_row *row, void *ctx) {
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

static void test_db_groups_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_status st = db->exec(db,
        "INSERT INTO groups (group_id, owner_sub, created_at) "
        "VALUES ('org.acme', 'alice', '2026-01-01T00:00:00Z')");
    ASSERT(st == COOKBOOK_DB_OK, "insert group");

    int count = 0;
    st = db->query(db,
        "SELECT group_id FROM groups WHERE group_id = 'org.acme'",
        count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "query group");
    ASSERT(count == 1, "found one group");

    st = db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'bob')");
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate group rejected");

    db->close(db);
}

static void test_db_artifacts_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'alice')");

    cookbook_db_status st = db->exec(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256) "
        "VALUES ('org.acme:core:1.0.0:linux:amd64:gnu', 'org.acme', "
        "'core', '1.0.0', 'linux:amd64:gnu', 'deadbeef')");
    ASSERT(st == COOKBOOK_DB_OK, "insert artifact");

    st = db->exec(db,
        "INSERT INTO artifact_semver (coord_id, major, minor, patch) "
        "VALUES ('org.acme:core:1.0.0:linux:amd64:gnu', 1, 0, 0)");
    ASSERT(st == COOKBOOK_DB_OK, "insert semver index");

    int count = 0;
    st = db->query(db,
        "SELECT coord_id FROM artifacts WHERE group_id = 'org.acme'",
        count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "query artifacts");
    ASSERT(count == 1, "found one artifact");

    db->close(db);
}

static void test_store_put_get(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open fs store");

    const char *data = "hello cookbook";
    cookbook_store_status st = store->put(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz",
        data, strlen(data));
    ASSERT(st == COOKBOOK_STORE_OK, "put object");

    st = store->exists(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_OK, "object exists");

    void *buf = NULL;
    size_t len = 0;
    st = store->get(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz",
        &buf, &len);
    ASSERT(st == COOKBOOK_STORE_OK, "get object");
    ASSERT(len == strlen(data), "correct length");
    ASSERT(buf && memcmp(buf, data, len) == 0, "correct content");
    store->free_buf(buf);

    st = store->del(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_OK, "delete object");

    st = store->exists(store,
        "central/org/acme/core/1.0.0/core-1.0.0-noarch.tar.gz");
    ASSERT(st == COOKBOOK_STORE_NOT_FOUND, "object gone after delete");

    store->close(store);
}

static void test_store_not_found(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open fs store for not-found test");

    void *buf = NULL;
    size_t len = 0;
    cookbook_store_status st = store->get(store, "no/such/key", &buf, &len);
    ASSERT(st == COOKBOOK_STORE_NOT_FOUND, "missing key returns not found");

    store->close(store);
}

/* ---- semver tests ---- */

static void test_semver_parse(void) {
    cookbook_semver sv;
    ASSERT(cookbook_semver_parse("1.3.0", &sv) == 0, "parse 1.3.0");
    ASSERT(sv.major == 1 && sv.minor == 3 && sv.patch == 0, "1.3.0 fields");
    ASSERT(sv.pre_release[0] == '\0', "no pre-release");

    ASSERT(cookbook_semver_parse("0.1.0-beta.1+build.42", &sv) == 0,
           "parse with pre+meta");
    ASSERT(sv.major == 0 && sv.minor == 1 && sv.patch == 0, "0.1.0 fields");
    ASSERT(strcmp(sv.pre_release, "beta.1") == 0, "pre_release=beta.1");
    ASSERT(strcmp(sv.build_meta, "build.42") == 0, "build_meta=build.42");

    ASSERT(cookbook_semver_parse("nope", &sv) != 0, "reject garbage");
    ASSERT(cookbook_semver_parse("1.2", &sv) != 0, "reject incomplete");
}

static void test_semver_compare(void) {
    cookbook_semver a, b;
    cookbook_semver_parse("1.0.0", &a);
    cookbook_semver_parse("2.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.0.0 < 2.0.0");

    cookbook_semver_parse("1.0.0-alpha", &a);
    cookbook_semver_parse("1.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.0.0-alpha < 1.0.0");

    cookbook_semver_parse("1.0.0-alpha", &a);
    cookbook_semver_parse("1.0.0-beta", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "alpha < beta");
}

static void test_range_caret(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("^1.3.0", &r) == 0, "parse ^1.3.0");
    ASSERT(r.type == COOKBOOK_RANGE_CARET, "type is caret");

    cookbook_semver_parse("1.3.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.3.0 satisfies ^1.3.0");

    cookbook_semver_parse("1.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.4.0 satisfies ^1.3.0");

    cookbook_semver_parse("1.99.99", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.99.99 satisfies ^1.3.0");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails ^1.3.0");

    cookbook_semver_parse("1.2.9", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.9 fails ^1.3.0");

    /* ^0.3.0 means >=0.3.0, <0.4.0 */
    ASSERT(cookbook_range_parse("^0.3.0", &r) == 0, "parse ^0.3.0");
    cookbook_semver_parse("0.3.5", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "0.3.5 satisfies ^0.3.0");
    cookbook_semver_parse("0.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.4.0 fails ^0.3.0");
}

static void test_range_tilde(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("~1.3.0", &r) == 0, "parse ~1.3.0");

    cookbook_semver_parse("1.3.5", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.3.5 satisfies ~1.3.0");

    cookbook_semver_parse("1.4.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.4.0 fails ~1.3.0");
}

static void test_range_wildcard(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("1.*", &r) == 0, "parse 1.*");
    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.0.0 satisfies 1.*");
    cookbook_semver_parse("1.99.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.99.0 satisfies 1.*");
    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails 1.*");

    ASSERT(cookbook_range_parse("*", &r) == 0, "parse *");
    cookbook_semver_parse("99.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "99.0.0 satisfies *");
}

static void test_range_bounded(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("[1.0.0,2.0.0)", &r) == 0, "parse [1.0.0,2.0.0)");

    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.0.0 satisfies [1,2)");

    cookbook_semver_parse("1.5.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.5.0 satisfies [1,2)");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "2.0.0 fails [1,2)");

    cookbook_semver_parse("0.9.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.9.0 fails [1,2)");
}

int main(void) {
    printf("cookbook test suite\n\n");

    test_version();
    test_resources_path();
    test_db_open_close();
    test_db_migrate();
    test_db_groups_crud();
    test_db_artifacts_crud();
    test_store_put_get();
    test_store_not_found();
    test_semver_parse();
    test_semver_compare();
    test_range_caret();
    test_range_tilde();
    test_range_wildcard();
    test_range_bounded();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
