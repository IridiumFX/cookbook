#include <stdio.h>
#include <string.h>
#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include "pasta.h"
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

/* ---- #8: content negotiation tests ---- */

static void test_validate_ascii(void) {
    /* valid ASCII */
    ASSERT(cookbook_validate_ascii("hello", 5) == 0, "pure ASCII is valid");
    ASSERT(cookbook_validate_ascii("", 0) == 0, "empty is valid");
    ASSERT(cookbook_validate_ascii("a = 1\nb = 2\n", 12) == 0,
           "ASCII with newlines");

    /* byte > 0x7F */
    ASSERT(cookbook_validate_ascii("caf\xC3\xA9", 5) == 4,
           "reject UTF-8 multi-byte at offset 4");
    ASSERT(cookbook_validate_ascii("\x80", 1) == 1,
           "reject 0x80 at offset 1");
    ASSERT(cookbook_validate_ascii("ab\xFF", 3) == 3,
           "reject 0xFF at offset 3");

    /* NUL byte */
    ASSERT(cookbook_validate_ascii("ab\x00" "cd", 5) == 3,
           "reject NUL at offset 3");
    ASSERT(cookbook_validate_ascii("\x00", 1) == 1,
           "reject NUL at offset 1");
}

static void test_pasta_to_json_primitives(void) {
    /* null */
    char *j = cookbook_pasta_to_json(NULL);
    ASSERT(j && strcmp(j, "null") == 0, "NULL → \"null\"");
    free(j);

    /* parse a small Pasta doc and convert to JSON */
    PastaResult pr;
    const char *src = "{ name: \"test\", count: 42, flag: true, empty: null }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse small Pasta doc");
    if (root) {
        j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "JSON serialization succeeds");
        /* verify key fields are present */
        ASSERT(strstr(j, "\"name\"") != NULL, "JSON has name key");
        ASSERT(strstr(j, "\"test\"") != NULL, "JSON has test value");
        ASSERT(strstr(j, "\"count\"") != NULL, "JSON has count key");
        ASSERT(strstr(j, "42") != NULL, "JSON has 42");
        ASSERT(strstr(j, "true") != NULL, "JSON has true");
        ASSERT(strstr(j, "null") != NULL, "JSON has null");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_nested(void) {
    PastaResult pr;
    const char *src =
        "{ versions: [ { version: \"1.0.0\", triples: [\"noarch\"] } ] }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse nested Pasta");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "nested JSON serialization");
        ASSERT(strstr(j, "\"versions\"") != NULL, "has versions key");
        ASSERT(strstr(j, "\"1.0.0\"") != NULL, "has version value");
        ASSERT(strstr(j, "\"noarch\"") != NULL, "has triple value");
        ASSERT(j[0] == '{', "starts with {");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_escaping(void) {
    PastaResult pr;
    const char *src = "{ msg: \"line1\\nline2\\ttab\" }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse Pasta with escapes");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "escape JSON serialization");
        ASSERT(strstr(j, "\\n") != NULL, "JSON has \\n escape");
        ASSERT(strstr(j, "\\t") != NULL, "JSON has \\t escape");
        free(j);
        pasta_free(root);
    }
}

/* ---- additional semver edge cases ---- */

static void test_semver_parse_edge_cases(void) {
    cookbook_semver sv;

    /* large version numbers */
    ASSERT(cookbook_semver_parse("999.999.999", &sv) == 0, "parse large version");
    ASSERT(sv.major == 999 && sv.minor == 999 && sv.patch == 999,
           "large version fields");

    /* version 0.0.0 */
    ASSERT(cookbook_semver_parse("0.0.0", &sv) == 0, "parse 0.0.0");
    ASSERT(sv.major == 0 && sv.minor == 0 && sv.patch == 0, "0.0.0 fields");

    /* pre-release only (no build metadata) */
    ASSERT(cookbook_semver_parse("1.0.0-rc.1", &sv) == 0, "parse rc.1");
    ASSERT(strcmp(sv.pre_release, "rc.1") == 0, "pre_release=rc.1");
    ASSERT(sv.build_meta[0] == '\0', "no build_meta");

    /* build metadata only (no pre-release) */
    ASSERT(cookbook_semver_parse("1.0.0+sha.abc123", &sv) == 0,
           "parse build meta only");
    ASSERT(sv.pre_release[0] == '\0', "no pre_release");
    ASSERT(strcmp(sv.build_meta, "sha.abc123") == 0, "build_meta=sha.abc123");

    /* reject edge cases */
    ASSERT(cookbook_semver_parse("", &sv) != 0, "reject empty");
    ASSERT(cookbook_semver_parse("v1.0.0", &sv) != 0, "reject v-prefix");
    ASSERT(cookbook_semver_parse("1.0", &sv) != 0, "reject two-part");
    ASSERT(cookbook_semver_parse("1.0.0.0", &sv) != 0, "reject four-part");
    ASSERT(cookbook_semver_parse("-1.0.0", &sv) != 0, "reject negative");
    ASSERT(cookbook_semver_parse("1.0.0-", &sv) != 0, "reject trailing dash");
}

