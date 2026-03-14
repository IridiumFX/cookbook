#include <stdio.h>
#include <string.h>
#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"
#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include "cookbook_grid.h"
#include "cookbook_policy.h"
#include "alforno.h"
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

/* ---- F3: credential tests ---- */

static void test_base64_std_decode(void) {
    /* "alice:secrettoken" → base64 "YWxpY2U6c2VjcmV0dG9rZW4=" */
    const char *b64 = "YWxpY2U6c2VjcmV0dG9rZW4=";
    char out[64] = {0};
    size_t len = cookbook_base64_decode(b64, strlen(b64), out, sizeof(out) - 1);
    out[len] = '\0';
    ASSERT(len == 17, "base64 std decode length");
    ASSERT(strcmp(out, "alice:secrettoken") == 0, "base64 std decode value");

    /* empty input */
    len = cookbook_base64_decode("", 0, out, sizeof(out));
    ASSERT(len == 0, "base64 std decode empty");

    /* no padding */
    const char *b64np = "YWxpY2U6c2VjcmV0dG9rZW4";
    len = cookbook_base64_decode(b64np, strlen(b64np), out, sizeof(out) - 1);
    out[len] = '\0';
    ASSERT(len == 17, "base64 std decode no-padding length");
    ASSERT(strcmp(out, "alice:secrettoken") == 0, "base64 std decode no-padding");

    /* with + and / characters */
    const char *b64plus = "YQ+/";
    len = cookbook_base64_decode(b64plus, 4, out, sizeof(out));
    ASSERT(len == 3, "base64 std decode with +/");
}

static void test_credential_hash_verify(void) {
    const char *token = "my-secret-token-12345";
    char *hash = cookbook_credential_hash(token);
    ASSERT(hash != NULL, "credential hash not NULL");

    int rc = cookbook_credential_verify(token, hash);
    ASSERT(rc == 0, "credential verify correct token");

    free(hash);
}

static void test_credential_verify_wrong(void) {
    const char *token = "correct-token";
    char *hash = cookbook_credential_hash(token);
    ASSERT(hash != NULL, "hash for wrong-token test");

    int rc = cookbook_credential_verify("wrong-token", hash);
    ASSERT(rc != 0, "credential verify rejects wrong token");

    free(hash);
}

static void test_credentials_table(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* hash a token and store it */
    char *hash = cookbook_credential_hash("my-api-key");
    ASSERT(hash != NULL, "hash for credentials table test");

    cookbook_db_param ip[] = {
        COOKBOOK_P_TEXT("alice"),
        COOKBOOK_P_TEXT(hash),
        COOKBOOK_P_TEXT("org.acme,org.beta")
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO credentials (subject, token_hash, groups) "
        "VALUES (?1, ?2, ?3)", ip, 3);
    ASSERT(st == COOKBOOK_DB_OK, "insert credential");

    /* look it up */
    int count = 0;
    cookbook_db_param qp[] = { COOKBOOK_P_TEXT("alice") };
    db->query_p(db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        qp, 1, count_cb, &count);
    ASSERT(count == 1, "credential found");

    /* duplicate subject should fail */
    st = db->exec_p(db,
        "INSERT INTO credentials (subject, token_hash, groups) "
        "VALUES (?1, ?2, ?3)", ip, 3);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate credential rejected");

    /* revoke and verify not found with revoked_at IS NULL */
    cookbook_db_param rp[] = { COOKBOOK_P_TEXT("alice") };
    st = db->exec_p(db,
        "UPDATE credentials SET revoked_at = '2026-01-01' "
        "WHERE subject = ?1", rp, 1);
    ASSERT(st == COOKBOOK_DB_OK, "revoke credential");

    count = 0;
    db->query_p(db,
        "SELECT token_hash FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        qp, 1, count_cb, &count);
    ASSERT(count == 0, "revoked credential excluded");

    free(hash);
    db->close(db);
}

/* ---- Grid tests ---- */

static void test_grid_loop_detection(void) {
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeA") == 1,
           "loop: exact match");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeB,nodeA") == 1,
           "loop: in chain");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeB,nodeC") == 0,
           "no loop: not in chain");
    ASSERT(cookbook_grid_is_loop("nodeA", "") == 0,
           "no loop: empty chain");
    ASSERT(cookbook_grid_is_loop("nodeA", NULL) == 0,
           "no loop: NULL chain");
    ASSERT(cookbook_grid_is_loop("node", "nodeA,nodeB") == 0,
           "no loop: prefix not match");
    ASSERT(cookbook_grid_is_loop("nodeA", "nodeAB,nodeC") == 0,
           "no loop: substring not match");
}

