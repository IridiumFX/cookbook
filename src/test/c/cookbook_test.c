#include <stdio.h>
#include <string.h>
#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include <sodium.h>

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

/* ---- parameterized query tests ---- */

static void test_db_parameterized_exec(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT("org.test"),
        COOKBOOK_P_TEXT("tester")
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        params, 2);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized insert group");

    /* verify it was inserted */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.test") };
    st = db->query_p(db,
        "SELECT group_id FROM groups WHERE group_id = ?1",
        qp, 1, count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized query group");
    ASSERT(count == 1, "found parameterized group");

    /* test constraint violation via parameterized */
    st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        params, 2);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "parameterized duplicate rejected");

    db->close(db);
}

static void test_db_parameterized_artifact(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    cookbook_db_param gp[] = {
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("alice")
    };
    db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        gp, 2);

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.acme:core:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("abcdef1234567890"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(1024)
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap, 9);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized insert artifact");

    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.acme") };
    st = db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE group_id = ?1",
        qp, 1, count_cb, &count);
    ASSERT(st == COOKBOOK_DB_OK, "parameterized query artifacts");
    ASSERT(count == 1, "found parameterized artifact");

    db->close(db);
}

/* ---- SHA-256 tests ---- */

static void test_sha256_empty(void) {
    char hex[65];
    cookbook_sha256_hex("", 0, hex);
    ASSERT(strcmp(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
        "SHA-256 of empty string");
}

static void test_sha256_abc(void) {
    char hex[65];
    cookbook_sha256_hex("abc", 3, hex);
    ASSERT(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "SHA-256 of 'abc'");
}

static void test_sha256_long(void) {
    /* "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" */
    const char *msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[65];
    cookbook_sha256_hex(msg, strlen(msg), hex);
    ASSERT(strcmp(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
        "SHA-256 of NIST test vector");
}

/* ---- auth tests ---- */

static void test_base64url_roundtrip(void) {
    const char *input = "hello world";
    char encoded[64];
    size_t elen = cookbook_base64url_encode(input, strlen(input),
                                            encoded, sizeof(encoded));
    ASSERT(elen > 0, "base64url encode non-empty");
    ASSERT(strcmp(encoded, "aGVsbG8gd29ybGQ") == 0, "base64url encode correct");

    char decoded[64];
    size_t dlen = cookbook_base64url_decode(encoded, elen, decoded, sizeof(decoded));
    ASSERT(dlen == strlen(input), "base64url decode length");
    ASSERT(memcmp(decoded, input, dlen) == 0, "base64url roundtrip");
}

static void test_jwt_create_verify(void) {
    if (sodium_init() < -1) return;

    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("alice", "org.acme,org.beta", 3600, sk);
    ASSERT(token != NULL, "JWT create succeeds");

    /* verify with correct key */
    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "JWT verify succeeds");
    ASSERT(claims.valid == 1, "JWT claims valid");
    ASSERT(strcmp(claims.sub, "alice") == 0, "JWT sub=alice");
    ASSERT(strstr(claims.groups, "org.acme") != NULL, "JWT has org.acme group");
    ASSERT(claims.exp > 0, "JWT has expiry");

    /* group check */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme") == 1,
           "JWT has_group org.acme");
    ASSERT(cookbook_jwt_has_group(&claims, "org.beta") == 1,
           "JWT has_group org.beta");
    ASSERT(cookbook_jwt_has_group(&claims, "org.gamma") == 0,
           "JWT !has_group org.gamma");

    /* verify with wrong key should fail */
    unsigned char pk2[32], sk2[64];
    cookbook_keygen(pk2, sk2);
    cookbook_jwt_claims claims2;
    rc = cookbook_jwt_verify(token, pk2, &claims2);
    ASSERT(rc != 0, "JWT verify with wrong key fails");

    free(token);
}