static void test_semver_compare_detailed(void) {
    cookbook_semver a, b;

    /* equal versions */
    cookbook_semver_parse("1.2.3", &a);
    cookbook_semver_parse("1.2.3", &b);
    ASSERT(cookbook_semver_compare(&a, &b) == 0, "1.2.3 == 1.2.3");

    /* minor difference */
    cookbook_semver_parse("1.2.0", &a);
    cookbook_semver_parse("1.3.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.2.0 < 1.3.0");

    /* patch difference */
    cookbook_semver_parse("1.2.3", &a);
    cookbook_semver_parse("1.2.4", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "1.2.3 < 1.2.4");

    /* pre-release numeric ordering: 1 < 2 < 11 */
    cookbook_semver_parse("1.0.0-alpha.1", &a);
    cookbook_semver_parse("1.0.0-alpha.2", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "alpha.1 < alpha.2");

    /* pre-release: alpha < beta < rc */
    cookbook_semver_parse("1.0.0-beta", &a);
    cookbook_semver_parse("1.0.0-rc", &b);
    ASSERT(cookbook_semver_compare(&a, &b) < 0, "beta < rc");

    /* build metadata ignored in comparison */
    cookbook_semver_parse("1.0.0+build1", &a);
    cookbook_semver_parse("1.0.0+build2", &b);
    ASSERT(cookbook_semver_compare(&a, &b) == 0,
           "build metadata ignored in compare");

    /* symmetry */
    cookbook_semver_parse("2.0.0", &a);
    cookbook_semver_parse("1.0.0", &b);
    ASSERT(cookbook_semver_compare(&a, &b) > 0, "2.0.0 > 1.0.0");
}

static void test_range_exact(void) {
    cookbook_range r;
    cookbook_semver v;

    ASSERT(cookbook_range_parse("1.2.3", &r) == 0, "parse exact 1.2.3");
    ASSERT(r.type == COOKBOOK_RANGE_EXACT, "type is exact");

    cookbook_semver_parse("1.2.3", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "1.2.3 satisfies exact 1.2.3");

    cookbook_semver_parse("1.2.4", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.4 fails exact 1.2.3");

    cookbook_semver_parse("1.2.2", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "1.2.2 fails exact 1.2.3");
}

static void test_range_caret_zero(void) {
    cookbook_range r;
    cookbook_semver v;

    /* ^0.0.3 means >=0.0.3, <0.0.4 (only patch bumps allowed) */
    ASSERT(cookbook_range_parse("^0.0.3", &r) == 0, "parse ^0.0.3");
    cookbook_semver_parse("0.0.3", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1, "0.0.3 satisfies ^0.0.3");
    cookbook_semver_parse("0.0.4", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.0.4 fails ^0.0.3");
    cookbook_semver_parse("0.1.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0, "0.1.0 fails ^0.0.3");
}

static void test_range_bounded_inclusive(void) {
    cookbook_range r;
    cookbook_semver v;

    /* [1.0.0,2.0.0] — upper inclusive */
    ASSERT(cookbook_range_parse("[1.0.0,2.0.0]", &r) == 0,
           "parse [1.0.0,2.0.0]");

    cookbook_semver_parse("2.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1,
           "2.0.0 satisfies [1.0.0,2.0.0] (upper inclusive)");

    /* (1.0.0,2.0.0) — both exclusive */
    ASSERT(cookbook_range_parse("(1.0.0,2.0.0)", &r) == 0,
           "parse (1.0.0,2.0.0)");

    cookbook_semver_parse("1.0.0", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 0,
           "1.0.0 fails (1.0.0,2.0.0) (lower exclusive)");
    cookbook_semver_parse("1.0.1", &v);
    ASSERT(cookbook_range_satisfies(&r, &v) == 1,
           "1.0.1 satisfies (1.0.0,2.0.0)");
}

/* ---- additional DB tests ---- */

static void test_db_yanked_status(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.yank', 'alice')");

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.yank:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yank"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("deadbeef"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(512)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap, 9);

    /* yank it */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.yank"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0")
    };
    cookbook_db_status st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 3);
    ASSERT(st == COOKBOOK_DB_OK, "yank succeeds");

    /* verify yanked=1 */
    int count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE yanked = 1 "
        "AND group_id = ?1", yp, 1, count_cb, &count);
    ASSERT(count == 1, "yanked artifact found");

    /* verify excluded from published+non-yanked query */
    count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE yanked = 0 "
        "AND status = 'published' AND group_id = ?1", yp, 1,
        count_cb, &count);
    ASSERT(count == 0, "yanked artifact excluded from resolve");

    db->close(db);
}