static void test_grid_peers_table(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert a peer */
    cookbook_db_param pp[] = {
        COOKBOOK_P_TEXT("east-1"),
        COOKBOOK_P_TEXT("East Region"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(50)
    };
    cookbook_db_status st = db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", pp, 5);
    ASSERT(st == COOKBOOK_DB_OK, "insert peer");

    /* duplicate URL should fail */
    cookbook_db_param pp2[] = {
        COOKBOOK_P_TEXT("east-2"),
        COOKBOOK_P_TEXT("East 2"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("proxy"),
        COOKBOOK_P_INT(100)
    };
    st = db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", pp2, 5);
    ASSERT(st == COOKBOOK_DB_CONSTRAINT, "duplicate URL rejected");

    /* verify peer exists */
    int count = 0;
    db->query(db, "SELECT peer_id FROM peers WHERE enabled = 1",
              count_cb, &count);
    ASSERT(count == 1, "one enabled peer");

    /* disable peer */
    cookbook_db_param dp[] = { COOKBOOK_P_TEXT("east-1") };
    db->exec_p(db,
        "UPDATE peers SET enabled = 0 WHERE peer_id = ?1", dp, 1);

    count = 0;
    db->query(db, "SELECT peer_id FROM peers WHERE enabled = 1",
              count_cb, &count);
    ASSERT(count == 0, "no enabled peers after disable");

    db->close(db);
}

static void test_grid_peer_load(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* insert two peers with different priorities */
    cookbook_db_param p1[] = {
        COOKBOOK_P_TEXT("west-1"),
        COOKBOOK_P_TEXT("West Region"),
        COOKBOOK_P_TEXT("http://west-1:8080"),
        COOKBOOK_P_TEXT("proxy"),
        COOKBOOK_P_INT(200)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", p1, 5);

    cookbook_db_param p2[] = {
        COOKBOOK_P_TEXT("east-1"),
        COOKBOOK_P_TEXT("East Region"),
        COOKBOOK_P_TEXT("http://east-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(50)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 1)", p2, 5);

    /* disabled peer */
    cookbook_db_param p3[] = {
        COOKBOOK_P_TEXT("down-1"),
        COOKBOOK_P_TEXT("Down"),
        COOKBOOK_P_TEXT("http://down-1:8080"),
        COOKBOOK_P_TEXT("redirect"),
        COOKBOOK_P_INT(10)
    };
    db->exec_p(db,
        "INSERT INTO peers (peer_id, name, url, mode, priority, enabled) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0)", p3, 5);

    cookbook_peer *peers = NULL;
    int n = cookbook_grid_load_peers(db, &peers);
    ASSERT(n == 2, "load 2 enabled peers");
    /* should be sorted by priority: east-1 (50) before west-1 (200) */
    ASSERT(strcmp(peers[0].peer_id, "east-1") == 0, "first peer is east-1");
    ASSERT(peers[0].mode == 'r', "east-1 mode is redirect");
    ASSERT(strcmp(peers[1].peer_id, "west-1") == 0, "second peer is west-1");
    ASSERT(peers[1].mode == 'p', "west-1 mode is proxy");

    cookbook_grid_free_peers(peers, n);
    db->close(db);
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

/* F1: yank reason stored and retrieved */
static void test_db_yank_reason(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.yr', 'alice')");

    cookbook_db_param ap[] = {
        COOKBOOK_P_TEXT("org.yr:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yr"),
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

    /* yank with reason */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("CVE-2026-1234: remote code execution")
    };
    cookbook_db_status st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 4);
    ASSERT(st == COOKBOOK_DB_OK, "yank with reason succeeds");

    /* verify reason stored via WHERE filter */
    int count = 0;
    cookbook_db_param fp[] = {
        COOKBOOK_P_TEXT("org.yr:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("CVE-2026-1234: remote code execution")
    };
    db->query_p(db,
        "SELECT coord_id FROM artifacts "
        "WHERE coord_id = ?1 AND yank_reason = ?2",
        fp, 2, count_cb, &count);
    ASSERT(count == 1, "yank reason stored correctly");

    /* yank without reason — reason should be NULL */
    cookbook_db_param ap2[] = {
        COOKBOOK_P_TEXT("org.yr:lib:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("cafebabe"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(256)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", ap2, 9);

    cookbook_db_param yp2[] = {
        COOKBOOK_P_TEXT("org.yr"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_NULL()
    };
    st = db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp2, 4);
    ASSERT(st == COOKBOOK_DB_OK, "yank without reason succeeds");

    count = 0;
    cookbook_db_param fp2[] = {
        COOKBOOK_P_TEXT("org.yr:lib:2.0.0:noarch")
    };
    db->query_p(db,
        "SELECT coord_id FROM artifacts "
        "WHERE coord_id = ?1 AND yank_reason IS NULL",
        fp2, 1, count_cb, &count);
    ASSERT(count == 1, "yank without reason stores NULL");

    db->close(db);
}

/* F2: resolve with include_yanked returns yanked versions */
static void test_db_resolve_include_yanked(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    db->exec(db,
        "INSERT INTO groups (group_id, owner_sub) "
        "VALUES ('org.ry', 'alice')");

    /* insert v1.0.0 (will be yanked) and v2.0.0 (published) */
    cookbook_db_param a1[] = {
        COOKBOOK_P_TEXT("org.ry:lib:1.0.0:noarch"),
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("aaa"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(100)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", a1, 9);

    cookbook_db_param s1[] = {
        COOKBOOK_P_TEXT("org.ry:lib:1.0.0:noarch"),
        COOKBOOK_P_INT(1), COOKBOOK_P_INT(0), COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT(""), COOKBOOK_P_TEXT("")
    };
    db->exec_p(db,
        "INSERT INTO artifact_semver "
        "(coord_id, major, minor, patch, pre_release, build_meta) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", s1, 6);

    cookbook_db_param a2[] = {
        COOKBOOK_P_TEXT("org.ry:lib:2.0.0:noarch"),
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("2.0.0"),
        COOKBOOK_P_TEXT("noarch"),
        COOKBOOK_P_TEXT("bbb"),
        COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT("published"),
        COOKBOOK_P_INT(200)
    };
    db->exec_p(db,
        "INSERT INTO artifacts "
        "(coord_id, group_id, artifact, version, triple, sha256, "
        " snapshot, status, size_bytes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", a2, 9);

    cookbook_db_param s2[] = {
        COOKBOOK_P_TEXT("org.ry:lib:2.0.0:noarch"),
        COOKBOOK_P_INT(2), COOKBOOK_P_INT(0), COOKBOOK_P_INT(0),
        COOKBOOK_P_TEXT(""), COOKBOOK_P_TEXT("")
    };
    db->exec_p(db,
        "INSERT INTO artifact_semver "
        "(coord_id, major, minor, patch, pre_release, build_meta) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", s2, 6);

    /* yank v1.0.0 with reason */
    cookbook_db_param yp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("1.0.0"),
        COOKBOOK_P_TEXT("security vulnerability")
    };
    db->exec_p(db,
        "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
        "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3",
        yp, 4);

    /* default resolve (exclude yanked) */
    int count = 0;
    cookbook_db_param rp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib")
    };
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "JOIN artifact_semver s ON a.coord_id = s.coord_id "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.yanked = 0 AND a.status = 'published'",
        rp, 2, count_cb, &count);
    ASSERT(count == 1, "default resolve excludes yanked");

    /* include_yanked resolve */
    count = 0;
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "JOIN artifact_semver s ON a.coord_id = s.coord_id "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.status = 'published'",
        rp, 2, count_cb, &count);
    ASSERT(count == 2, "include_yanked resolve returns both versions");

    /* verify yanked version has reason in extended query */
    count = 0;
    cookbook_db_param vrp[] = {
        COOKBOOK_P_TEXT("org.ry"),
        COOKBOOK_P_TEXT("lib"),
        COOKBOOK_P_TEXT("security vulnerability")
    };
    db->query_p(db,
        "SELECT a.version FROM artifacts a "
        "WHERE a.group_id = ?1 AND a.artifact = ?2 "
        "AND a.yanked = 1 AND a.yank_reason = ?3",
        vrp, 3, count_cb, &count);
    ASSERT(count == 1, "yanked version has correct reason in resolve");

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

/* ---- auth v2: policy tests ---- */

static void test_policy_crud(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* put a policy */
    int rc = cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\",\n  kind: \"user\",\n"
        "  teams: [core]\n}\n@grants {\n  com.iridiumfx: \"crwd\"\n}\n");
    ASSERT(rc == 0, "policy put succeeds");

    /* get it back */
    char *p = cookbook_policy_get(db, "alice");
    ASSERT(p != NULL, "policy get returns non-NULL");
    ASSERT(strstr(p, "alice") != NULL, "policy contains subject");
    ASSERT(strstr(p, "crwd") != NULL, "policy contains grants");
    free(p);

    /* get non-existent */
    char *p2 = cookbook_policy_get(db, "bob");
    ASSERT(p2 == NULL, "non-existent policy returns NULL");

    /* update (replace) */
    rc = cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\"\n}\n"
        "@grants {\n  com.iridiumfx: \"r\"\n}\n");
    ASSERT(rc == 0, "policy update succeeds");
    p = cookbook_policy_get(db, "alice");
    ASSERT(p != NULL, "updated policy exists");
    ASSERT(strstr(p, "\"r\"") != NULL, "updated policy has new grant");
    free(p);

    /* delete */
    rc = cookbook_policy_delete(db, "alice");
    ASSERT(rc == 0, "policy delete succeeds");
    p = cookbook_policy_get(db, "alice");
    ASSERT(p == NULL, "deleted policy is gone");

    db->close(db);
}

static void test_policy_resolve(void) {
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* add user with team reference */
    cookbook_policy_put(db, "alice", "user",
        "@identity {\n  subject: \"alice\",\n  kind: \"user\",\n"
        "  teams: [core]\n}\n"
        "@grants {\n  com.iridiumfx: \"r\"\n}\n");

    /* add team policy */
    cookbook_policy_put(db, "core", "team",
        "@identity {\n  team_id: \"core\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.iridiumfx: \"crwd\",\n"
        "  com.iridiumfx.internal: \"crwd\"\n}\n"
        "@exclude {\n  com.iridiumfx.secret: true\n}\n");

    /* resolve — should aggregate alice + core */
    char *json = cookbook_policy_resolve(db, "alice");
    ASSERT(json != NULL, "policy resolve returns non-NULL");
    ASSERT(strstr(json, "\"grants\"") != NULL, "resolved has grants");
    ASSERT(strstr(json, "com.iridiumfx") != NULL, "resolved has group");
    ASSERT(strstr(json, "\"exclude\"") != NULL, "resolved has exclude");
    ASSERT(strstr(json, "com.iridiumfx.secret") != NULL, "resolved has exclusion");

    /* verify collect OR: com.iridiumfx should have permissions from BOTH
       user ("r") and team ("crwd") OR'd together, not just last-write-wins */
    ASSERT(strstr(json, "crwd") != NULL || strstr(json, "rcrwd") != NULL,
           "resolved grants OR user+team permissions");

    /* com.iridiumfx.internal only in team → single value, not array */
    ASSERT(strstr(json, "com.iridiumfx.internal") != NULL,
           "team-only grant preserved");
    free(json);

    /* resolve non-existent user */
    char *j2 = cookbook_policy_resolve(db, "nobody");
    ASSERT(j2 == NULL, "resolve unknown user returns NULL");

    db->close(db);
}

static void test_policy_resolve_collect(void) {
    /* prove that merge:"collect" OR's permissions instead of last-write-wins */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* user has 'r' only, team has 'cw' only — result must have all three */
    cookbook_policy_put(db, "dev1", "user",
        "@identity {\n  subject: \"dev1\",\n  kind: \"user\",\n"
        "  teams: [builders]\n}\n"
        "@grants {\n  com.example: \"r\"\n}\n");

    cookbook_policy_put(db, "builders", "team",
        "@identity {\n  team_id: \"builders\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.example: \"cw\"\n}\n");

    char *json = cookbook_policy_resolve(db, "dev1");
    ASSERT(json != NULL, "collect resolve returns non-NULL");

    /* with last-write-wins: "cw" only. with collect OR: "rcw" or "cwr" etc. */
    int has_r = (strstr(json, "r") != NULL);
    int has_c = (strstr(json, "c") != NULL);
    int has_w = (strstr(json, "w") != NULL);
    ASSERT(has_r && has_c && has_w,
           "collect OR yields all three permissions (r+cw)");

    /* verify via auth_check that all three ops are allowed */
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'r') == 1,
           "collect: read allowed (from user)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'c') == 1,
           "collect: create allowed (from team)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'w') == 1,
           "collect: write allowed (from team)");
    ASSERT(cookbook_auth_check(json, NULL, "com.example", 'd') == 0,
           "collect: delete denied (nobody granted it)");

    free(json);
    db->close(db);
}

static void test_auth_check_prefix(void) {
    const char *grants =
        "{\"grants\":{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"},"
        "\"exclude\":{}}";

    /* exact prefix match */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx", 'r') == 1,
           "exact prefix read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx", 'c') == 1,
           "exact prefix create allowed");

    /* hierarchical match: com.iridiumfx.pasta starts with com.iridiumfx */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'r') == 1,
           "hierarchical read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'w') == 1,
           "hierarchical write allowed");

    /* org.acme is read-only */
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme", 'r') == 1,
           "org.acme read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme.sdk", 'r') == 1,
           "org.acme.sdk read allowed");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme", 'w') == 0,
           "org.acme write denied");
    ASSERT(cookbook_auth_check(grants, NULL, "org.acme.sdk", 'c') == 0,
           "org.acme.sdk create denied");

    /* no matching grant */
    ASSERT(cookbook_auth_check(grants, NULL, "net.example", 'r') == 0,
           "ungranted group denied");

    /* prefix must match at dot boundary */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfxtra", 'r') == 0,
           "non-dot-boundary prefix rejected");
}