static void test_ed25519_sign_verify(void) {
    if (sodium_init() < -1) return;

    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    const char *msg = "test message for signing";
    unsigned char sig[64];
    ASSERT(cookbook_sign(msg, strlen(msg), sig, sk) == 0, "Ed25519 sign");
    ASSERT(cookbook_verify(msg, strlen(msg), sig, pk) == 0, "Ed25519 verify");

    /* tamper with signature */
    sig[0] ^= 0xff;
    ASSERT(cookbook_verify(msg, strlen(msg), sig, pk) != 0,
           "Ed25519 verify tampered fails");
}

/* ---- mirror manifest / metrics integration tests ---- */

/* These test the server's mirror manifest and metrics endpoints via
   the server handler functions (which we can't call directly from here
   since they require civetweb). Instead, we test the underlying data
   queries that power them. */

static int mirror_count_cb(const cookbook_db_row *row, void *ctx) {
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

static void test_mirror_query(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert test data */
    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.acme', 'alice')");

    cookbook_db_param ap1[] = {
        COOKBOOK_P_TEXT("org.acme:core:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabbccdd"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(1024)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap1, 9);

    cookbook_db_param ap2[] = {
        COOKBOOK_P_TEXT("org.acme:core:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("eeff0011"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(2048)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ap2, 9);

    /* query all published — should find 2 */
    int count = 0;
    db->query(db,
        "SELECT DISTINCT group_id, artifact, version, triple "
        "FROM artifacts WHERE status = 'published' AND yanked = 0",
        mirror_count_cb, &count);
    ASSERT(count == 2, "mirror manifest finds 2 published artifacts");

    /* query specific coordinate */
    count = 0;
    cookbook_db_param qp[] = {
        COOKBOOK_P_TEXT("org.acme"),
        COOKBOOK_P_TEXT("core"),
        COOKBOOK_P_TEXT("1.0.0")
    };
    db->query_p(db,
        "SELECT group_id, artifact, version, triple "
        "FROM artifacts WHERE group_id = ?1 AND artifact = ?2 "
        "AND version = ?3 AND status = 'published'",
        qp, 3, mirror_count_cb, &count);
    ASSERT(count == 1, "mirror manifest finds specific version");

    db->close(db);
}

/* ---- S3 store open/close (no real server needed) ---- */

static void test_s3_store_open_null(void) {
    /* missing required params should return NULL */
    cookbook_store *s;
    s = cookbook_store_open_s3(NULL, "us-east-1", "key", "secret", NULL);
    ASSERT(s == NULL, "S3 open with NULL bucket fails");
    s = cookbook_store_open_s3("bucket", "us-east-1", NULL, "secret", NULL);
    ASSERT(s == NULL, "S3 open with NULL access_key fails");
    s = cookbook_store_open_s3("bucket", "us-east-1", "key", NULL, NULL);
    ASSERT(s == NULL, "S3 open with NULL secret_key fails");
}

static void test_s3_store_open_close(void) {
    /* opening with valid params should succeed (doesn't connect yet) */
    cookbook_store *s = cookbook_store_open_s3("test-bucket", "us-east-1",
                                              "AKIA_TEST", "secret123",
                                              "localhost:9000");
    ASSERT(s != NULL, "S3 open with valid params succeeds");
    if (s) s->close(s);
}

/* ---- PostgreSQL stub test ---- */

static void test_postgres_stub(void) {
    /* when built without libpq, open should return NULL gracefully */
    cookbook_db *db = cookbook_db_open_postgres("postgres://localhost/test");
#ifdef COOKBOOK_HAS_POSTGRES
    /* if PG is available, this would try to connect (and likely fail
       in test env), so we just skip */
    if (db) db->close(db);
#else
    ASSERT(db == NULL, "PG stub returns NULL when libpq unavailable");
#endif
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
    test_db_parameterized_exec();
    test_db_parameterized_artifact();
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_long();
    test_base64url_roundtrip();
    test_jwt_create_verify();
    test_ed25519_sign_verify();
    test_mirror_query();
    test_s3_store_open_null();
    test_s3_store_open_close();
    test_postgres_stub();
    test_semver_parse();
    test_semver_compare();
    test_range_caret();
    test_range_tilde();
    test_range_wildcard();
    test_range_bounded();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