static void test_db_null_params(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* NULL into NOT NULL column should fail with constraint error */
    cookbook_db_param gp[] = {
        COOKBOOK_P_TEXT("org.null"),
        COOKBOOK_P_NULL()
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO groups (group_id, owner_sub) VALUES (?1, ?2)",
        gp, 2);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "NULL into NOT NULL rejected");

    /* NULL in a nullable column (descriptor_sha256) should work */
    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.null', 'alice')");
    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.null:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.null"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabb"),
        COOKBOOK_P_NULL(),              /* descriptor_sha256 */
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(128)
    };
    st = db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " descriptor_sha256, snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)", ap, 10);
    ASSERT(st == COOKBOOK_DB_OK, "NULL in nullable column accepted");

    db->close(db);
}

static void test_db_pending_to_published(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.phase', 'alice')");

    /* insert as pending */
    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.phase:app:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.phase"),
        COOKBOOK_P_TEXT("app"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aabbccdd"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("pending"),
        COOKBOOK_P_INT(256)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap, 9);

    /* verify pending */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("org.phase") };
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE status = 'pending' "
        "AND group_id = ?1", qp, 1, count_cb, &count);
    ASSERT(count == 1, "pending artifact found");

    /* transition to published */
    cookbook_db_param up[] = {
        COOKBOOK_P_TEXT("org.phase:app:1.0.0:noarch")
    };
    db->exec_p(db,
        "UPDATE artifacts SET status = 'published' WHERE coord_id = ?1",
        up, 1);

    count = 0;
    db->query_p(db,
        "SELECT coord_id FROM artifacts WHERE status = 'published' "
        "AND group_id = ?1", qp, 1, count_cb, &count);
    ASSERT(count == 1, "published artifact found");

    db->close(db);
}

/* ---- additional store tests ---- */

static void test_store_overwrite(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store2";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open store for overwrite test");

    const char *key = "central/overwrite/test.txt";
    store->put(store, key, "first", 5);

    /* overwrite with different content */
    cookbook_store_status st = store->put(store, key, "second", 6);
    ASSERT(st == COOKBOOK_STORE_OK, "overwrite put succeeds");

    void *buf = NULL;
    size_t len = 0;
    store->get(store, key, &buf, &len);
    ASSERT(len == 6, "overwrite length correct");
    ASSERT(buf && memcmp(buf, "second", 6) == 0, "overwrite content correct");
    store->free_buf(buf);

    store->del(store, key);
    store->close(store);
}

static void test_store_large_value(void) {
    const char *dir = COOKBOOK_TEST_RESOURCES "/tmp_store3";
    cookbook_store *store = cookbook_store_open_fs(dir);
    ASSERT(store != NULL, "open store for large value test");

    /* 64KB value */
    size_t sz = 65536;
    char *big = malloc(sz);
    for (size_t i = 0; i < sz; i++) big[i] = (char)(i & 0x7F);

    const char *key = "central/large/blob.bin";
    cookbook_store_status st = store->put(store, key, big, sz);
    ASSERT(st == COOKBOOK_STORE_OK, "large put succeeds");

    void *buf = NULL;
    size_t len = 0;
    st = store->get(store, key, &buf, &len);
    ASSERT(st == COOKBOOK_STORE_OK, "large get succeeds");
    ASSERT(len == sz, "large roundtrip length");
    ASSERT(buf && memcmp(buf, big, sz) == 0, "large roundtrip content");
    store->free_buf(buf);

    store->del(store, key);
    free(big);
    store->close(store);
}

/* ---- additional auth tests ---- */