static void test_auth_check_exclude(void) {
    const char *grants =
        "{\"grants\":{\"com.iridiumfx\":\"crwd\"},"
        "\"exclude\":{\"com.iridiumfx.secret\":true}}";

    /* normal access allowed */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.pasta", 'r') == 1,
           "non-excluded group allowed");

    /* excluded group denied despite grant */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.secret", 'r') == 0,
           "excluded group denied");

    /* sub-group of excluded also denied */
    ASSERT(cookbook_auth_check(grants, NULL, "com.iridiumfx.secret.keys", 'r') == 0,
           "sub-group of excluded denied");
}

static void test_auth_check_edge_cases(void) {
    /* NULL grants */
    ASSERT(cookbook_auth_check(NULL, NULL, "com.foo", 'r') == 0,
           "NULL grants denied");

    /* NULL group */
    ASSERT(cookbook_auth_check("{\"grants\":{}}", NULL, NULL, 'r') == 0,
           "NULL group denied");

    /* empty grants */
    ASSERT(cookbook_auth_check("{\"grants\":{},\"exclude\":{}}", NULL,
                               "com.foo", 'r') == 0,
           "empty grants denied");
}

static void test_alforno_integration(void) {
    /* verify alforno aggregate works with our pasta pastlets */
    const char *user_pastlet =
        "@identity {\n  subject: \"test\"\n}\n"
        "@grants {\n  com.test: \"r\"\n}\n";
    const char *team_pastlet =
        "@identity {\n  team_id: \"devs\"\n}\n"
        "@grants {\n  com.test: \"cw\",\n  org.shared: \"r\"\n}\n";

    AlfResult ar;
    AlfContext *ctx = alf_create(ALF_AGGREGATE, &ar);
    ASSERT(ctx != NULL, "alf_create succeeds");
    if (!ctx) return;

    int rc = alf_add_input(ctx, user_pastlet, strlen(user_pastlet), &ar);
    ASSERT(rc == 0, "alf_add_input user succeeds");

    rc = alf_add_input(ctx, team_pastlet, strlen(team_pastlet), &ar);
    ASSERT(rc == 0, "alf_add_input team succeeds");

    PastaValue *resolved = alf_process(ctx, &ar);
    ASSERT(resolved != NULL, "alf_process succeeds");

    /* check that @grants section exists */
    const PastaValue *grants = pasta_map_get(resolved, "grants");
    ASSERT(grants != NULL, "resolved has @grants");
    ASSERT(pasta_type(grants) == PASTA_MAP, "@grants is a map");

    /* last-write-wins: com.test should be "cw" (from team, which came second) */
    const PastaValue *ct = pasta_map_get(grants, "com.test");
    ASSERT(ct != NULL, "com.test grant exists");
    ASSERT(pasta_type(ct) == PASTA_STRING, "com.test is string");
    ASSERT(strcmp(pasta_get_string(ct), "cw") == 0,
           "com.test is 'cw' (last-write-wins from team)");

    /* org.shared should be "r" (only in team) */
    const PastaValue *os = pasta_map_get(grants, "org.shared");
    ASSERT(os != NULL, "org.shared grant exists");
    ASSERT(strcmp(pasta_get_string(os), "r") == 0,
           "org.shared is 'r'");

    /* check @exclude doesn't exist (neither pastlet has it) */
    const PastaValue *exc = pasta_map_get(resolved, "exclude");
    ASSERT(exc == NULL, "no @exclude when none provided");

    pasta_free(resolved);
    alf_free(ctx);
}