static void test_jwt_expired(void) {
    if (sodium_init() < -1) return;

    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    /* create a token with -1 second TTL — should be expired */
    char *token = cookbook_jwt_create("bob", "org.acme", -1, sk);
    ASSERT(token != NULL, "JWT create with negative TTL");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    /* either verify fails or claims.valid is 0 */
    ASSERT(rc != 0 || claims.valid == 0, "expired JWT rejected");
    free(token);
}

static void test_jwt_group_boundary(void) {
    if (sodium_init() < -1) return;

    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("alice", "org.acme.core", 3600, sk);
    ASSERT(token != NULL, "JWT create with dotted group");

    cookbook_jwt_claims claims;
    cookbook_jwt_verify(token, pk, &claims);

    /* exact match should work */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme.core") == 1,
           "exact group match");
    /* prefix should NOT match */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme") == 0,
           "prefix group does not match");
    /* substring should NOT match */
    ASSERT(cookbook_jwt_has_group(&claims, "org.acme.cor") == 0,
           "substring group does not match");
    /* empty group */
    ASSERT(cookbook_jwt_has_group(&claims, "") == 0,
           "empty group does not match");

    free(token);
}

static void test_base64url_edge_cases(void) {
    /* empty input */
    char encoded[64];
    size_t elen = cookbook_base64url_encode("", 0, encoded, sizeof(encoded));
    ASSERT(elen == 0, "base64url empty encode is empty");

    /* single byte */
    elen = cookbook_base64url_encode("A", 1, encoded, sizeof(encoded));
    ASSERT(elen > 0, "base64url single byte encodes");
    char decoded[64];
    size_t dlen = cookbook_base64url_decode(encoded, elen,
                                            decoded, sizeof(decoded));
    ASSERT(dlen == 1 && decoded[0] == 'A', "base64url single byte roundtrip");

    /* two bytes (tests padding=1 scenario) */
    elen = cookbook_base64url_encode("AB", 2, encoded, sizeof(encoded));
    dlen = cookbook_base64url_decode(encoded, elen, decoded, sizeof(decoded));
    ASSERT(dlen == 2 && decoded[0] == 'A' && decoded[1] == 'B',
           "base64url two byte roundtrip");

    /* binary data with all byte values 0-255 */
    unsigned char binary[256];
    for (int i = 0; i < 256; i++) binary[i] = (unsigned char)i;
    char big_encoded[512];
    elen = cookbook_base64url_encode(binary, 256, big_encoded,
                                     sizeof(big_encoded));
    ASSERT(elen > 0, "base64url binary encode");
    unsigned char big_decoded[256];
    dlen = cookbook_base64url_decode(big_encoded, elen, big_decoded,
                                     sizeof(big_decoded));
    ASSERT(dlen == 256, "base64url binary decode length");
    ASSERT(memcmp(binary, big_decoded, 256) == 0,
           "base64url binary roundtrip all bytes");
}

/* ---- additional ASCII validation tests ---- */

static void test_validate_ascii_boundaries(void) {
    /* 0x7F (DEL) is valid ASCII */
    ASSERT(cookbook_validate_ascii("\x7F", 1) == 0,
           "0x7F (DEL) is valid ASCII");

    /* 0x01 (SOH) — valid ASCII control char */
    ASSERT(cookbook_validate_ascii("\x01", 1) == 0,
           "0x01 is valid ASCII");

    /* 0x1F — last control char, valid */
    ASSERT(cookbook_validate_ascii("\x1F", 1) == 0,
           "0x1F is valid ASCII");

    /* 0x80 — first non-ASCII */
    ASSERT(cookbook_validate_ascii("\x80", 1) == 1,
           "0x80 is first non-ASCII");

    /* mixed valid + invalid at end */
    ASSERT(cookbook_validate_ascii("hello\x80", 6) == 6,
           "invalid at position 6");

    /* all printable ASCII */
    const char *printable =
        " !\"#$%&'()*+,-./0123456789:;<=>?@"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~";
    ASSERT(cookbook_validate_ascii(printable, strlen(printable)) == 0,
           "all printable ASCII valid");
}

/* ---- additional pasta-to-json tests ---- */