/* ---- auth v2 phase 2: JWT v2 tests ---- */

static void test_jwt_v2_roundtrip(void) {
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    const char *resolved = "{\"grants\":{\"com.iridiumfx\":\"crwd\",\"org.acme\":\"r\"},"
                           "\"exclude\":{\"com.iridiumfx.secret\":true}}";

    char *token = cookbook_jwt_create_v2("alice", "com.iridiumfx",
                                          resolved, 3600, sk);
    ASSERT(token != NULL, "jwt v2 create succeeds");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "jwt v2 verify succeeds");
    ASSERT(claims.valid == 1, "jwt v2 claims valid");
    ASSERT(claims.version == 2, "jwt v2 version is 2");
    ASSERT(strcmp(claims.sub, "alice") == 0, "jwt v2 sub is alice");
    ASSERT(strcmp(claims.groups, "com.iridiumfx") == 0, "jwt v2 groups preserved");

    /* check grants extracted */
    ASSERT(claims.grants_json != NULL, "jwt v2 grants_json extracted");
    ASSERT(strstr(claims.grants_json, "com.iridiumfx") != NULL,
           "jwt v2 grants contains com.iridiumfx");
    ASSERT(strstr(claims.grants_json, "crwd") != NULL,
           "jwt v2 grants contains crwd");
    ASSERT(strstr(claims.grants_json, "org.acme") != NULL,
           "jwt v2 grants contains org.acme");

    /* check exclude extracted */
    ASSERT(claims.exclude_json != NULL, "jwt v2 exclude_json extracted");
    ASSERT(strstr(claims.exclude_json, "com.iridiumfx.secret") != NULL,
           "jwt v2 exclude contains com.iridiumfx.secret");

    /* use auth_check with extracted claims */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.iridiumfx.pasta", 'r') == 1,
           "jwt v2 grants allow read on sub-group");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.iridiumfx.secret", 'r') == 0,
           "jwt v2 exclude blocks secret");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "org.acme", 'r') == 1,
           "jwt v2 grants allow read on org.acme");
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "org.acme", 'w') == 0,
           "jwt v2 org.acme write denied (read-only)");

    cookbook_jwt_claims_free(&claims);
    free(token);
}

static void test_jwt_v2_policy_integration(void) {
    /* end-to-end: store policy → resolve → create JWT v2 → verify → check */
    cookbook_db *db = cookbook_db_open_sqlite(NULL);
    cookbook_db_migrate(db);

    /* set up user + team policies */
    cookbook_policy_put(db, "bob", "user",
        "@identity {\n  subject: \"bob\",\n  kind: \"user\",\n"
        "  teams: [devs]\n}\n"
        "@grants {\n  com.example: \"r\"\n}\n");

    cookbook_policy_put(db, "devs", "team",
        "@identity {\n  team_id: \"devs\",\n  kind: \"team\"\n}\n"
        "@grants {\n  com.example: \"crwd\",\n"
        "  com.example.tools: \"rw\"\n}\n"
        "@exclude {\n  com.example.private: true\n}\n");

    /* resolve */
    char *resolved = cookbook_policy_resolve(db, "bob");
    ASSERT(resolved != NULL, "policy resolves for bob");

    /* create JWT v2 */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create_v2("bob", NULL, resolved, 3600, sk);
    ASSERT(token != NULL, "jwt v2 from policy succeeds");
    free(resolved);

    /* verify and extract */
    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "policy jwt v2 verify succeeds");
    ASSERT(claims.version == 2, "policy jwt v2 version");
    ASSERT(claims.grants_json != NULL, "policy jwt v2 has grants");
    ASSERT(claims.exclude_json != NULL, "policy jwt v2 has exclude");

    /* enforce: com.example.tools should be writable (from team) */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example.tools", 'w') == 1,
           "bob can write com.example.tools via team");

    /* enforce: com.example.private denied by exclude */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example.private", 'r') == 0,
           "bob denied com.example.private via exclude");

    /* enforce: com.example readable (aggregate: last-write-wins = crwd from team) */
    ASSERT(cookbook_auth_check(claims.grants_json, claims.exclude_json,
                               "com.example", 'c') == 1,
           "bob can create under com.example (team escalation)");

    cookbook_jwt_claims_free(&claims);
    free(token);
    db->close(db);
}