static void test_pasta_to_json_empty_containers(void) {
    PastaResult pr;

    /* empty map */
    const char *s1 = "{}";
    PastaValue *root = pasta_parse(s1, strlen(s1), &pr);
    ASSERT(root != NULL, "parse empty map");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strcmp(j, "{}") == 0, "empty map → {}");
        free(j);
        pasta_free(root);
    }

    /* empty array */
    const char *s2 = "{ items: [] }";
    root = pasta_parse(s2, strlen(s2), &pr);
    ASSERT(root != NULL, "parse empty array");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "empty array JSON");
        ASSERT(strstr(j, "[]") != NULL, "JSON has []");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_numbers(void) {
    PastaResult pr;

    /* integer */
    const char *s1 = "{ val: 0 }";
    PastaValue *root = pasta_parse(s1, strlen(s1), &pr);
    ASSERT(root != NULL, "parse zero");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, ":0}") != NULL, "zero in JSON");
        free(j);
        pasta_free(root);
    }

    /* negative */
    const char *s2 = "{ val: -42 }";
    root = pasta_parse(s2, strlen(s2), &pr);
    ASSERT(root != NULL, "parse negative");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, "-42") != NULL, "negative in JSON");
        free(j);
        pasta_free(root);
    }

    /* float */
    const char *s3 = "{ val: 3.14 }";
    root = pasta_parse(s3, strlen(s3), &pr);
    ASSERT(root != NULL, "parse float");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j && strstr(j, "3.14") != NULL, "float in JSON");
        free(j);
        pasta_free(root);
    }
}

static void test_pasta_to_json_deeply_nested(void) {
    PastaResult pr;
    const char *src =
        "{ a: { b: { c: { d: [1, 2, { e: true }] } } } }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse deeply nested");
    if (root) {
        char *j = cookbook_pasta_to_json(root);
        ASSERT(j != NULL, "deeply nested JSON");
        ASSERT(strstr(j, "\"a\"") != NULL, "has key a");
        ASSERT(strstr(j, "\"e\"") != NULL, "has key e");
        ASSERT(strstr(j, "true") != NULL, "has true at depth");
        free(j);
        pasta_free(root);
    }
}

/* ---- additional sorted write tests ---- */

static void test_pasta_sorted_nested(void) {
    PastaResult pr;
    const char *src =
        "{ z_outer: { z_inner: 1, a_inner: 2 }, a_outer: 3 }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse nested unsorted");
    if (root) {
        char *sorted = pasta_write(root, PASTA_COMPACT | PASTA_SORTED);
        ASSERT(sorted != NULL, "nested sorted write");
        if (sorted) {
            /* outer keys sorted: a_outer before z_outer */
            char *pa = strstr(sorted, "a_outer");
            char *pz = strstr(sorted, "z_outer");
            ASSERT(pa && pz && pa < pz,
                   "outer keys sorted: a_outer < z_outer");
            /* inner keys sorted: a_inner before z_inner */
            char *pai = strstr(sorted, "a_inner");
            char *pzi = strstr(sorted, "z_inner");
            ASSERT(pai && pzi && pai < pzi,
                   "inner keys sorted: a_inner < z_inner");
            free(sorted);
        }
        pasta_free(root);
    }
}

static void test_pasta_sorted_write(void) {
    PastaResult pr;
    const char *src = "{ zebra: 1, apple: 2, mango: 3 }";
    PastaValue *root = pasta_parse(src, strlen(src), &pr);
    ASSERT(root != NULL, "parse unsorted Pasta");
    if (root) {
        char *sorted = pasta_write(root, PASTA_COMPACT | PASTA_SORTED);
        ASSERT(sorted != NULL, "sorted compact write");
        if (sorted) {
            /* apple should come before mango, mango before zebra */
            char *pa = strstr(sorted, "apple");
            char *pm = strstr(sorted, "mango");
            char *pz = strstr(sorted, "zebra");
            ASSERT(pa && pm && pz, "all keys present in sorted output");
            ASSERT(pa < pm && pm < pz,
                   "keys in lexicographic order: apple < mango < zebra");
            free(sorted);
        }
        pasta_free(root);
    }
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
    test_semver_parse_edge_cases();
    test_semver_compare_detailed();
    test_range_exact();
    test_range_caret_zero();
    test_range_bounded_inclusive();
    test_db_yanked_status();
    test_db_null_params();
    test_db_pending_to_published();
    test_store_overwrite();
    test_store_large_value();
    test_jwt_expired();
    test_jwt_group_boundary();
    test_base64url_edge_cases();
    test_validate_ascii();
    test_validate_ascii_boundaries();
    test_pasta_to_json_primitives();
    test_pasta_to_json_nested();
    test_pasta_to_json_escaping();
    test_pasta_to_json_empty_containers();
    test_pasta_to_json_numbers();
    test_pasta_to_json_deeply_nested();
    test_pasta_sorted_write();
    test_pasta_sorted_nested();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