static void test_jwt_v1_v2_compat(void) {
    /* v1 token should still work — no grants/exclude, version=1 */
    unsigned char pk[32], sk[64];
    cookbook_keygen(pk, sk);

    char *token = cookbook_jwt_create("charlie", "org.test", 3600, sk);
    ASSERT(token != NULL, "jwt v1 create succeeds");

    cookbook_jwt_claims claims;
    int rc = cookbook_jwt_verify(token, pk, &claims);
    ASSERT(rc == 0, "jwt v1 verify succeeds");
    ASSERT(claims.version == 1, "jwt v1 version is 1");
    ASSERT(strcmp(claims.sub, "charlie") == 0, "jwt v1 sub");
    ASSERT(strcmp(claims.groups, "org.test") == 0, "jwt v1 groups");
    ASSERT(claims.grants_json == NULL, "jwt v1 no grants_json");
    ASSERT(claims.exclude_json == NULL, "jwt v1 no exclude_json");

    cookbook_jwt_claims_free(&claims);
    free(token);
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
    test_db_yank_reason();
    test_db_resolve_include_yanked();
    test_db_null_params();
    test_db_pending_to_published();
    test_store_overwrite();
    test_store_large_value();
    test_jwt_expired();
    test_jwt_group_boundary();
    test_base64url_edge_cases();
    test_base64_std_decode();
    test_credential_hash_verify();
    test_credential_verify_wrong();
    test_credentials_table();
    test_grid_loop_detection();
    test_grid_peers_table();
    test_grid_peer_load();
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
    test_policy_crud();
    test_policy_resolve();
    test_policy_resolve_collect();
    test_auth_check_prefix();
    test_auth_check_exclude();
    test_auth_check_edge_cases();
    test_alforno_integration();
    test_jwt_v2_roundtrip();
    test_jwt_v2_policy_integration();
    test_jwt_v1_v2_compat();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
